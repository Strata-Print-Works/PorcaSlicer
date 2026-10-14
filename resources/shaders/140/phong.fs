#version 140
// Multisample depth texture for the anti-aliased outline (see 3DScene.cpp render_with_outline).
// Optional on the GLSL 140 path: if unavailable, fallback to a non-multisample depth texture.
#extension GL_ARB_texture_multisample : enable

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

#define INTENSITY_CORRECTION 0.6
#define PHONG_BRIGHTNESS     1.0

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.85 * INTENSITY_CORRECTION)   
#define LIGHT_TOP_SPECULAR   (0.35 * INTENSITY_CORRECTION)   
#define LIGHT_TOP_SHININESS  32.0                           

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.35 * INTENSITY_CORRECTION)   
#define LIGHT_FRONT_SPECULAR (0.12 * INTENSITY_CORRECTION)   
#define LIGHT_FRONT_SHININESS 16.0                           

#define INTENSITY_AMBIENT    0.25                            
#define WINDOW_REFLECTION_INTENSITY 0.30                    

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

struct SlopeDetection
{
    bool actived;
    float normal_z;
    mat3 volume_world_normal_matrix;
    vec3 up_direction;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

//BBS: add outline_color
uniform bool is_outline;
// The outline is a per-fragment discard mask, which the framebuffer MSAA cannot smooth, so the
// silhouette is resolved per sample from a multisample copy of the outlined model's depth buffer.
#ifdef GL_ARB_texture_multisample
uniform sampler2DMS depth_tex;
uniform int msaa_samples; // samples in depth_tex, 1 when MSAA is off
#else
uniform sampler2D depth_tex;
#endif
uniform vec2 screen_size;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

uniform float z_far;
uniform float z_near;
uniform bool enable_ssao;
#ifdef PORCA_PLASTIC
uniform sampler2D studio_radiance;
uniform sampler2D studio_brdf;
uniform bool studio_environment_enabled;
uniform mat3 inverse_view_rotation;
uniform mat3 environment_rotation;
uniform vec3 plastic_fill_dir;
uniform bool plastic_orthographic;
uniform bool plastic_encode_srgb;
uniform bool plastic_material;
uniform int plastic_finish; // SurfaceFinish: satin, matte, silk, carbon fiber, galaxy, wood.
uniform vec3 plastic_layer_up_eye;
uniform mat3 view_normal_matrix;
in vec3 plastic_surface_position;
uniform float shadow_depth_bias;
#endif

// 1 = the Design tab's studio lighting (studio_shade below); 0 = the two-light model above, which
// every other canvas keeps. world_up_eye is world +Z in eye space, for the hemisphere ambient.
uniform int lighting_model;
uniform vec3 world_up_eye;

// Depth-based shadow map (object-on-object and self shadows). shadow_intensity == 0 disables it.
uniform sampler2D shadow_map;
uniform mat4 shadow_light_vp;
uniform float shadow_intensity;
uniform float shadow_map_texel;
// ORCA: realistic view - static shadows also light the scene from their fixed light.
uniform bool use_static_light;
uniform vec3 static_light_dir;
vec3 top_light_dir() { return use_static_light ? static_light_dir : LIGHT_TOP_DIR; }

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;
in vec3 eye_position;

out vec4 out_color;

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithm by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);

    d = d*2.0-1.0;

    return -k*(d+A)*(d+A)/B;
}

// Depth of sample s at integer pixel coord.
#ifdef GL_ARB_texture_multisample
float FetchDepth(ivec2 coord, int s)
{
    // texelFetch has no wrap mode, so clamp to the edge texel (sampler2D used CLAMP_TO_EDGE).
    ivec2 sz = textureSize(depth_tex);
    return abs(texelFetch(depth_tex, clamp(coord, ivec2(0), sz - 1), s).r);
}
#else
float FetchDepth(ivec2 coord, int s)
{
    return abs(texture(depth_tex, (vec2(coord) + 0.5) / screen_size).r);
}
#endif

float DetectSilho(ivec2 coord, ivec2 dir, int s)
{
    float x0 = FetchDepth(coord + dir*-2, s);
    float x1 = FetchDepth(coord + dir*-1, s);
    float x2 = FetchDepth(coord,          s);
    float x3 = FetchDepth(coord + dir* 1, s);

    float d0 = (x1-x0);
    float d1 = (x2-x3);

    float r0 = x1 + d0 - x2;
    float r1 = x2 + d1 - x1;

    float tol = GetTolerance(x2, 0.04);

    return smoothstep(0.0, tol*tol, max( - r0*r1, 0.0));
}

float DetectSilho(ivec2 coord, int s)
{
    return max(
        DetectSilho(coord, ivec2(1,0), s),
        DetectSilho(coord, ivec2(0,1), s)
        );
}

// Full response of one sample. Reduce the max() per sample and average only afterwards:
// max(mean) <= mean(max), and averaging first hollows out diagonal and curved lines.
float DetectSilhoSample(ivec2 coord, int s)
{
    float v = DetectSilho(coord, s);
    // Makes silhouettes thicker.
    for (int i = 1; i <= INFLATE; ++i)
    {
        v = max(v, DetectSilho(coord + ivec2(i, 0), s));
        v = max(v, DetectSilho(coord + ivec2(0, i), s));
    }
    return v;
}

// Average the per-sample coverage into the sub-pixel anti-aliasing of the line.
float DetectSilho(vec2 fragCoord)
{
    ivec2 coord = ivec2(fragCoord);
#ifdef GL_ARB_texture_multisample
    int n = max(msaa_samples, 1);
#else
    const int n = 1;
#endif
    float acc = 0.0;
    for (int s = 0; s < n; ++s)
        acc += DetectSilhoSample(coord, s);
    return acc / float(n);
}

float compute_ssao_factor(vec3 normal, vec3 view_dir, vec3 eye_pos)
{
    vec3 normal_dx = dFdx(normal);
    vec3 normal_dy = dFdy(normal);
    float normal_variation = clamp(length(normal_dx) + length(normal_dy), 0.0, 1.0);

    float depth_gradient = clamp(length(vec2(dFdx(eye_pos.z), dFdy(eye_pos.z))) * 0.8, 0.0, 1.0);

    float cavity = clamp(normal_variation * 0.70 + depth_gradient * 0.60, 0.0, 1.0);
    float cavity_mask = smoothstep(0.25, 0.75, cavity);
    float ao_strength = pow(cavity, 1.15) * cavity_mask;
    return clamp(1.0 - ao_strength * 0.90, 0.25, 1.0);
}

float soft_circle(vec2 p, vec2 center, float radius, float blur)
{
    float dist = distance(p, center);
    return 1.0 - smoothstep(radius - blur, radius, dist);
}

vec3 compute_window_reflection(vec3 normal, vec3 view_dir)
{
    const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
    
    vec3 light_dir = normalize(LIGHT_TOP_DIR);
    vec3 reflect_light = normalize(reflect(-light_dir, normal));
    
    // UV coordinates for the reflection
    vec2 uv = (reflect_light.xy / (1.0 + max(reflect_light.z, 0.3))) * 2.2;
    
    vec2 grad = fwidth(uv) * 0.8;
    float blur = 0.12 + grad.x * 1.5;
    
    // === CIRCULAR WINDOW (porthole style) ===
    // Single round window, no bars
    vec2 window_center = vec2(0.0, 0.0);
    float window_radius = 0.5;  // Radius of the circular window
    
    float window_light = soft_circle(uv, window_center, window_radius, blur);
    
    // No bars - just pure circular glass
    float bars = 1.0;
    

    float fresnel = pow(1.0 - max(dot(normal, view_dir), 0.0), 1.2);
    float facing = smoothstep(-0.4, 0.6, reflect_light.z);
    

    float intensity = window_light * bars * (0.15 + 0.15 * fresnel) * facing;
    intensity = clamp(intensity, 0.0, 0.25);

    return vec3(intensity);
}

// Returns a lighting multiplier in [1 - shadow_intensity, 1]: < 1 where the fragment is
// occluded from the light in the shadow map. 3x3 PCF softens the edges.
float shadow_shade()
{
    if (shadow_intensity <= 0.0)
        return 1.0;

    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0)
        return 1.0;

    // Slope-scaled depth bias: larger where the surface grazes / faces away from the light. This
    // suppresses self-shadow acne without discarding real shadows cast by other objects onto
    // back-facing surfaces (e.g. the shaded back/tip of a cone sitting inside a larger shadow).
    float NdotL = dot(normalize(eye_normal), top_light_dir());
    #ifdef PORCA_PLASTIC
    float bias = shadow_depth_bias * mix(1.0, 5.0, clamp(1.0 - NdotL, 0.0, 1.0));
#else
    float bias = mix(0.0004, 0.004, clamp(1.0 - NdotL, 0.0, 1.0));
#endif
    // 5x5 PCF: softens shadow edges into a smooth penumbra and blurs residual facet acne.
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return 1.0 - shadow_intensity * (sum / 25.0);
}

// Studio lighting for the Design tab. The default model lights every face from near the camera,
// so the sides of a part come out in nearly the same tone and its form is hard to read. This one
// separates faces by their orientation in the WORLD (a sky/ground hemisphere: up-facing faces
// cool and bright, down-facing ones warm and dark), keeps a strong key light from the upper left
// and a weak fill from the right, gives a plastic-like highlight, and darkens the base colour
// toward the silhouette while adding a faint sheen there, so curved faces read as round.
vec3 studio_shade(vec3 base, vec3 n, vec3 v)
{
    vec3 key  = normalize(vec3(-0.45, 0.60, 0.66));
    vec3 fill = normalize(vec3(0.70, -0.15, 0.70));
    float hemi = 0.5 + 0.5 * dot(n, normalize(world_up_eye));
    vec3 ambient = mix(vec3(0.16, 0.15, 0.14), vec3(0.40, 0.42, 0.46), hemi);
    float kd = max(dot(n, key), 0.0);
    float fd = max(dot(n, fill), 0.0);
    vec3 diffuse = ambient + vec3(0.60) * kd + vec3(0.20) * fd;
    float spec = 0.28 * pow(max(dot(n, normalize(key + v)), 0.0), 48.0)
               + 0.06 * pow(max(dot(n, normalize(fill + v)), 0.0), 24.0);
    float rim = pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 3.0);
    return base * diffuse * (1.0 - 0.30 * rim) + vec3(spec + 0.08 * rim);
}

#ifdef PORCA_PLASTIC
const float PLASTIC_PI = 3.14159265;
float plastic_roughness()
{
    if (plastic_finish == 1) return 0.80;
    if (plastic_finish == 2) return 0.22;
    if (plastic_finish == 3) return 0.88;
    if (plastic_finish == 4) return 0.50;
    if (plastic_finish == 5) return 0.78;
    return 0.46;
}

float plastic_hash(vec3 p)
{
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float plastic_noise(vec3 p)
{
    vec3 cell = floor(p);
    vec3 w = fract(p);
    w = w * w * (3.0 - 2.0 * w);
    return mix(mix(mix(plastic_hash(cell), plastic_hash(cell + vec3(1,0,0)), w.x),
                   mix(plastic_hash(cell + vec3(0,1,0)), plastic_hash(cell + vec3(1,1,0)), w.x), w.y),
               mix(mix(plastic_hash(cell + vec3(0,0,1)), plastic_hash(cell + vec3(1,0,1)), w.x),
                   mix(plastic_hash(cell + vec3(0,1,1)), plastic_hash(cell + vec3(1,1,1)), w.x), w.y), w.z);
}

vec3 plastic_tangent(vec3 n)
{
    vec3 t = cross(plastic_layer_up_eye, n);
    if (dot(t, t) < 0.00001)
        t = cross(transpose(inverse_view_rotation) * vec3(1,0,0), n);
    return normalize(t);
}

vec3 plastic_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 plastic_display(vec3 c)
{
    // Bounded, shader-local tone mapping. UI and diagnostic marks remain outside it.
    c = c / (vec3(1.0) + c);
    return plastic_encode_srgb ? mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
                                    step(vec3(0.0031308), c)) : c;
}

vec3 plastic_fresnel(float vh)
{
    return vec3(0.04) + vec3(0.96) * pow(1.0 - clamp(vh, 0.0, 1.0), 5.0);
}

vec3 plastic_light(vec3 base, vec3 n, vec3 v, vec3 l, float roughness)
{
    float nl = max(dot(n, l), 0.0);
    float nv = max(dot(n, v), 0.001);
    vec3 half_sum = l + v;
    vec3 h = half_sum * inversesqrt(max(dot(half_sum, half_sum), 0.000001));
    float nh = max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    float alpha = roughness * roughness;
    float a2 = alpha * alpha;
    float denominator = nh * nh * (a2 - 1.0) + 1.0;
    float distribution = a2 / max(PLASTIC_PI * denominator * denominator, 0.00001);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) +
                                 nv * sqrt(nl * nl * (1.0 - a2) + a2), 0.00001);
    if (plastic_finish == 2) {
        vec3 t = plastic_tangent(n);
        vec3 b = cross(n, t);
        float ax = max(alpha, 0.04);
        float ay = 0.32;
        float hx = dot(h, t) / ax;
        float hy = dot(h, b) / ay;
        float d = hx * hx + hy * hy + nh * nh;
        distribution = 1.0 / max(PLASTIC_PI * ax * ay * d * d, 0.00001);
        vec2 vl = vec2(ax * dot(v, t), ay * dot(v, b));
        vec2 ll = vec2(ax * dot(l, t), ay * dot(l, b));
        visibility = 0.5 / max(nl * sqrt(dot(vl, vl) + nv * nv) +
                               nv * sqrt(dot(ll, ll) + nl * nl), 0.00001);
    }
    vec3 f = plastic_fresnel(vh);
    return ((vec3(1.0) - f) * base / PLASTIC_PI + distribution * visibility * f) * nl;
}

vec3 plastic_room(vec3 direction, float roughness)
{
    direction = normalize(environment_rotation * inverse_view_rotation * direction);
    vec2 uv = vec2(atan(direction.y, direction.x) / (2.0 * PLASTIC_PI) + 0.5,
                   acos(clamp(direction.z, -1.0, 1.0)) / PLASTIC_PI);
    // Eight 256x128 longitude/latitude tiles, each surrounded by a pixel gutter.
    float level = clamp(roughness, 0.0, 1.0) * 7.0;
    float lo = floor(level);
    float hi = min(lo + 1.0, 7.0);
    vec2 tile_uv = vec2((1.0 + uv.x * 256.0) / 258.0, (1.0 + uv.y * 128.0) / 130.0);
    vec4 a = texture(studio_radiance, vec2(tile_uv.x, (lo + tile_uv.y) / 8.0));
    vec4 b = texture(studio_radiance, vec2(tile_uv.x, (hi + tile_uv.y) / 8.0));
    // Decode before blending roughness levels; alpha stores radiance range, not opacity.
    return mix(a.rgb * a.a * 8.0, b.rgb * b.a * 8.0, fract(level));
}

vec3 plastic_shade(vec3 color, vec3 n, vec3 v, float key_visibility)
{
    vec3 base = plastic_linear(color);
    float roughness = plastic_roughness();
    float flake = 0.0;
    if (plastic_finish == 3) {
        vec3 p = plastic_surface_position * 7.0;
        float detail = 1.0 - smoothstep(0.5, 1.5, length(fwidth(p)));
        float grain = plastic_noise(p);
        base *= mix(1.0, 0.93 + 0.10 * grain, detail);
        roughness = clamp(roughness + (grain - 0.5) * 0.08 * detail, 0.75, 0.98);
    } else if (plastic_finish == 4) {
        vec3 p = plastic_surface_position * 2.5;
        vec3 cell = floor(p);
        float seed = plastic_hash(cell);
        vec3 centre = vec3(plastic_hash(cell + 11.0), plastic_hash(cell + 23.0), plastic_hash(cell + 37.0));
        float detail = 1.0 - smoothstep(0.35, 1.1, length(fwidth(p)));
        flake = step(0.86, seed) * (1.0 - smoothstep(0.14, 0.30, length(fract(p) - centre))) * detail;
        vec3 tilt = vec3(plastic_hash(cell + 43.0), plastic_hash(cell + 59.0), plastic_hash(cell + 71.0)) - 0.5;
        tilt = view_normal_matrix * tilt;
        tilt *= inversesqrt(max(dot(tilt, tilt), 0.00001));
        n = normalize(n + tilt * flake * 0.65);
        roughness = mix(roughness, 0.12, flake);
    } else if (plastic_finish == 5) {
        vec3 p = plastic_surface_position;
        float grain_phase = p.x * 3.0 + plastic_noise(vec3(p.x * 0.15, p.y * 0.15, p.z * 0.025)) * 4.0;
        float detail = 1.0 - smoothstep(0.8, 2.0, fwidth(grain_phase));
        base *= 1.0 - (0.5 + 0.5 * sin(grain_phase)) * 0.12 * detail;
    }
    vec3 key = plastic_light(base, n, v, top_light_dir(), roughness) * 2.5 * key_visibility;
    vec3 fill = plastic_light(base, n, v, normalize(plastic_fill_dir), roughness) * 0.65;
    float hemi = clamp(dot(n, normalize(world_up_eye)) * 0.5 + 0.5, 0.0, 1.0);
    vec3 indirect = base * mix(vec3(0.12, 0.11, 0.10), vec3(0.32, 0.34, 0.37), hemi);
    if (studio_environment_enabled) {
        vec2 brdf = texture(studio_brdf, vec2(max(dot(n, v), 0.0), roughness)).rg;
        vec3 reflection = reflect(-v, n);
        vec3 room = plastic_room(reflection, roughness);
        if (plastic_finish == 2) {
            vec3 stretch = cross(n, plastic_tangent(n)) * 0.22;
            room = (room + plastic_room(normalize(reflection + stretch), 0.32) +
                           plastic_room(normalize(reflection - stretch), 0.32)) / 3.0;
        }
        indirect += room * (vec3(0.04) * brdf.x + brdf.y) * (1.0 + flake * 4.0);
    }
    return plastic_display(key + fill + indirect);
}
#endif

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
    if (use_color_clip_plane) {
        color.rgb = (color_clip_plane_dot < 0.0) ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
        color.a = uniform_color.a;
    }
    else
        color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }

    vec3 pv_check_min = ZERO;
    vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
        pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
        pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
    }
    else if (print_volume.type == 1) {
        float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
        pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
        pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
    }
    color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    vec3 normal = normalize(eye_normal);
    vec3 view_dir = normalize(-eye_position);
#ifdef PORCA_PLASTIC
    if (plastic_orthographic)
        view_dir = vec3(0.0, 0.0, 1.0);
#endif

    float NdotL_top = max(dot(normal, top_light_dir()), 0.0);
    float diffuse = INTENSITY_AMBIENT + NdotL_top * LIGHT_TOP_DIFFUSE;
    vec3 half_top = normalize(top_light_dir() + view_dir);
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(normal, half_top), 0.0), LIGHT_TOP_SHININESS);

    float NdotL_front = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    diffuse += NdotL_front * LIGHT_FRONT_DIFFUSE;
    vec3 half_front = normalize(LIGHT_FRONT_DIR + view_dir);
    specular += LIGHT_FRONT_SPECULAR * pow(max(dot(normal, half_front), 0.0), LIGHT_FRONT_SHININESS);
    vec3 window_reflection = compute_window_reflection(normal, view_dir);

    // SSAO is applied in post-process pass. Keep base lighting unchanged here.

    float shade = shadow_shade();
    vec3 lit = (lighting_model == 1) ? studio_shade(color.rgb, normal, view_dir)
                                     : (vec3(specular) + window_reflection + color.rgb * diffuse) * PHONG_BRIGHTNESS;
#ifdef PORCA_PLASTIC
    if (plastic_material) {
        lit = plastic_shade(color.rgb, normal, view_dir, shade);
        // Preserve explicit diagnostic colors instead of tone mapping them into a finish.
        if (slope.actived || use_color_clip_plane)
            lit = color.rgb;
        shade = 1.0; // Only the key was occluded, never fill or room reflections.
    }
#endif

    if (is_outline) {
        vec3 shaded_rgb = lit * shade;
        vec4 shaded_color = vec4(clamp(shaded_rgb, vec3(0.0), vec3(1.0)), color.a);
        float s = DetectSilho(gl_FragCoord.xy);
        if (s < 0.01)
            discard;
        out_color = vec4(mix(shaded_color.rgb, getBackfaceColor(shaded_color.rgb), s), shaded_color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        out_color = vec4(clamp((0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + window_reflection + 0.8 * color.rgb * diffuse) * PHONG_BRIGHTNESS * shade, vec3(0.0), vec3(1.0)), color.a);
#endif
    else
        out_color = vec4(clamp(lit * shade, vec3(0.0), vec3(1.0)), color.a);
}
