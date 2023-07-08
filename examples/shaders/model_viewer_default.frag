#version 450

layout (location = 0) in vec3 in_normal;
layout (location = 2) in vec3 in_light_direction;
layout (location = 3) in vec3 in_albedo_color;

layout (location = 0) out vec4 fragment;

void main()
{
	float lambertian = max(dot(in_normal, in_light_direction), 0.0);
	vec3 diffuse = in_albedo_color * lambertian;
	vec3 ambient = in_albedo_color * 0.1;
	fragment = vec4(diffuse + ambient, 1.0);
}
