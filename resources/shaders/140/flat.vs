#version 140

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

in vec3 v_position;
#ifdef PORCA_SHADOW_RECEIVER
uniform mat4 shadow_world_matrix;
out vec4 receiver_world_pos;
#endif

void main()
{
#ifdef PORCA_SHADOW_RECEIVER
    receiver_world_pos = shadow_world_matrix * vec4(v_position, 1.0);
#endif
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
