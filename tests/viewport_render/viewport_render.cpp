// Opt-in graphics acceptance fixture. No GUI_App, device data, plugins or sockets.
#include "slic3r/GUI/ViewportLighting.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include <glad/gl.h>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/defs.h>
#include <wx/filefn.h>
#include <wx/gdicmn.h>
#include <wx/frame.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/string.h>
#if defined(__WXGTK__) && defined(wxHAS_EGL)
#include <EGL/egl.h>
#endif
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <stdexcept>
#include <exception>
#include <string>
#include <vector>

using Slic3r::Vec3f;
using Slic3r::Vec3d;
using Slic3r::Matrix4f;
using Slic3r::Matrix3f;
using Slic3r::BoundingBoxf3;
using Slic3r::GUI::fit_workspace_shadow;

namespace {
std::string source(const std::string& file, const std::string& define)
{
    std::ifstream stream(std::string(PORCA_RENDER_RESOURCES) + "/shaders/140/" + file);
    if (!stream)
        throw std::runtime_error("Missing shader " + file);
    std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (!define.empty())
        text.insert(text.find('\n') + 1, "#define " + define + "\n");
    return text;
}

GLuint program(const std::string& name, const std::string& define = {})
{
    const GLuint result = glCreateProgram();
    std::array<GLuint, 2> shaders{};
    for (int i = 0; i < 2; ++i) {
        shaders[i] = glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const std::string text = source(name + (i == 0 ? ".vs" : ".fs"), define);
        const char* pointer = text.c_str();
        glShaderSource(shaders[i], 1, &pointer, nullptr);
        glCompileShader(shaders[i]);
        GLint success = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &success);
        if (!success) {
            std::array<char, 8192> log{};
            glGetShaderInfoLog(shaders[i], GLsizei(log.size()), nullptr, log.data());
            throw std::runtime_error(name + ": " + log.data());
        }
        glAttachShader(result, shaders[i]);
    }
    glLinkProgram(result);
    for (GLuint shader : shaders)
        glDeleteShader(shader);
    GLint success = 0;
    glGetProgramiv(result, GL_LINK_STATUS, &success);
    if (!success) {
        std::array<char, 8192> log{};
        glGetProgramInfoLog(result, GLsizei(log.size()), nullptr, log.data());
        throw std::runtime_error(name + ": " + log.data());
    }
    return result;
}

void matrix(GLuint shader, const char* name, const Matrix4f& value)
{
    glUniformMatrix4fv(glGetUniformLocation(shader, name), 1, GL_FALSE, value.data());
}

void integer(GLuint shader, const char* name, int value)
{
    glUniform1i(glGetUniformLocation(shader, name), value);
}

void scalar(GLuint shader, const char* name, float value)
{
    glUniform1f(glGetUniformLocation(shader, name), value);
}

struct Mesh
{
    std::vector<float> vertices;
    void vertex(const Vec3f& position, const Vec3f& normal)
    {
        vertices.insert(vertices.end(), position.data(), position.data() + 3);
        vertices.insert(vertices.end(), normal.data(), normal.data() + 3);
    }
    void triangle(const Vec3f& a, const Vec3f& b, const Vec3f& c)
    {
        const Vec3f normal = (b - a).cross(c - a).normalized();
        vertex(a, normal); vertex(b, normal); vertex(c, normal);
    }
};

Mesh sphere()
{
    Mesh mesh;
    auto point = [](int row, int column) {
        const float latitude = float(row) * 3.14159265f / 16;
        const float longitude = float(column) * 6.2831853f / 32;
        return Vec3f(std::sin(latitude) * std::cos(longitude),
                     std::sin(latitude) * std::sin(longitude), std::cos(latitude));
    };
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 32; ++col) {
            for (const Vec3f& n : {point(row, col), point(row + 1, col), point(row + 1, col + 1),
                                  point(row, col), point(row + 1, col + 1), point(row, col + 1)})
                mesh.vertex(n * 20 + Vec3f(-55, 0, 20), n);
        }
    return mesh;
}

Mesh cube()
{
    Mesh mesh;
    const std::array<Vec3f, 8> corners{Vec3f(35,-20,0), Vec3f(75,-20,0), Vec3f(75,20,0), Vec3f(35,20,0),
        Vec3f(35,-20,40), Vec3f(75,-20,40), Vec3f(75,20,40), Vec3f(35,20,40)};
    for (const auto& face : std::array<std::array<int, 4>, 6>{{{0,3,2,1},{4,5,6,7},{0,1,5,4},{3,7,6,2},{0,4,7,3},{1,2,6,5}}}) {
        mesh.triangle(corners[face[0]], corners[face[1]], corners[face[2]]);
        mesh.triangle(corners[face[0]], corners[face[2]], corners[face[3]]);
    }
    return mesh;
}

void draw(GLuint shader, const Mesh& mesh, GLuint vao, GLuint buffer)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.vertices.size() * sizeof(float)), mesh.vertices.data(), GL_STATIC_DRAW);
    for (const auto& attribute : std::array<std::pair<const char*, int>, 2>{{{"v_position",0},{"v_normal",3}}}) {
        const GLint location = glGetAttribLocation(shader, attribute.first);
        if (location >= 0) {
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                reinterpret_cast<void*>(attribute.second * sizeof(float)));
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(mesh.vertices.size() / 6));
    glBindVertexArray(0);
}

GLuint texture(const char* filename)
{
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8((std::string(PORCA_RENDER_RESOURCES) + "/lighting/" + filename).c_str()), wxBITMAP_TYPE_PNG))
        throw std::runtime_error("Missing studio resource");
    std::vector<unsigned char> pixels(size_t(image.GetWidth()) * image.GetHeight() * 4);
    for (size_t i = 0; i < pixels.size() / 4; ++i) {
        for (int c = 0; c < 3; ++c) pixels[i * 4 + c] = image.GetData()[i * 3 + c];
        pixels[i * 4 + 3] = image.HasAlpha() ? image.GetAlpha()[i] : 255;
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.GetWidth(), image.GetHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}
} // namespace

class ViewportRenderApp : public wxApp
{
    wxGLCanvas* m_canvas{nullptr};
    wxGLContext* m_context{nullptr};
    wxString m_output;
    int m_result{0};
    bool m_rendered{false};
public:
    bool OnInit() override
    {
        if (argc != 2) {
            std::cerr << "Usage: porca_viewport_render_tests OUTPUT_DIRECTORY\n";
            return false;
        }
        m_output = argv[1];
        wxInitAllImageHandlers();
        auto* frame = new wxFrame(nullptr, wxID_ANY, "Porca viewport fixture");
        int attributes[] = {WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_MIN_RED,8,WX_GL_MIN_GREEN,8,
            WX_GL_MIN_BLUE,8,WX_GL_MIN_ALPHA,8,WX_GL_DEPTH_SIZE,24,WX_GL_STENCIL_SIZE,8,0};
        m_canvas = new wxGLCanvas(frame, wxID_ANY, attributes);
        m_canvas->SetMinSize(wxSize(512, 512));
        m_canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_canvas, 1, wxEXPAND);
        frame->SetSizerAndFit(sizer);
        wxGLContextAttrs context_attributes;
        context_attributes.PlatformDefaults().CoreProfile().MajorVersion(3).MinorVersion(2).EndList();
        m_context = new wxGLContext(m_canvas, nullptr, &context_attributes);
        m_canvas->Bind(wxEVT_PAINT, [this, frame](wxPaintEvent&) {
            wxPaintDC dc(m_canvas);
            if (m_rendered || !m_canvas->IsShownOnScreen())
                return;
            m_rendered = true;
            try { render(); }
            catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                m_result = 1;
            }
            CallAfter([this, frame] {
                delete m_context;
                m_context = nullptr;
                frame->Destroy();
                ExitMainLoop();
            });
        });
        frame->Show();
        return true;
    }
    int OnRun() override { wxApp::OnRun(); return m_result; }

    void render()
    {
        if (!m_context->IsOK() || !m_canvas->IsShownOnScreen() || !m_canvas->SetCurrent(*m_context))
            throw std::runtime_error("No current graphics context");
        int loaded = 0;
#if defined(__WXGTK__) && defined(wxHAS_EGL)
        // Keep EGL procedure addresses alive, including on Wayland.
        loaded = gladLoadGL(reinterpret_cast<GLADloadfunc>(eglGetProcAddress));
#else
        loaded = gladLoaderLoadGL();
#endif
        if (loaded == 0) throw std::runtime_error("GL loader failed");
        const int width = int(m_canvas->GetClientSize().x * m_canvas->GetContentScaleFactor());
        const int height = int(m_canvas->GetClientSize().y * m_canvas->GetContentScaleFactor());
        glViewport(0, 0, width, height);
        GLuint vao = 0, buffer = 0;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &buffer);
        std::vector<GLuint> programs;
        programs.push_back(program("phong"));
        programs.push_back(program("phong", "PORCA_PLASTIC"));
        programs.push_back(program("flat", "PORCA_SHADOW_RECEIVER"));
        programs.push_back(program("hotbed", "PORCA_SHADOW_RECEIVER"));
        const GLuint caster = program("flat_clip");
        programs.push_back(caster);
        const Vec3d light = Vec3d(-.4,-.6,1).normalized();
        const auto fit = fit_workspace_shadow(BoundingBoxf3(Vec3d(-75,-20,0),Vec3d(75,20,40)),light,2048);
        if (!fit) throw std::runtime_error("Shadow fit failed");
        const Matrix4f light_vp = (fit->projection * fit->view).matrix().cast<float>();
        GLuint depth = 0, fbo = 0;
        glGenTextures(1,&depth); glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D,depth);
        glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,2048,2048,0,GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth,0);
        glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete depth framebuffer");
        glViewport(0,0,2048,2048); glEnable(GL_DEPTH_TEST); glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(caster); matrix(caster,"view_model_matrix",fit->view.matrix().cast<float>());
        matrix(caster,"projection_matrix",fit->projection.matrix().cast<float>());
        matrix(caster,"volume_world_matrix",Matrix4f::Identity());
        glUniform4f(glGetUniformLocation(caster,"clipping_plane"),0,0,0,1);
        glUniform2f(glGetUniformLocation(caster,"z_range"),-1000,1000);
        const Mesh round = sphere(), sharp = cube();
        draw(caster,round,vao,buffer); draw(caster,sharp,vao,buffer);
        glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,width,height);
        glActiveTexture(GL_TEXTURE2); const GLuint radiance = texture("studio-radiance.png");
        glActiveTexture(GL_TEXTURE3); const GLuint brdf = texture("studio-brdf.png");
        const Vec3f eye(190,-250,180), target(0,0,15);
        const Vec3f z = (eye-target).normalized(), x = Vec3f::UnitZ().cross(z).normalized(), y = z.cross(x);
        Matrix4f view = Matrix4f::Identity();
        view.block<1,3>(0,0)=x.transpose(); view.block<1,3>(1,0)=y.transpose(); view.block<1,3>(2,0)=z.transpose();
        view(0,3)=-x.dot(eye); view(1,3)=-y.dot(eye); view(2,3)=-z.dot(eye);
        Matrix4f projection = Matrix4f::Zero();
        projection(0,0)=2.5f * height / width; projection(1,1)=2.5f;
        projection(2,2)=-1001.0f/999.0f; projection(2,3)=-2000.0f/999.0f; projection(3,2)=-1;
        const Matrix3f rotation=view.block<3,3>(0,0);
        Mesh floor;
        floor.triangle(Vec3f(-95,-50,-3),Vec3f(95,-50,-3),Vec3f(95,50,-3));
        floor.triangle(Vec3f(-95,-50,-3),Vec3f(95,50,-3),Vec3f(-95,50,-3));
        if (!wxFileName::DirExists(m_output) &&
            !wxFileName::Mkdir(m_output, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
            throw std::runtime_error("Cannot create fixture output directory");
        for (int mode=0;mode<2;++mode) {
            glClearColor(.91f,.925f,.937f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            for (const auto& item : std::array<std::pair<GLuint,const Mesh*>,3>{{{programs[2],&floor},{programs[mode],&round},{programs[mode],&sharp}}}) {
                const GLuint shader=item.first; glUseProgram(shader);
                matrix(shader,"view_model_matrix",view); matrix(shader,"projection_matrix",projection);
                matrix(shader,"volume_world_matrix",Matrix4f::Identity()); matrix(shader,"shadow_world_matrix",Matrix4f::Identity());
                matrix(shader,"shadow_light_vp",light_vp);
                glUniformMatrix3fv(glGetUniformLocation(shader,"view_normal_matrix"),1,GL_FALSE,rotation.data());
                const Matrix3f inverse=rotation.transpose(), identity=Matrix3f::Identity();
                glUniformMatrix3fv(glGetUniformLocation(shader,"inverse_view_rotation"),1,GL_FALSE,inverse.data());
                glUniformMatrix3fv(glGetUniformLocation(shader,"environment_rotation"),1,GL_FALSE,identity.data());
                const Vec3f key=rotation*light.cast<float>(), fill=rotation*Vec3f(.8f,.3f,.55f).normalized(), up=rotation*Vec3f::UnitZ();
                glUniform3fv(glGetUniformLocation(shader,"static_light_dir"),1,key.data());
                glUniform3fv(glGetUniformLocation(shader,"plastic_fill_dir"),1,fill.data());
                glUniform3fv(glGetUniformLocation(shader,"world_up_eye"),1,up.data());
                glUniform4f(glGetUniformLocation(shader,"clipping_plane"),0,0,0,1);
                glUniform2f(glGetUniformLocation(shader,"z_range"),-1000,1000);
                glUniform4fv(glGetUniformLocation(shader,"shadow_bounds"),1,fit->receiver_bounds.data());
                integer(shader,"print_volume.type",-1); integer(shader,"depth_tex",5);
                integer(shader,"shadow_map",4); scalar(shader,"shadow_map_texel",1.0f/2048);
                scalar(shader,"shadow_intensity",mode==1 ? .85f : .35f); scalar(shader,"shadow_depth_bias",fit->depth_bias);
                integer(shader,"studio_radiance",2); integer(shader,"studio_brdf",3);
                integer(shader,"use_static_light",1); integer(shader,"plastic_encode_srgb",1);
                integer(shader,"studio_environment_enabled",1); integer(shader,"plastic_material",1);
                glUniform4f(glGetUniformLocation(shader,"uniform_color"),item.second==&floor?.75f:.08f,item.second==&floor?.78f:.32f,item.second==&floor?.8f:.75f,1);
                draw(shader,*item.second,vao,buffer);
            }
            if (glGetError()!=GL_NO_ERROR) throw std::runtime_error("Graphics error while drawing");
            std::vector<unsigned char> pixels(size_t(width)*height*4);
            glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            wxImage image(width,height); image.InitAlpha();
            for (int yy=0;yy<height;++yy) for (int xx=0;xx<width;++xx) {
                const size_t src=(size_t(height-1-yy)*width+xx)*4, dst=size_t(yy)*width+xx;
                for (int c=0;c<3;++c) image.GetData()[dst*3+c]=pixels[src+c];
                image.GetAlpha()[dst]=pixels[src+3];
            }
            if (!image.SaveFile(m_output + (mode == 0 ? "/classic.png" : "/satin.png"), wxBITMAP_TYPE_PNG))
                throw std::runtime_error("Cannot save fixture image");
        }
        std::cout << "GPU: " << glGetString(GL_RENDERER) << "; viewport " << width << 'x' << height << '\n';
        for (GLuint shader:programs) glDeleteProgram(shader);
        for (GLuint id:{depth,radiance,brdf}) glDeleteTextures(1,&id);
        glDeleteFramebuffers(1,&fbo); glDeleteBuffers(1,&buffer); glDeleteVertexArrays(1,&vao);
    }
};

wxIMPLEMENT_APP(ViewportRenderApp);
