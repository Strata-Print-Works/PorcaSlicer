#pragma once

#include "GLTexture.hpp"

namespace Slic3r::GUI {

// A small, offline-generated RGBM radiance atlas and split-sum BRDF lookup.
// Ownership is per canvas, like its post-processing and shadow textures.
class StudioEnvironment
{
public:
    bool ensure_loaded();
    void reset();
    unsigned int radiance_texture() const { return m_radiance.get_id(); }
    unsigned int brdf_texture() const { return m_brdf.get_id(); }
private:
    GLTexture m_radiance;
    GLTexture m_brdf;
    bool m_attempted{false};
};

} // namespace Slic3r::GUI
