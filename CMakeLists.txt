cmake_minimum_required(VERSION 3.20)

project(littlevk CXX)

set(CMAKE_CXX_STANDARD 20)

# Graphics libraries
find_package(Vulkan REQUIRED)

add_executable(hello-triangle examples/hello_triangle.cpp)
add_executable(spinning-cube examples/spinning_cube.cpp)
add_executable(mesh-viewer examples/mesh_viewer.cpp)
add_executable(model-viewer examples/model_viewer.cpp)

include_directories(. examples/glm examples/stb)

set(LIBRARIES
	Vulkan::Vulkan glfw SPIRV
	glslang-default-resource-limits
	assimp)

target_link_libraries(hello-triangle PRIVATE ${LIBRARIES})
target_link_libraries(spinning-cube  PRIVATE ${LIBRARIES})
target_link_libraries(mesh-viewer    PRIVATE ${LIBRARIES})
target_link_libraries(model-viewer   PRIVATE ${LIBRARIES})

add_definitions(-DEXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
