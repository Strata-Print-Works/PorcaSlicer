#include "ViewportLighting.hpp"
#include "GLShader.hpp"
#include <glad/gl.h>

namespace Slic3r::GUI {

void ShadowRenderContext::set_uniforms(GLShaderProgram& shader) const
{
    shader.set_uniform("shadow_map", 4);
    shader.set_uniform("shadow_light_vp", light_vp);
    shader.set_uniform("shadow_map_texel", valid() ? 1.0f / size : 0.0f);
    shader.set_uniform("shadow_intensity", valid() ? 0.85f : 0.0f);
    shader.set_uniform("shadow_depth_bias", depth_bias);
    shader.set_uniform("shadow_bounds", receiver_bounds);
    shader.set_uniform("receiver_light_dir", light_direction_eye);
}

ShadowTextureBinding::ShadowTextureBinding(const ShadowRenderContext* shadow)
{
    if (shadow == nullptr || !shadow->valid())
        return;
    m_bound = true;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &m_active);
    glActiveTexture(GL_TEXTURE4);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture);
    glBindTexture(GL_TEXTURE_2D, shadow->texture);
    glActiveTexture(m_active);
}

ShadowTextureBinding::~ShadowTextureBinding()
{
    if (m_bound) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glActiveTexture(m_active);
    }
}

} // namespace Slic3r::GUI
