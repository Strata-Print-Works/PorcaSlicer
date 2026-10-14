#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
const vec3 WHITE = vec3(1.0, 1.0, 1.0);
struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

uniform vec4 uniform_color;
uniform float emission_factor;
uniform PrintVolumeDetection print_volume;
// x = diffuse, y = specular;
in vec2 intensity;
in vec4 world_pos;

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
in vec2 key_intensity;
#endif

void main()
{
    vec3  color = uniform_color.rgb;
    float alpha = uniform_color.a;
	// if the fragment is outside the print volume -> use darker color
    vec3 pv_check_min = ZERO;
    vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {// rectangle
        pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
        pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
    }
    else if (print_volume.type == 1) {// circle
        float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
        pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
        pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
    }
    color = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color, WHITE, 0.3333) : color;
	//out_color = vec4(vec3(intensity.y) + color * intensity.x, alpha);
	out_color = vec4(vec3(intensity.y) + color * (intensity.x + emission_factor), alpha);
#ifdef PORCA_SHADOW_RECEIVER
    float visibility = receiver_visibility(world_pos);
    out_color.rgb -= (1.0 - visibility) * (vec3(key_intensity.y) + color * key_intensity.x);
#endif
}