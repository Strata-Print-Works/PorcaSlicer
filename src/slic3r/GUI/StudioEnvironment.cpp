#include "StudioEnvironment.hpp"
#include "GLTexture.hpp"

#include "libslic3r/Utils.hpp"
#include <string>
#include <glad/gl.h>

namespace Slic3r::GUI {

bool StudioEnvironment::ensure_loaded()
{
    if (!m_attempted) {
        m_attempted = true;
        GLint maximum_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_size);
        if (maximum_size < 1040)
            return false;
        GLint binding = 0, alignment = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        const std::string path = resources_dir() + "/lighting/";
        if (!m_radiance.load_from_file(path + "studio-radiance.png", false, GLTexture::None, false) ||
            !m_brdf.load_from_file(path + "studio-brdf.png", false, GLTexture::None, false)) {
            m_radiance.reset();
            m_brdf.reset();
        }
        // glGenTextures can succeed even when storage allocation fails.
        auto allocated = [](const GLTexture& texture, int width, int height) {
            if (texture.get_id() == 0)
                return false;
            glBindTexture(GL_TEXTURE_2D, texture.get_id());
            GLint actual_width = 0, actual_height = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_height);
            return actual_width == width && actual_height == height;
        };
        if (!allocated(m_radiance, 258, 1040) || !allocated(m_brdf, 128, 128)) {
            m_radiance.reset();
            m_brdf.reset();
        }
        glBindTexture(GL_TEXTURE_2D, binding);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    }
    return m_radiance.get_id() != 0 && m_brdf.get_id() != 0;
}

void StudioEnvironment::reset()
{
    m_radiance.reset();
    m_brdf.reset();
    m_attempted = false;
}

} // namespace Slic3r::GUI
