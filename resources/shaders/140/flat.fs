#version 140

uniform vec4 uniform_color;

out vec4 out_color;


#ifdef PORCA_SHADOW_RECEIVER
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_map_texel;
uniform float shadow_intensity;
uniform float shadow_depth_bias;
uniform vec4 shadow_bounds;
float receiver_visibility(vec4 position)
{
    if (shadow_intensity <= 0.0)
        return 1.0;
    if (any(lessThan(position.xy, shadow_bounds.xy)) || any(greaterThan(position.xy, shadow_bounds.zw)))
        return 1.0;
    vec4 light = shadow_light_vp * position;
    vec3 p = light.xyz / light.w * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0))))
        return 1.0;
    float occlusion = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            float depth = texture(shadow_map, p.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            occlusion += p.z - shadow_depth_bias > depth ? 1.0 : 0.0;
        }
    return 1.0 - shadow_intensity * occlusion / 25.0;
}
#endif
#ifdef PORCA_SHADOW_RECEIVER
in vec4 receiver_world_pos;
#endif

void main()
{
    out_color = uniform_color;
#ifdef PORCA_SHADOW_RECEIVER
    // A quiet unlit bed/floor surface needs a restrained contact cue; alpha is unchanged.
    out_color.rgb *= mix(0.65, 1.0, receiver_visibility(receiver_world_pos));
#endif
}
