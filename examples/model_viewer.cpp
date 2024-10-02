#include <stack>

#include "littlevk.hpp"

// GLM for vector math
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Assimp for mesh loading
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

// Image loading with stb_image
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// Resource directories
#ifndef EXAMPLES_DIRETORY
#define EXAMPLES_DIRECTORY ".."
#endif

#define SHADERS_DIRECTORY EXAMPLES_DIRECTORY "/shaders"

#define CLEAR_LINE "\r\033[K"

// Argument parsing
// TODO: plus a logging utility... (and mesh utility)
// TODO: remove this...
#include "argparser.hpp"

// Vertex data
struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 uv;
};

// Mesh and mesh loading
struct Mesh {
	std::vector <Vertex> vertices;
	std::vector <uint32_t> indices;
	std::filesystem::path albedo_path;
	glm::vec3 albedo_color { 1.0f };
};

using Model = std::vector <Mesh>;

Model load_model(const std::filesystem::path &);

// Translating CPU data to Vulkan resources
struct VulkanMesh {
	littlevk::Buffer vertex_buffer;
	littlevk::Buffer index_buffer;
	size_t index_count;

	littlevk::Image albedo_image;
	vk::Sampler albedo_sampler;
	bool has_texture;

	glm::vec3 albedo_color;

	vk::DescriptorSet descriptor_set;
};

// Mouse control
struct {
	double last_x = 0.0;
	double last_y = 0.0;

	glm::mat4 view;
	glm::vec3 center;
	float radius;
	float radius_scale = 1.0f;

	double phi = 0.0;
	double theta = 0.0;

	bool left_dragging = false;
	bool right_dragging = false;
} g_state;

void rotate_view(double, double);
void mouse_callback(GLFWwindow *, int, int, int);
void cursor_callback(GLFWwindow *, double, double);
void scroll_callback(GLFWwindow *, double, double);

struct App : littlevk::Skeleton {
	vk::PhysicalDevice phdev;
	vk::PhysicalDeviceMemoryProperties memory_properties;
	vk::CommandPool command_pool;

	littlevk::Deallocator deallocator;

	std::map <std::string, littlevk::Image> image_cache;

	App();
};

App::App()
{
	// Vulkan device extensions
	static const std::vector <const char *> EXTENSIONS {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev, EXTENSIONS);
	};

	phdev = littlevk::pick_physical_device(predicate);
	memory_properties = phdev.getMemoryProperties();

	// Create an application skeleton with the bare minimum
        skeletonize(phdev, { 800, 600 }, "Model Viewer", EXTENSIONS);

	// Auto deallocation system
	deallocator = littlevk::Deallocator { device };

	// Command pool
	command_pool = littlevk::command_pool(device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	).unwrap(deallocator);
}

std::optional <littlevk::Image> load_texture(App &app, const std::filesystem::path &path)
{
	if (app.image_cache.count(path.string()))
		return app.image_cache.at(path.string());

	littlevk::Image image;

	int width;
	int height;
	int channels;

	uint8_t *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
	if (pixels) {
		printf(CLEAR_LINE "Loaded albedo texture %s with resolution of %d x %d pixels",
			path.c_str(), width, height);

		// Prepare a staging buffder
		littlevk::Buffer staging_buffer;

		std::tie(image, staging_buffer) = bind(app.device, app.memory_properties, app.deallocator)
			.image(width, height,
				vk::Format::eR8G8B8A8Unorm,
				vk::ImageUsageFlagBits::eSampled
					| vk::ImageUsageFlagBits::eTransferDst,
				vk::ImageAspectFlagBits::eColor)
			.buffer(pixels, sizeof(uint32_t) * width * height, vk::BufferUsageFlagBits::eTransferSrc);

		// TODO: bind function
		littlevk::submit_now(app.device, app.command_pool, app.graphics_queue,
			[&](const vk::CommandBuffer &cmd) {
				littlevk::transition(cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
				littlevk::copy_buffer_to_image(cmd, image, staging_buffer, vk::ImageLayout::eTransferDstOptimal);
				littlevk::transition(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
			}
		);

		// Free interim data
		littlevk::destroy_buffer(app.device, staging_buffer);
		stbi_image_free(pixels);

		app.image_cache[path.string()] = image;
	} else {
		return {};
	}

	return image;
}

// TODO: destructor
void destroy_app(App &app)
{
	// First wait for all operations to finish
	app.device.waitIdle();

	// Release all automatically collected references
	app.deallocator.drop();

	// Destroy the application skeleton
	app.drop();
}

// TODO: app method
VulkanMesh vulkan_mesh(App &app, const Mesh &mesh)
{
	// Create the Vulkan mesh
	VulkanMesh vk_mesh;

	vk_mesh.index_count = mesh.indices.size();
	vk_mesh.has_texture = false;

	// Buffers
	std::tie(vk_mesh.vertex_buffer, vk_mesh.index_buffer) = bind(app.device, app.memory_properties, app.deallocator)
		.buffer(mesh.vertices, vk::BufferUsageFlagBits::eVertexBuffer)
		.buffer(mesh.indices, vk::BufferUsageFlagBits::eIndexBuffer);

	// Images
	if (!mesh.albedo_path.empty()) {
		auto image = load_texture(app, mesh.albedo_path);

		if (image.has_value()) {
			vk_mesh.albedo_image = image.value();
			vk_mesh.albedo_sampler = littlevk::SamplerAssembler(app.device, app.deallocator);
			vk_mesh.has_texture = true;
		} else {
			printf(CLEAR_LINE "Failed to load albedo texture %s", mesh.albedo_path.c_str());
		}
	}

	// Other material properties
	vk_mesh.albedo_color = glm::vec4(mesh.albedo_color, 1.0f);

	return vk_mesh;
}

int main(int argc, char *argv[])
{
	using standalone::readfile;

	// Process the arguments
	ArgParser argparser { "example-model-viewer", 1, {
		ArgParser::Option { "filename", "Input model" },
	}};

	argparser.parse(argc, argv);

	std::filesystem::path path;
	path = argparser.get <std::string> (0);
	path = std::filesystem::weakly_canonical(path);

	// Load the mesh
	Model model = load_model(path);
	printf("Loaded model with %lu meshes\n", model.size());

	// Precompute some data for rendering
	glm::vec3 center = glm::vec3(0.0f);
	glm::vec3 min = glm::vec3(FLT_MAX);
	glm::vec3 max = glm::vec3(-FLT_MAX);

	float count = 0.0f;
	for (const auto &mesh : model) {
		for (const Vertex &vertex : mesh.vertices) {
			center += vertex.position;
			min = glm::min(min, vertex.position);
			max = glm::max(max, vertex.position);
			count++;
		}
	}

	center /= count;

	// Initialize the rendering backend
	App app;

	// Create a render pass
	vk::RenderPass render_pass = littlevk::RenderPassAssembler(app.device, app.deallocator)
		.add_attachment(littlevk::default_color_attachment(app.swapchain.format))
		.add_attachment(littlevk::default_depth_attachment())
		.add_subpass(vk::PipelineBindPoint::eGraphics)
			.color_attachment(0, vk::ImageLayout::eColorAttachmentOptimal)
			.depth_attachment(1, vk::ImageLayout::eDepthStencilAttachmentOptimal)
			.done();

	// Create a depth buffer
	littlevk::Image depth_buffer = bind(app.device, app.memory_properties, app.deallocator)
		.image(app.window.extent,
			vk::Format::eD32Sfloat,
			vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::ImageAspectFlagBits::eDepth);

	// Create framebuffers from the swapchain
	littlevk::FramebufferGenerator generator(app.device, render_pass, app.window.extent, app.deallocator);
	for (const auto &view : app.swapchain.image_views)
		generator.add(view, depth_buffer.view);

	std::vector <vk::Framebuffer> framebuffers = generator.unpack();

	// Allocate command buffers
	auto command_buffers = app.device.allocateCommandBuffers({
		app.command_pool, vk::CommandBufferLevel::ePrimary, 2
	});

	// Allocate mesh resources
	std::vector <VulkanMesh> vk_meshes;
	for (const auto &mesh : model) {
		VulkanMesh vk_mesh = vulkan_mesh(app, mesh);
		vk_meshes.push_back(vk_mesh);
	}

	printf("\nAllocated %lu meshes\n", vk_meshes.size());

	// Descriptor pool allocation; just enough for all meshes
	vk::DescriptorPoolSize pool_sizes {
		vk::DescriptorType::eCombinedImageSampler,
		(uint32_t) model.size()
	};

	vk::DescriptorPool descriptor_pool = littlevk::descriptor_pool(
		app.device, vk::DescriptorPoolCreateInfo {
			{}, (uint32_t) model.size(),
			pool_sizes
		}
	).unwrap(app.deallocator);

	// Create the graphics pipelines
	struct MVP {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;

		alignas(16) glm::vec3 light_direction;
		alignas(16) glm::vec3 albedo_color;
	};

	constexpr std::array <vk::DescriptorSetLayoutBinding, 1> textured_dslbs {
		{{ 0, vk::DescriptorType::eCombinedImageSampler,
			 1, vk::ShaderStageFlagBits::eFragment }}
	};

	auto vertex_layout = littlevk::VertexLayout <littlevk::rgb32f, littlevk::rgb32f, littlevk::rg32f> ();

	auto textured_bundle = littlevk::ShaderStageBundle(app.device, app.deallocator)
		.source(readfile(SHADERS_DIRECTORY "/model_viewer.vert"), vk::ShaderStageFlagBits::eVertex)
		.source(readfile(SHADERS_DIRECTORY "/model_viewer_textured.frag"), vk::ShaderStageFlagBits::eFragment);

	auto default_bundle = littlevk::ShaderStageBundle(app.device, app.deallocator)
		.source(readfile(SHADERS_DIRECTORY "/model_viewer.vert"), vk::ShaderStageFlagBits::eVertex)
		.source(readfile(SHADERS_DIRECTORY "/model_viewer_default.frag"), vk::ShaderStageFlagBits::eFragment);

	littlevk::Pipeline textured_ppl = littlevk::PipelineAssembler <littlevk::eGraphics> (app.device, app.window, app.deallocator)
		.with_render_pass(render_pass, 0)
		.with_vertex_layout(vertex_layout)
		.with_shader_bundle(textured_bundle)
		.with_dsl_bindings(textured_dslbs)
		.with_push_constant <MVP> (vk::ShaderStageFlagBits::eVertex);

	littlevk::Pipeline default_ppl = littlevk::PipelineAssembler <littlevk::eGraphics> (app.device, app.window, app.deallocator)
		.with_render_pass(render_pass, 0)
		.with_vertex_layout(vertex_layout)
		.with_shader_bundle(default_bundle)
		.with_push_constant <MVP> (vk::ShaderStageFlagBits::eVertex);

	// Link descriptor sets
	// TODO: Needs a proper storage mechanism to offload without memory issues
	// std::vector <vk::WriteDescriptorSet> writes;

	for (auto &vk_mesh : vk_meshes) {
		if (!vk_mesh.has_texture) {
			if (glm::length(vk_mesh.albedo_color) < 1e-6f)
				vk_mesh.albedo_color = glm::vec3 { 0.5f, 0.8f, 0.8f };

			continue;
		}

		// Allocate a descriptor set for each mesh...
		vk_mesh.descriptor_set = littlevk::bind(app.device, descriptor_pool)
			.allocate_descriptor_sets(*textured_ppl.dsl).front();

		littlevk::bind(app.device, vk_mesh.descriptor_set, textured_dslbs)
			.update(0, 0, vk_mesh.albedo_sampler, vk_mesh.albedo_image.view, vk::ImageLayout::eShaderReadOnlyOptimal)
			// .offload(writes);
			.finalize();
	}

	// app.device.updateDescriptorSets(writes, nullptr);

	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(app.deallocator);

	// Prepare camera and model matrices
	g_state.center = center;
	g_state.radius = glm::length(max - min);
	rotate_view(0.0f, 0.0f);

	// Pre render items
	bool pause_rotate = false;
	bool pause_resume_pressed = false;

	float previous_time = 0.0f;
	float current_time = 0.0f;

	// Mouse actions
	glfwSetMouseButtonCallback(app.window.handle, mouse_callback);
	glfwSetCursorPosCallback(app.window.handle, cursor_callback);
	glfwSetScrollCallback(app.window.handle, scroll_callback);

	// Resize callback
	auto resize = [&]() {
		app.resize();

		// Recreate the depth buffer
		littlevk::Image depth_buffer = bind(app.device, app.memory_properties, app.deallocator)
			.image(app.window.extent,
				vk::Format::eD32Sfloat,
				vk::ImageUsageFlagBits::eDepthStencilAttachment,
				vk::ImageAspectFlagBits::eDepth);

		// Rebuid the framebuffers
		generator.extent = app.window.extent;
		for (const auto &view : app.swapchain.image_views)
			generator.add(view);

		framebuffers = generator.unpack();
	};

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();

		// Event handling
                if (glfwWindowShouldClose(app.window.handle))
                        break;

		// Pause/resume rotation
		if (glfwGetKey(app.window.handle, GLFW_KEY_SPACE) == GLFW_PRESS) {
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

		// Record command buffer
		const auto &cmd = command_buffers[frame];
		cmd.begin(vk::CommandBufferBeginInfo {});

		// Set viewport and scissor
		littlevk::viewport_and_scissor(cmd, littlevk::RenderArea(app.window));

		littlevk::RenderPassBeginInfo(2)
			.with_render_pass(render_pass)
			.with_framebuffer(framebuffers[op.index])
			.with_extent(app.window.extent)
			.clear_color(0, std::array <float, 4> { 0, 0, 0, 0 })
			.clear_depth(1, 1)
			.begin(cmd);

		// Render the triangle
		MVP push_constants;

		// Rotate the model matrix
		push_constants.model = glm::mat4 { 1.0f };
		push_constants.view = g_state.view;
		push_constants.proj = glm::perspective(glm::radians(45.0f), app.aspect_ratio(),
			0.1f, 100.0f * glm::length(max - min));

		push_constants.light_direction = glm::normalize(glm::vec3 { 1.0f, 1.0f, 1.0f });

		for (auto &vk_mesh : vk_meshes) {
			push_constants.albedo_color = vk_mesh.albedo_color;

			if (vk_mesh.has_texture) {
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, textured_ppl.handle);
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, textured_ppl.layout, 0, vk_mesh.descriptor_set, {});
				cmd.pushConstants <MVP> (textured_ppl.layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
			} else {
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, default_ppl.handle);
				cmd.pushConstants <MVP> (default_ppl.layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
			}

			cmd.bindVertexBuffers(0, vk_mesh.vertex_buffer.buffer, { 0 });
			cmd.bindIndexBuffer(vk_mesh.index_buffer.buffer, 0, vk::IndexType::eUint32);
			cmd.drawIndexed(vk_mesh.index_count, 1, 0, 0, 0);
		}

		cmd.endRenderPass();
		cmd.end();

		// Submit command buffer while signaling the semaphore
		constexpr vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

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

	destroy_app(app);
	return 0;
}

// Mouse callbacks
void rotate_view(double dx, double dy)
{
	g_state.phi += dx * 0.01;
	g_state.theta += dy * 0.01;

	g_state.theta = glm::clamp(g_state.theta, -glm::half_pi <double> (), glm::half_pi <double> ());

	glm::vec3 direction {
		cos(g_state.phi) * cos(g_state.theta),
		sin(g_state.theta),
		sin(g_state.phi) * cos(g_state.theta)
	};

	float r = g_state.radius * g_state.radius_scale;
	g_state.view = glm::lookAt(
		g_state.center + r * direction,
		g_state.center, glm::vec3(0.0, 1.0, 0.0)
	);
}

void mouse_callback(GLFWwindow *window, int button, int action, int mods)
{
	if (button == GLFW_MOUSE_BUTTON_LEFT) {
		if (action == GLFW_PRESS)
			g_state.left_dragging = true;
		else if (action == GLFW_RELEASE)
			g_state.left_dragging = false;
	}

	if (button == GLFW_MOUSE_BUTTON_RIGHT) {
		if (action == GLFW_PRESS)
			g_state.right_dragging = true;
		else if (action == GLFW_RELEASE)
			g_state.right_dragging = false;
	}
};

void cursor_callback(GLFWwindow *window, double xpos, double ypos)
{
	double dx = xpos - g_state.last_x;
	double dy = ypos - g_state.last_y;

	if (g_state.left_dragging)
		rotate_view(dx, dy);

	if (g_state.right_dragging) {
		glm::mat4 view = glm::inverse(g_state.view);
		glm::vec3 right = view * glm::vec4(1.0, 0.0, 0.0, 0.0);
		glm::vec3 up = view * glm::vec4(0.0, 1.0, 0.0, 0.0);

		float r = g_state.radius * g_state.radius_scale;
		g_state.center -= float(dx) * right * r * 0.001f;
		g_state.center += float(dy) * up * r * 0.001f;
		rotate_view(0.0, 0.0);
	}

	g_state.last_x = xpos;
	g_state.last_y = ypos;
}

void scroll_callback(GLFWwindow *window, double xoffset, double yoffset)
{
	g_state.radius_scale -= yoffset * 0.1f;
	g_state.radius_scale = glm::clamp(g_state.radius_scale, 0.1f, 10.0f);
	rotate_view(0.0, 0.0);
}

Mesh process_mesh(aiMesh *mesh, const aiScene *scene, const std::filesystem::path &directory)
{
	// Mesh data
	std::vector <Vertex> vertices;
	std::vector <uint32_t> triangles;

	// Process all the mesh's vertices
	for (size_t i = 0; i < mesh->mNumVertices; i++) {
		Vertex v;

		v.position = {
			mesh->mVertices[i].x,
			mesh->mVertices[i].y,
			mesh->mVertices[i].z
		};

		if (mesh->HasNormals()) {
			v.normal = {
				mesh->mNormals[i].x,
				mesh->mNormals[i].y,
				mesh->mNormals[i].z
			};
		}

		if (mesh->HasTextureCoords(0)) {
			v.uv = {
				mesh->mTextureCoords[0][i].x,
				mesh->mTextureCoords[0][i].y
			};
		}

		vertices.push_back(v);
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

	Mesh new_mesh { vertices, triangles };

	// Process materials
	aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];

	// Get albedo, specular and shininess
	aiVector3D albedo;
	material->Get(AI_MATKEY_COLOR_DIFFUSE, albedo);
	new_mesh.albedo_color = glm::vec3 { albedo.x, albedo.y, albedo.z };

	// Load diffuse/albedo texture
	aiString path;
	if (material->GetTexture(aiTextureType_DIFFUSE, 0, &path) == AI_SUCCESS) {
		std::string path_string = path.C_Str();
		std::replace(path_string.begin(), path_string.end(), '\\', '/');
		std::filesystem::path texture_path = path_string;
		new_mesh.albedo_path = directory / texture_path;
	}

	return new_mesh;
}

Model process_node(aiNode *node, const aiScene *scene, const std::filesystem::path &directory)
{
	Model model;

	for (size_t i = 0; i < node->mNumMeshes; i++) {
		aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
                Mesh processed_mesh = process_mesh(mesh, scene, directory);
		if (processed_mesh.indices.size() > 0)
			model.push_back(processed_mesh);
	}

	// Recusively process all the node's children
	for (size_t i = 0; i < node->mNumChildren; i++) {
		Model processed_models = process_node(node->mChildren[i], scene, directory);
		if (processed_models.size() > 0)
			model.insert(model.end(), processed_models.begin(), processed_models.end());
	}

	return model;
}

Model load_model(const std::filesystem::path &path)
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
	return process_node(scene->mRootNode, scene, path.parent_path());
}
