cmake_minimum_required(VERSION 3.20)

project(littlevk CXX)

set(CMAKE_CXX_STANDARD 17)

# Graphics libraries
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glslang REQUIRED)

add_executable(example-hello-triangle hello_triangle.cpp)
add_executable(example-spinning-cube spinning_cube.cpp)
add_executable(example-mesh-viewer mesh_viewer.cpp)
add_executable(example-model-viewer model_viewer.cpp)

include_directories(.. glm stb)

set(LIBRARIES
	Vulkan::Vulkan glfw SPIRV
	glslang::glslang-default-resource-limits
	assimp)

target_link_libraries(example-hello-triangle PRIVATE ${LIBRARIES})
target_link_libraries(example-spinning-cube  PRIVATE ${LIBRARIES})
target_link_libraries(example-mesh-viewer    PRIVATE ${LIBRARIES})
target_link_libraries(example-model-viewer   PRIVATE ${LIBRARIES})

add_definitions(-DEXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
