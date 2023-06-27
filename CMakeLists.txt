cmake_minimum_required(VERSION 3.20)

project(littlevk CXX)

set(CMAKE_CXX_STANDARD 17)

# Graphics libraries
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glslang REQUIRED)

add_executable(example-hello-triangle hello_triangle.cpp)

target_link_libraries(example-hello-triangle PRIVATE
	Vulkan::Vulkan glfw SPIRV
	glslang::glslang
	glslang::glslang-default-resource-limits)
