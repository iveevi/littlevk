#include "littlevk.hpp"

// GLM for vector math
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Assimp for mesh loading
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

// Argument parsing
#include "argparser.hpp"

// Vertex data
struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
};

// Mesh and mesh loading
struct Mesh {
	std::vector <Vertex> vertices;
	std::vector <uint32_t> indices;
};

Mesh load_mesh(const std::filesystem::path &);

// Shader sources
const std::string vertex_shader_source = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;

layout (push_constant) uniform MVP {
	mat4 model;
	mat4 view;
	mat4 proj;

	vec3 color;
	vec3 light_direction;
};

layout (location = 0) out vec3 out_color;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec3 out_light_direction;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;

	mat3 mv = mat3(view * model);

	out_color = color;
	out_normal = normalize(mv * normal);
	out_light_direction = light_direction;
}
)";

const std::string fragment_shader_source = R"(
#version 450

layout (location = 0) in vec3 in_color;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec3 in_light_direction;

// layout (location = 2) in vec2 frag_texcoord;
// push constant for enabling textures

layout (location = 0) out vec4 out_color;

void main() {
	out_color = vec4(in_color, 1.0) * max(dot(in_normal, in_light_direction), 0.0);
}
)";

int main(int argc, char *argv[])
{
	// Vulkan device extensions
	static const std::vector <const char *> EXTENSIONS {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	// Process the arguments
	ArgParser argparser { "example-mesh-viewer", 1, {
		ArgParser::Option { "filename", "Input mesh" },
	}};

	argparser.parse(argc, argv);

	std::filesystem::path path;
	path = argparser.get <std::string> (0);
	path = std::filesystem::weakly_canonical(path);

	// Load the mesh
	Mesh mesh = load_mesh(path);

	// Precompute some data for rendering
	glm::vec3 center = glm::vec3(0.0f);
	glm::vec3 min = glm::vec3(FLT_MAX);
	glm::vec3 max = glm::vec3(-FLT_MAX);

	for (const Vertex &vertex : mesh.vertices) {
		center += vertex.position/float(mesh.vertices.size());
		min = glm::min(min, vertex.position);
		max = glm::max(max, vertex.position);
	}

	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev, EXTENSIONS);
	};

	vk::PhysicalDevice phdev = littlevk::pick_physical_device(predicate);
	vk::PhysicalDeviceMemoryProperties memory_properties = phdev.getMemoryProperties();

	// Create an application skeleton with the bare minimum
	littlevk::Skeleton app;
        app.skeletonize(phdev, { 800, 600 }, "Mesh Viewer", EXTENSIONS);

	// Create a deallocator for automatic resource cleanup
	auto deallocator = new littlevk::Deallocator { app.device };
	
	// Create a render pass
	vk::RenderPass render_pass = littlevk::RenderPassAssembler(app.device, deallocator)
		.add_attachment(littlevk::default_color_attachment(app.swapchain.format))
		.add_attachment(littlevk::default_depth_attachment())
		.add_subpass(vk::PipelineBindPoint::eGraphics)
			.color_attachment(0, vk::ImageLayout::eColorAttachmentOptimal)
			.depth_attachment(1, vk::ImageLayout::eDepthStencilAttachmentOptimal)
			.done();

	// Create a depth buffer
	littlevk::Image depth_buffer = bind(app.device, memory_properties, deallocator)
		.image(app.window->extent,
			vk::Format::eD32Sfloat,
			vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::ImageAspectFlagBits::eDepth);

	// Create framebuffers from the swapchain
	littlevk::FramebufferGenerator generator(app.device, render_pass, app.window->extent, deallocator);
	for (const auto &view : app.swapchain.image_views)
		generator.add(view, depth_buffer.view);

	std::vector <vk::Framebuffer> framebuffers = generator.unpack();

	// Allocate command buffers
	vk::CommandPool command_pool = littlevk::command_pool(app.device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	).unwrap(deallocator);

	auto command_buffers = app.device.allocateCommandBuffers
		({ command_pool, vk::CommandBufferLevel::ePrimary, 2 });

	// Allocate mesh buffers
	littlevk::Buffer vertex_buffer;
	littlevk::Buffer index_buffer;

	std::tie(vertex_buffer, index_buffer) = bind(app.device, memory_properties, deallocator)
		.buffer(mesh.vertices, vk::BufferUsageFlagBits::eVertexBuffer)
		.buffer(mesh.indices, vk::BufferUsageFlagBits::eIndexBuffer);

	// Compile shader modules
	vk::ShaderModule vertex_module = littlevk::shader::compile(
		app.device, vertex_shader_source,
		vk::ShaderStageFlagBits::eVertex
	).unwrap(deallocator);

	vk::ShaderModule fragment_module = littlevk::shader::compile(
		app.device, fragment_shader_source,
		vk::ShaderStageFlagBits::eFragment
	).unwrap(deallocator);

	// Create a graphics pipeline
	struct MVP {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;

		alignas(16) glm::vec3 color;
		alignas(16) glm::vec3 light_direction;
	};

	auto vertex_layout = littlevk::VertexLayout <littlevk::rgb32f, littlevk::rgb32f> ();

	auto bundle = littlevk::ShaderStageBundle(app.device, deallocator)
		.attach(vertex_shader_source, vk::ShaderStageFlagBits::eVertex)
		.attach(fragment_shader_source, vk::ShaderStageFlagBits::eFragment);

	littlevk::Pipeline ppl = littlevk::PipelineAssembler(app.device, app.window, deallocator)
		.with_render_pass(render_pass, 0)
		.with_vertex_layout(vertex_layout)
		.with_shader_bundle(bundle)
		.with_push_constant <MVP> (vk::ShaderStageFlagBits::eVertex);

	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(deallocator);

	// Prepare camera and model matrices
	glm::mat4 model = glm::mat4 { 1.0f };
	model = glm::translate(model, -center);

	float radius = 1.0f;
	glm::mat4 view = glm::lookAt(
		radius * glm::vec3 { 0.0f, 0.0f, glm::length(max - min) },
		glm::vec3 { 0.0f, 0.0f, 0.0f },
		glm::vec3 { 0.0f, 1.0f, 0.0f }
	);

	// Pre render items
	bool pause_rotate = false;
	bool pause_resume_pressed = false;

	float previous_time = 0.0f;
	float current_time = 0.0f;

	printf("Instructions:\n");
	printf("[ +/- ] Zoom in/out\n");
	printf("[Space] Pause/resume rotation\n");

	// Resize callback
	auto resize = [&]() {
		app.resize();

		// Recreate the depth buffer
		littlevk::Image depth_buffer = bind(app.device, memory_properties, deallocator)
			.image(app.window->extent,
				vk::Format::eD32Sfloat,
				vk::ImageUsageFlagBits::eDepthStencilAttachment,
				vk::ImageAspectFlagBits::eDepth);

		// Rebuid the framebuffers
		generator.extent = app.window->extent;
		for (const auto &view : app.swapchain.image_views)
			generator.add(view);

		framebuffers = generator.unpack();
	};

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();

		// Event handling
                if (glfwWindowShouldClose(app.window->handle))
                        break;

		// Zoom in/out
		if (glfwGetKey(app.window->handle, GLFW_KEY_EQUAL) == GLFW_PRESS) {
			radius += 0.01f;
			view = glm::lookAt(
				radius * glm::vec3 { 0.0f, 0.0f, glm::length(max - min) },
				glm::vec3 { 0.0f, 0.0f, 0.0f },
				glm::vec3 { 0.0f, 1.0f, 0.0f }
			);
		} else if (glfwGetKey(app.window->handle, GLFW_KEY_MINUS) == GLFW_PRESS) {
			radius -= 0.01f;
			view = glm::lookAt(
				radius * glm::vec3 { 0.0f, 0.0f, glm::length(max - min) },
				glm::vec3 { 0.0f, 0.0f, 0.0f },
				glm::vec3 { 0.0f, 1.0f, 0.0f }
			);
		}

		// Pause/resume rotation
		if (glfwGetKey(app.window->handle, GLFW_KEY_SPACE) == GLFW_PRESS) {
			if (!pause_resume_pressed) {
				pause_rotate = !pause_rotate;
				pause_resume_pressed = true;
			}
		} else {
			pause_resume_pressed = false;
		}

		if (!pause_rotate)
			current_time += glfwGetTime() - previous_time;
		previous_time = glfwGetTime();

		// Rendering
		littlevk::SurfaceOperation op;
                op = littlevk::acquire_image(app.device, app.swapchain.swapchain, sync[frame]);
		if (op.status == littlevk::SurfaceOperation::eResize) {
			resize();
			continue;
		}

		// Start empty render pass
		std::array <vk::ClearValue, 2> clear_values {
			vk::ClearColorValue { std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f } },
			vk::ClearDepthStencilValue { 1.0f, 0 }
		};

		vk::RenderPassBeginInfo render_pass_info {
			render_pass, framebuffers[op.index],
			vk::Rect2D { {}, app.window->extent },
			clear_values
		};

		// Record command buffer
		vk::CommandBuffer &cmd = command_buffers[frame];

		cmd.begin(vk::CommandBufferBeginInfo {});
		cmd.beginRenderPass(render_pass_info, vk::SubpassContents::eInline);

		// Set viewport and scissor
		littlevk::viewport_and_scissor(cmd, littlevk::RenderArea(app.window));

		// Render the triangle
		MVP push_constants;

		// Rotate the model matrix
		push_constants.view = view;

		push_constants.model = glm::rotate(
			model,
			current_time * glm::radians(90.0f),
			glm::vec3 { 0.0f, 1.0f, 0.0f }
		);

		push_constants.proj = glm::perspective(
			glm::radians(45.0f),
			app.aspect_ratio(),
			0.1f, 100 * glm::length(max - min)
		);

		push_constants.color = glm::vec3 { 1.0f, 0.0f, 0.0f };
		push_constants.light_direction = glm::normalize(glm::vec3 { 0, 0, 1 });

		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ppl.handle);
		cmd.pushConstants <MVP> (ppl.layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
		cmd.bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		cmd.bindIndexBuffer(index_buffer.buffer, 0, vk::IndexType::eUint32);
		cmd.drawIndexed(mesh.indices.size(), 1, 0, 0, 0);

		cmd.endRenderPass();
		cmd.end();

		// Submit command buffer while signaling the semaphore
		vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submit_info {
			sync.image_available[frame],
			wait_stage, cmd,
			sync.render_finished[frame]
		};

		app.graphics_queue.submit(submit_info, sync.in_flight[frame]);

                op = littlevk::present_image(app.present_queue, app.swapchain.swapchain, sync[frame], op.index);
		if (op.status == littlevk::SurfaceOperation::eResize)
			resize();

		frame = 1 - frame;
        }

	// Finish all pending operations
	app.device.waitIdle();

	// Free resources using automatic deallocator
	delete deallocator;

        // Delete application
	app.destroy();

	// TODO: address santizer to check leaks...
        return 0;
}

Mesh process_mesh(aiMesh *mesh, const aiScene *scene, const std::string &dir)
{
	// Mesh data
	std::vector <Vertex> vertices;
	std::vector <uint32_t> triangles;

	// Process all the mesh's vertices
	for (size_t i = 0; i < mesh->mNumVertices; i++) {
		glm::vec3 v {
			mesh->mVertices[i].x,
			mesh->mVertices[i].y,
			mesh->mVertices[i].z
		};

		glm::vec3 n {
			mesh->mNormals[i].x,
			mesh->mNormals[i].y,
			mesh->mNormals[i].z
		};

		vertices.push_back({ v, n });
	}

	// Process all the mesh's triangles
	std::stack <size_t> buffer;
	for (size_t i = 0; i < mesh->mNumFaces; i++) {
		aiFace face = mesh->mFaces[i];
		for (size_t j = 0; j < face.mNumIndices; j++) {
			buffer.push(face.mIndices[j]);
			if (buffer.size() >= 3) {
				size_t i0 = buffer.top(); buffer.pop();
				size_t i1 = buffer.top(); buffer.pop();
				size_t i2 = buffer.top(); buffer.pop();

				triangles.push_back(i0);
				triangles.push_back(i1);
				triangles.push_back(i2);
			}
		}
	}

	return { vertices, triangles };
}

Mesh process_node(aiNode *node, const aiScene *scene, const std::string &dir)
{
	for (size_t i = 0; i < node->mNumMeshes; i++) {
		aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
                Mesh processed_mesh = process_mesh(mesh, scene, dir);
		if (processed_mesh.indices.size() > 0)
			return processed_mesh;
	}

	// Recusively process all the node's children
	for (size_t i = 0; i < node->mNumChildren; i++) {
		Mesh processed_mesh = process_node(node->mChildren[i], scene, dir);
		if (processed_mesh.indices.size() > 0)
			return processed_mesh;
	}

	return {};
}

Mesh load_mesh(const std::filesystem::path &path)
{
	Assimp::Importer importer;

	// Read scene
	const aiScene *scene = importer.ReadFile(
		path, aiProcess_Triangulate
			| aiProcess_GenNormals
			| aiProcess_FlipUVs
	);

	// Check if the scene was loaded
	if (!scene | scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE
			|| !scene->mRootNode) {
		fprintf(stderr, "Assimp error: \"%s\"\n", importer.GetErrorString());
		return {};
	}

	// Process the scene (root node)
	return process_node(scene->mRootNode, scene, path.string());
}
