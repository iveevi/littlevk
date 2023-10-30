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

// TODO: pass from CMake
#ifndef EXAMPLES_DIRETORY
#define EXAMPLES_DIRECTORY ".."
#endif

// Argument parsing
// TODO: plus a logging utility... (and mesh utility)
// TODO: remove this...
#include "argparser.hpp"

// Vertex data
struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec2 uv;

	static constexpr vk::VertexInputBindingDescription binding() {
		return vk::VertexInputBindingDescription {
			0, sizeof(Vertex), vk::VertexInputRate::eVertex
		};
	}

	static constexpr std::array <vk::VertexInputAttributeDescription, 3> attributes() {
		return {
			vk::VertexInputAttributeDescription {
				0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position)
			},
			vk::VertexInputAttributeDescription {
				1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal)
			},
			vk::VertexInputAttributeDescription {
				2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv)
			},
		};
	}
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

#define CLEAR_LINE "\r\033[K"

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream file(path);
	if (!file.is_open())
		throw std::runtime_error("failed to open file " + path.string());

	std::string contents;
	file.seekg(0, std::ios::end);
	contents.resize(file.tellg());
	file.seekg(0, std::ios::beg);
	file.read(contents.data(), contents.size());
	file.close();

	return contents;
}

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

	littlevk::Deallocator *deallocator = nullptr;
	
	vk::CommandPool command_pool;

	std::map <std::string, littlevk::Image> image_cache;
};

// TODO: just use a regular constructor...
App make_app()
{
	App app;

	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev, {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		});
	};

	app.phdev = littlevk::pick_physical_device(predicate);
	app.memory_properties = app.phdev.getMemoryProperties();
	
	// Create an application skeleton with the bare minimum
        app.skeletonize(app.phdev, { 800, 600 }, "Model Viewer");
	
	// Add the rest of the application
	app.deallocator = new littlevk::Deallocator { app.device };

	app.command_pool = littlevk::command_pool(app.device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(app.phdev)
		}
	).unwrap(app.deallocator);

	return app;
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

		image = littlevk::image(app.device, {
			(uint32_t) width, (uint32_t) height,
			vk::Format::eR8G8B8A8Unorm,
			vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
			vk::ImageAspectFlagBits::eColor
		}, app.memory_properties).unwrap(app.deallocator);

		// Upload the image data
		littlevk::Buffer staging_buffer = littlevk::buffer(
			app.device,
			4 * width * height,
			vk::BufferUsageFlagBits::eTransferSrc,
			app.memory_properties
		).value;

		littlevk::upload(app.device, staging_buffer, pixels);

		// TODO: some state wise struct to simplify transitioning?
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

void destroy_app(App &app)
{
	// First wait for all operations to finish
	app.device.waitIdle();

	delete app.deallocator;

	// Destroy the application skeleton
	app.destroy();
}

littlevk::ComposedReturnProxy <VulkanMesh> vulkan_mesh(App &app, const Mesh &mesh)
{
	static constexpr vk::SamplerCreateInfo default_sampler_info {
		vk::SamplerCreateFlags {},
		vk::Filter::eLinear,
		vk::Filter::eLinear,
		vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eRepeat,
		vk::SamplerAddressMode::eRepeat,
		vk::SamplerAddressMode::eRepeat,
		0.0f,
		VK_FALSE,
		1.0f,
		VK_FALSE,
		vk::CompareOp::eAlways,
		0.0f,
		0.0f,
		vk::BorderColor::eIntOpaqueBlack,
		VK_FALSE
	};

	// Create the Vulkan mesh
	VulkanMesh vk_mesh;

	vk_mesh.index_count = mesh.indices.size();
	vk_mesh.has_texture = false;

	littlevk::DeallocationQueue queue;

	// Buffers
	vk_mesh.vertex_buffer = littlevk::buffer(
		app.device,
		mesh.vertices.size() * sizeof(Vertex),
		vk::BufferUsageFlagBits::eVertexBuffer,
		app.memory_properties
	).defer(queue);

	vk_mesh.index_buffer = littlevk::buffer(
		app.device,
		mesh.indices.size() * sizeof(uint32_t),
		vk::BufferUsageFlagBits::eIndexBuffer,
		app.memory_properties
	).defer(queue);

	littlevk::upload(app.device, vk_mesh.vertex_buffer, mesh.vertices);
	littlevk::upload(app.device, vk_mesh.index_buffer, mesh.indices);

	// Images
	if (!mesh.albedo_path.empty()) {
		auto image = load_texture(app, mesh.albedo_path);

		if (image.has_value()) {
			vk_mesh.albedo_image = image.value();
			vk_mesh.albedo_sampler = littlevk::sampler(app.device, default_sampler_info).defer(queue);
			vk_mesh.has_texture = true;
		} else {
			printf(CLEAR_LINE "Failed to load albedo texture %s", mesh.albedo_path.c_str());
		}
	}

	// Other material properties
	vk_mesh.albedo_color = glm::vec4(mesh.albedo_color, 1.0f);

	return { vk_mesh, queue };
}

void link_descriptor_set(const vk::Device &device, const vk::DescriptorPool &pool, const vk::DescriptorSetLayout &layout, VulkanMesh &vk_mesh)
{
	// Allocate a descriptor set
	vk_mesh.descriptor_set = device.allocateDescriptorSets({
		pool, 1, &layout
	}).front();

	// Binding 0: albedo texture
	// TODO: littlevk utility for this?
	vk::DescriptorImageInfo image_info {
		vk_mesh.albedo_sampler,
		vk_mesh.albedo_image.view,
		vk::ImageLayout::eShaderReadOnlyOptimal
	};

	vk::WriteDescriptorSet write {
		vk_mesh.descriptor_set,
		0, 0, 1,
		vk::DescriptorType::eCombinedImageSampler,
		&image_info,
		nullptr, nullptr
	};

	device.updateDescriptorSets(write, nullptr);
}

int main(int argc, char *argv[])
{
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

	// Initialize rendering backend
	App app = make_app();

	// Create a render pass
	std::array <vk::AttachmentDescription, 2> attachments {
		// TODO: same thing for samplers...
		littlevk::default_color_attachment(app.swapchain.format),
		littlevk::default_depth_attachment(),
	};

	std::array <vk::AttachmentReference, 1> color_attachments {
		vk::AttachmentReference {
			0,
			vk::ImageLayout::eColorAttachmentOptimal,
		}
	};

	vk::AttachmentReference depth_attachment {
		1, vk::ImageLayout::eDepthStencilAttachmentOptimal,
	};

	vk::SubpassDescription subpass {
		{}, vk::PipelineBindPoint::eGraphics,
		{}, color_attachments,
		{}, &depth_attachment
	};

	vk::RenderPass render_pass = littlevk::render_pass(
		app.device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	).unwrap(app.deallocator);

	// Create a depth buffer
	littlevk::ImageCreateInfo depth_info {
		app.window->extent.width,
		app.window->extent.height,
		vk::Format::eD32Sfloat,
		vk::ImageUsageFlagBits::eDepthStencilAttachment,
		vk::ImageAspectFlagBits::eDepth,
	};

	littlevk::Image depth_buffer = littlevk::image(
		app.device,
		depth_info,
		app.memory_properties
	).unwrap(app.deallocator);

	// Create framebuffers from the swapchain
	littlevk::FramebufferSetInfo fb_info;
	fb_info.swapchain = &app.swapchain;
	fb_info.render_pass = render_pass;
	fb_info.extent = app.window->extent;
	fb_info.depth_buffer = &depth_buffer.view;

	auto framebuffers = littlevk::framebuffers(app.device, fb_info).unwrap(app.deallocator);

	// Allocate command buffers
	auto command_buffers = app.device.allocateCommandBuffers({
		app.command_pool, vk::CommandBufferLevel::ePrimary, 2
	});

	// Allocate mesh resources
	// TODO: partition into meshes that have a texture and those that don't
	std::vector <VulkanMesh> vk_meshes;
	for (const auto &mesh : model) {
		VulkanMesh vk_mesh = vulkan_mesh(app, mesh).unwrap(app.deallocator);
		vk_meshes.push_back(vk_mesh);
	}

	printf("\nAllocated %lu meshes\n", vk_meshes.size());

	// Compile shader modules
	vk::ShaderModule vertex_module = littlevk::shader::compile(
		app.device, read_file(EXAMPLES_DIRECTORY "/shaders/model_viewer.vert"),
		vk::ShaderStageFlagBits::eVertex
	).unwrap(app.deallocator);

	vk::ShaderModule textured_fragment_module = littlevk::shader::compile(
		app.device, read_file(EXAMPLES_DIRECTORY "/shaders/model_viewer_textured.frag"),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(app.deallocator);

	vk::ShaderModule default_fragment_module = littlevk::shader::compile(
		app.device, read_file(EXAMPLES_DIRECTORY "/shaders/model_viewer_default.frag"),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(app.deallocator);

	// Descriptor pool allocation
	vk::DescriptorPoolSize pool_size {
		vk::DescriptorType::eCombinedImageSampler, (uint32_t) model.size()
	};

	vk::DescriptorPool descriptor_pool = littlevk::descriptor_pool(
		app.device, vk::DescriptorPoolCreateInfo {
			{}, (uint32_t) model.size(), pool_size
		}
	).unwrap(app.deallocator);

	// Descriptor set layout for the textured meshes
	vk::DescriptorSetLayoutBinding render_binding {
		0, vk::DescriptorType::eCombinedImageSampler,
		1, vk::ShaderStageFlagBits::eFragment
	};

	vk::DescriptorSetLayout render_layout = littlevk::descriptor_set_layout(
		app.device, vk::DescriptorSetLayoutCreateInfo {
			{}, render_binding
		}
	).unwrap(app.deallocator);

	// Create the graphics pipelines
	struct PushConstants {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;

		// alignas(16) glm::vec3 view_position;
		alignas(16) glm::vec3 light_direction;
		alignas(16) glm::vec3 albedo_color;
	};

	// TODO: move the light using arrows?

	vk::PushConstantRange push_constant_range {
		vk::ShaderStageFlagBits::eVertex,
		0, sizeof(PushConstants)
	};

	vk::PipelineLayout textured_pipeline_layout = littlevk::pipeline_layout(
		app.device,
		vk::PipelineLayoutCreateInfo {
			{}, render_layout,
			push_constant_range
		}
	).unwrap(app.deallocator);

	vk::PipelineLayout default_pipeline_layout = littlevk::pipeline_layout(
		app.device,
		vk::PipelineLayoutCreateInfo {
			{}, {},
			push_constant_range
		}
	).unwrap(app.deallocator);

	littlevk::pipeline::GraphicsCreateInfo pipeline_info;
	pipeline_info.vertex_binding = Vertex::binding();
	pipeline_info.vertex_attributes = Vertex::attributes();
	pipeline_info.vertex_shader = vertex_module;
	pipeline_info.extent = app.window->extent;
	pipeline_info.render_pass = render_pass;

	pipeline_info.fragment_shader = textured_fragment_module;
	pipeline_info.pipeline_layout = textured_pipeline_layout;
	
	vk::Pipeline textured_pipeline = littlevk::pipeline::compile(app.device, pipeline_info).unwrap(app.deallocator);

	pipeline_info.fragment_shader = default_fragment_module;
	pipeline_info.pipeline_layout = default_pipeline_layout;

	vk::Pipeline default_pipeline = littlevk::pipeline::compile(app.device, pipeline_info).unwrap(app.deallocator);

	// Link descriptor sets
	for (auto &vk_mesh : vk_meshes) {
		if (!vk_mesh.has_texture)
			continue;

		link_descriptor_set(app.device,
			descriptor_pool,
			render_layout,
			vk_mesh
		);
	}

	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(app.deallocator);

	// Prepare camera and model matrices
	glm::mat4 proj = glm::perspective(
		glm::radians(45.0f),
		app.window->extent.width / (float) app.window->extent.height,
		0.1f, 100.0f * glm::length(max - min)
	);

	g_state.center = center;
	g_state.radius = glm::length(max - min);
	rotate_view(0.0f, 0.0f);

	// Pre render items
	bool pause_rotate = false;
	bool pause_resume_pressed = false;

	float previous_time = 0.0f;
	float current_time = 0.0f;

	// Mouse actions
	glfwSetMouseButtonCallback(app.window->handle, mouse_callback);
	glfwSetCursorPosCallback(app.window->handle, cursor_callback);
	glfwSetScrollCallback(app.window->handle, scroll_callback);

	// Resize callback
	auto resize = [&]() {
		app.resize();

		// Recreate the depth buffer
		littlevk::ImageCreateInfo depth_info {
			app.window->extent.width,
			app.window->extent.height,
			vk::Format::eD32Sfloat,
			vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::ImageAspectFlagBits::eDepth,
		};

		littlevk::Image depth_buffer = littlevk::image(
			app.device,
			depth_info, app.memory_properties
		).unwrap(app.deallocator);

		// Rebuid the framebuffers
		fb_info.depth_buffer = &depth_buffer.view;
		fb_info.extent = app.window->extent;

		framebuffers = littlevk::framebuffers(app.device, fb_info).unwrap(app.deallocator);
	};

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();

		// Event handling
                if (glfwWindowShouldClose(app.window->handle))
                        break;

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

		// Render the triangle
		PushConstants push_constants;

		// Rotate the model matrix
		push_constants.model = glm::mat4 { 1.0f };
		push_constants.view = g_state.view;
		push_constants.proj = proj;

		push_constants.light_direction = glm::normalize(glm::vec3 { 1.0f, 1.0f, 1.0f });

		for (auto &vk_mesh : vk_meshes) {
			push_constants.albedo_color = vk_mesh.albedo_color;

			if (vk_mesh.has_texture) {
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, textured_pipeline);
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, textured_pipeline_layout, 0, vk_mesh.descriptor_set, {});
				cmd.pushConstants <PushConstants> (textured_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
			} else {
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, default_pipeline);
				cmd.pushConstants <PushConstants> (default_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
			}

			cmd.bindVertexBuffers(0, vk_mesh.vertex_buffer.buffer, { 0 });
			cmd.bindIndexBuffer(vk_mesh.index_buffer.buffer, 0, vk::IndexType::eUint32);
			cmd.drawIndexed(vk_mesh.index_count, 1, 0, 0, 0);
		}

		cmd.endRenderPass();
		cmd.end();

		// Submit command buffer while signaling the semaphore
		vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submit_info {
			1, &sync.image_available[frame],
			&wait_stage,
			1, &cmd,
			1, &sync.render_finished[frame]
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

// TODO: common mesh.hpp as a util.hpp?
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
	// TODO: multiple meshes...
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
