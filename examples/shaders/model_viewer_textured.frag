#version 450

layout (location = 0) in vec3 in_normal;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec3 in_light_direction;

layout (binding = 0) uniform sampler2D albedo_sampler;

layout (location = 0) out vec4 fragment;

void main()
{
	vec4 albedo = texture(albedo_sampler, in_uv);
	if (albedo.a < 0.5)
		discard;

	float lambertian = max(dot(in_normal, in_light_direction), 0.0);
	vec3 diffuse = albedo.xyz * lambertian;
	vec3 ambient = albedo.xyz * 0.1;
	fragment = vec4(diffuse + ambient, 1.0);
}
