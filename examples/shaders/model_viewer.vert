#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 uv;

layout (push_constant) uniform PushConstants {
	mat4 model;
	mat4 view;
	mat4 proj;

	vec3 light_direction;
	vec3 albedo_color;
};

layout (location = 0) out vec3 out_normal;
layout (location = 1) out vec2 out_uv;
layout (location = 2) out vec3 out_light_direction;
layout (location = 3) out vec3 out_albedo_color;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;

	mat3 mv = mat3(view * model);

	out_normal = normalize(mv * normal);
	out_uv = uv;
	out_light_direction = mv * normalize(light_direction);
	out_albedo_color = albedo_color;
}
