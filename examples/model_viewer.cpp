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

// Argument parsing
// TODO: plus a logging utility... (and mesh utility)
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

// TODO: put all the app data into a struct...

// Mesh and mesh loading
struct Mesh {
	std::vector <Vertex> vertices;
	std::vector <uint32_t> indices;

	std::filesystem::path albedo_path;
	glm::vec3 albedo_color;
};

using Model = std::vector <Mesh>;

Model load_model(const std::filesystem::path &);

// Translating CPU data to Vulkan resources
struct VulkanMesh {
	littlevk::Buffer vertex_buffer;
	littlevk::Buffer index_buffer;
	size_t index_count;

	littlevk::Image albedo_image;
	bool has_texture;

	vk::Sampler albedo_sampler;

	glm::vec3 albedo_color;

	vk::DescriptorSet descriptor_set;
};

std::map <std::string, littlevk::Image> image_cache;

#define CLEAR_LINE "\r\033[K"

std::optional <littlevk::Image> load_texture(const vk::PhysicalDevice &phdev, const vk::Device &device, littlevk::DeallocationQueue &queue, const vk::PhysicalDeviceMemoryProperties &mem_props, const std::filesystem::path &path)
{
	if (image_cache.count(path.string())) {
		printf(CLEAR_LINE "Found cached texture %s", path.c_str());
		return image_cache[path.string()];
	}

	littlevk::Image image;

	int width;
	int height;
	int channels;

	uint8_t *pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
	if (pixels) {
		printf(CLEAR_LINE "Loaded albedo texture %s with resolution of %d x %d pixels",
			path.c_str(), width, height);

		image = littlevk::image(device, {
			(uint32_t) width, (uint32_t) height,
			vk::Format::eR8G8B8A8Unorm,
			vk::ImageUsageFlagBits::eSampled
				| vk::ImageUsageFlagBits::eTransferDst,
			vk::ImageAspectFlagBits::eColor
		}, mem_props).defer(queue);

		// Upload the image data
		littlevk::Buffer staging_buffer = littlevk::buffer(
			device,
			4 * width * height,
			vk::BufferUsageFlagBits::eTransferSrc,
				// | vk::BufferUsageFlagBits::eTransferDst,
			mem_props
		).value;

		littlevk::upload(device, staging_buffer, pixels);

		// TODO: single time command buffer...
		// TODO: only need a queue family, do we even need the phdev?
		vk::CommandPool command_pool = littlevk::command_pool(device,
			vk::CommandPoolCreateInfo {
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
				littlevk::find_graphics_queue_family(phdev)
			}
		).value;

		auto command_buffer = device.allocateCommandBuffers({
			command_pool, vk::CommandBufferLevel::ePrimary, 1
		}).front();

		command_buffer.begin({
			vk::CommandBufferUsageFlagBits::eOneTimeSubmit
		});

		littlevk::transition_image_layout(command_buffer, image,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

		vk::BufferImageCopy region {
			0, 0, 0,
			vk::ImageSubresourceLayers {
				vk::ImageAspectFlagBits::eColor, 0, 0, 1
			},
			vk::Offset3D { 0, 0, 0 },
			vk::Extent3D {
				(uint32_t) width, (uint32_t) height, 1
			}
		};

		// TODO: buffer and image should have a * operator to directly access the vk::Buffer/vk::Image
		command_buffer.copyBufferToImage(staging_buffer.buffer, image.image,
			vk::ImageLayout::eTransferDstOptimal, region);

		littlevk::transition_image_layout(command_buffer, image,
			vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);

		command_buffer.end();

		// Submit the command buffer
		vk::Queue queue = device.getQueue(littlevk::find_graphics_queue_family(phdev), 0);

		queue.submit({
			vk::SubmitInfo {
				0, nullptr, nullptr,
				1, &command_buffer,
				0, nullptr
			}
		}, nullptr);

		queue.waitIdle();

		// Free interim data
		littlevk::destroy_command_pool(device, command_pool);
		littlevk::destroy_buffer(device, staging_buffer);
		stbi_image_free(pixels);

		image_cache[path.string()] = image;
	} else {
		return {};
	}

	return image;
}

littlevk::ComposedReturnProxy <VulkanMesh> vulkan_mesh(const vk::PhysicalDevice &phdev, const vk::Device &device, const Mesh &mesh, const vk::PhysicalDeviceMemoryProperties &mem_props)
{
	VulkanMesh vk_mesh;

	vk_mesh.index_count = mesh.indices.size();
	vk_mesh.has_texture = false;

	littlevk::DeallocationQueue queue;

	// Buffers
	vk_mesh.vertex_buffer = littlevk::buffer(
		device,
		mesh.vertices.size() * sizeof(Vertex),
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).defer(queue);

	vk_mesh.index_buffer = littlevk::buffer(
		device,
		mesh.indices.size() * sizeof(uint32_t),
		vk::BufferUsageFlagBits::eIndexBuffer,
		mem_props
	).defer(queue);

	littlevk::upload(device, vk_mesh.vertex_buffer, mesh.vertices);
	littlevk::upload(device, vk_mesh.index_buffer, mesh.indices);

	// Images
	if (!mesh.albedo_path.empty()) {
		auto image = load_texture(phdev, device, queue, mem_props, mesh.albedo_path);
		if (image.has_value()) {
			vk_mesh.albedo_image = image.value();

			// Allocate an image sampler
			// TODO: how do we simply this?
			// do we input a string version of this?
			// we still want the uers to have maximum control
			vk::SamplerCreateInfo sampler_info;

			sampler_info.magFilter = vk::Filter::eLinear;
			sampler_info.minFilter = vk::Filter::eLinear;
			sampler_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
			sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
			sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
			sampler_info.addressModeW = vk::SamplerAddressMode::eRepeat;
			sampler_info.mipLodBias = 0.0f;
			sampler_info.anisotropyEnable = VK_FALSE;
			sampler_info.maxAnisotropy = 1.0f;
			sampler_info.compareEnable = VK_FALSE;
			sampler_info.compareOp = vk::CompareOp::eAlways;
			sampler_info.minLod = 0.0f;
			sampler_info.maxLod = 0.0f;
			sampler_info.borderColor = vk::BorderColor::eIntOpaqueBlack;
			sampler_info.unnormalizedCoordinates = VK_FALSE;

			vk_mesh.albedo_sampler = device.createSampler(sampler_info);
			vk_mesh.has_texture = true;
		} else {
			printf(CLEAR_LINE "Failed to load albedo texture %s", mesh.albedo_path.c_str());
		}
	}

	// TODO: clear lines each time in this phase...

	// Other material properties
	vk_mesh.albedo_color = glm::vec4(mesh.albedo_color, 1.0f);

	return { vk_mesh, queue };
}

void link_descriptor_set(const vk::Device &device, const vk::DescriptorPool &pool, const vk::DescriptorSetLayout &layout, VulkanMesh &vk_mesh)
{
	// TODO: skip if no albedo texture

	// Allocate a descriptor set
	vk_mesh.descriptor_set = device.allocateDescriptorSets({
		pool, 1, &layout
	}).front();

	// Binding 0: albedo texture
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

// Shader sources
// TODO: push constants offset
// TODO: normal textures and basic phong lighting...
const std::string vertex_shader_source = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec2 uv;

layout (push_constant) uniform PushConstants {
	mat4 model;
	mat4 view;
	mat4 proj;

	vec3 albedo_color;
	vec3 light_direction;
};

layout (location = 0) out vec3 out_normal;
layout (location = 1) out vec2 out_uv;
layout (location = 2) out vec3 out_albedo_color;
layout (location = 3) out vec3 out_light_direction;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;

	mat3 mv = mat3(view * model);

	out_normal = normalize(mv * normal);
	out_uv = uv;
	out_albedo_color = albedo_color;
	out_light_direction = normalize(light_direction);
}
)";

// TODO: add back diffuse lighting (and specular as well...)
const std::string textured_fragment_shader_source = R"(
#version 450

layout (location = 0) in vec3 in_normal;
layout (location = 1) in vec2 in_uv;
layout (location = 3) in vec3 in_light_direction;

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
)";

const std::string default_fragment_shader_source = R"(
#version 450

layout (location = 0) in vec3 in_normal;
layout (location = 2) in vec3 in_albedo_color;
layout (location = 3) in vec3 in_light_direction;

layout (location = 0) out vec4 fragment;

void main()
{
	float lambertian = max(dot(in_normal, in_light_direction), 0.0);
	vec3 diffuse = in_albedo_color * lambertian;
	vec3 ambient = in_albedo_color * 0.1;
	fragment = vec4(diffuse + ambient, 1.0);
}
)";

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
} mouse_state;

void mouse_callback(GLFWwindow* window, int button, int action, int mods)
{
	if (button == GLFW_MOUSE_BUTTON_LEFT) {
		if (action == GLFW_PRESS)
			mouse_state.left_dragging = true;
		else if (action == GLFW_RELEASE)
			mouse_state.left_dragging = false;
	}

	if (button == GLFW_MOUSE_BUTTON_RIGHT) {
		if (action == GLFW_PRESS)
			mouse_state.right_dragging = true;
		else if (action == GLFW_RELEASE)
			mouse_state.right_dragging = false;
	}
};

void rotate_view(double dx, double dy)
{
	mouse_state.phi += dx * 0.01;
	mouse_state.theta += dy * 0.01;

	mouse_state.theta = glm::clamp(mouse_state.theta, -glm::half_pi <double> (), glm::half_pi <double> ());

	glm::vec3 direction {
		cos(mouse_state.phi) * cos(mouse_state.theta),
		sin(mouse_state.theta),
		sin(mouse_state.phi) * cos(mouse_state.theta)
	};

	float r = mouse_state.radius * mouse_state.radius_scale;
	mouse_state.view = glm::lookAt(
		mouse_state.center + r * direction,
		mouse_state.center, glm::vec3(0.0, 1.0, 0.0)
	);
}

void cursor_callback(GLFWwindow* window, double xpos, double ypos)
{
	double dx = xpos - mouse_state.last_x;
	double dy = ypos - mouse_state.last_y;

	if (mouse_state.left_dragging)
		rotate_view(dx, dy);

	if (mouse_state.right_dragging) {
		glm::mat4 view = glm::inverse(mouse_state.view);
		glm::vec3 right = view * glm::vec4(1.0, 0.0, 0.0, 0.0);
		glm::vec3 up = view * glm::vec4(0.0, 1.0, 0.0, 0.0);

		float r = mouse_state.radius * mouse_state.radius_scale;
		mouse_state.center -= float(dx) * right * r * 0.001f;
		mouse_state.center += float(dy) * up * r * 0.001f;
		rotate_view(0.0, 0.0);
	}

	mouse_state.last_x = xpos;
	mouse_state.last_y = ypos;
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
	mouse_state.radius_scale -= yoffset * 0.1f;
	mouse_state.radius_scale = glm::clamp(mouse_state.radius_scale, 0.1f, 10.0f);
	rotate_view(0.0, 0.0);
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
	// TODO: compute total vertex/triangle count...
	// for (const auto &mesh : model) {
	// 	printf("  mesh with %lu vertices and %lu indices\n",
	// 		mesh.vertices.size(), mesh.indices.size());
	//
	// 	if (!mesh.albedo_path.empty())
	// 		printf("    albedo texture: %s\n", mesh.albedo_path.c_str());
	// }

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

	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev,  {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		});
	};

	vk::PhysicalDevice phdev = littlevk::pick_physical_device(predicate);
	vk::PhysicalDeviceMemoryProperties mem_props = phdev.getMemoryProperties();

	// Create an application skeleton with the bare minimum
	littlevk::ApplicationSkeleton *app = new littlevk::ApplicationSkeleton;
        make_application(app, phdev, { 800, 600 }, "Model Viewer");

	// Create a deallocator for automatic resource cleanup
	auto deallocator = new littlevk::Deallocator { app->device };

	// Create a render pass
	std::array <vk::AttachmentDescription, 2> attachments {
		vk::AttachmentDescription {
			{},
			app->swapchain.format,
			vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eStore,
			vk::AttachmentLoadOp::eDontCare,
			vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::ePresentSrcKHR,
		},
		vk::AttachmentDescription {
			{},
			vk::Format::eD32Sfloat,
			vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eDontCare,
			vk::AttachmentLoadOp::eDontCare,
			vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eDepthStencilAttachmentOptimal,
		}
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
		app->device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	).unwrap(deallocator);

	// Create a depth buffer
	littlevk::ImageCreateInfo depth_info {
		app->window->extent.width,
		app->window->extent.height,
		vk::Format::eD32Sfloat,
		vk::ImageUsageFlagBits::eDepthStencilAttachment,
		vk::ImageAspectFlagBits::eDepth,
	};

	littlevk::Image depth_buffer = littlevk::image(
		app->device,
		depth_info, mem_props
	).unwrap(deallocator);

	// Create framebuffers from the swapchain
	littlevk::FramebufferSetInfo fb_info;
	fb_info.swapchain = &app->swapchain;
	fb_info.render_pass = render_pass;
	fb_info.extent = app->window->extent;
	fb_info.depth_buffer = &depth_buffer.view;

	auto framebuffers = littlevk::framebuffers(app->device, fb_info).unwrap(deallocator);

	// Allocate command buffers
	vk::CommandPool command_pool = littlevk::command_pool(app->device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	).unwrap(deallocator);

	auto command_buffers = app->device.allocateCommandBuffers({
		command_pool, vk::CommandBufferLevel::ePrimary, 2
	});

	// Allocate mesh resources
	// TODO: partition into meshes that have a texture and those that don't
	// VulkanMesh vk_mesh = vulkan_mesh(phdev, app->device, mesh, mem_props).unwrap(deallocator);
	std::vector <VulkanMesh> vk_meshes;
	for (const auto &mesh : model) {
		VulkanMesh vk_mesh = vulkan_mesh(phdev, app->device, mesh, mem_props).unwrap(deallocator);
		vk_meshes.push_back(vk_mesh);
	}

	printf("\nAllocated %lu meshes\n", vk_meshes.size());

	// Compile shader modules
	vk::ShaderModule vertex_module = littlevk::shader::compile(
		app->device, vertex_shader_source,
		vk::ShaderStageFlagBits::eVertex
	).unwrap(deallocator);

	vk::ShaderModule textured_fragment_module = littlevk::shader::compile(
		app->device, textured_fragment_shader_source,
		vk::ShaderStageFlagBits::eFragment
	).unwrap(deallocator);
	
	vk::ShaderModule default_fragment_module = littlevk::shader::compile(
		app->device, default_fragment_shader_source,
		vk::ShaderStageFlagBits::eFragment
	).unwrap(deallocator);

	// Descriptor pool allocation
	vk::DescriptorPoolSize pool_size {
		vk::DescriptorType::eCombinedImageSampler, (uint32_t) model.size()
	};

	vk::DescriptorPool descriptor_pool = app->device.createDescriptorPool(
		vk::DescriptorPoolCreateInfo {
			{}, (uint32_t) model.size(), pool_size
		}
	);

	// Descriptor set layout for the textured meshes
	vk::DescriptorSetLayoutBinding render_binding {
		0, vk::DescriptorType::eCombinedImageSampler,
		1, vk::ShaderStageFlagBits::eFragment
	};

	vk::DescriptorSetLayout render_layout = app->device.createDescriptorSetLayout(
		vk::DescriptorSetLayoutCreateInfo {
			{}, render_binding
		}
	);

	// Create a graphics pipeline
	struct PushConstants {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;

		alignas(16) glm::vec3 albedo_color;
		alignas(16) glm::vec3 light_direction;
	};

	// TODO: move the light using arrows?

	vk::PushConstantRange push_constant_range {
		vk::ShaderStageFlagBits::eVertex,
		0, sizeof(PushConstants)
	};

	vk::PipelineLayout textured_pipeline_layout = littlevk::pipeline_layout(
		app->device,
		vk::PipelineLayoutCreateInfo {
			{}, render_layout,
			push_constant_range
		}
	).unwrap(deallocator);
	
	vk::PipelineLayout default_pipeline_layout = littlevk::pipeline_layout(
		app->device,
		vk::PipelineLayoutCreateInfo {
			{}, {},
			push_constant_range
		}
	).unwrap(deallocator);

	littlevk::pipeline::GraphicsCreateInfo textured_pipeline_info;
	textured_pipeline_info.vertex_binding = Vertex::binding();
	textured_pipeline_info.vertex_attributes = Vertex::attributes();
	textured_pipeline_info.vertex_shader = vertex_module;
	textured_pipeline_info.fragment_shader = textured_fragment_module;
	textured_pipeline_info.extent = app->window->extent;
	textured_pipeline_info.pipeline_layout = textured_pipeline_layout;
	textured_pipeline_info.render_pass = render_pass;
	
	littlevk::pipeline::GraphicsCreateInfo default_pipeline_info;
	default_pipeline_info.vertex_binding = Vertex::binding();
	default_pipeline_info.vertex_attributes = Vertex::attributes();
	default_pipeline_info.vertex_shader = vertex_module;
	default_pipeline_info.fragment_shader = default_fragment_module;
	default_pipeline_info.extent = app->window->extent;
	default_pipeline_info.pipeline_layout = default_pipeline_layout;
	default_pipeline_info.render_pass = render_pass;

	vk::Pipeline textured_pipeline = littlevk::pipeline::compile(app->device, textured_pipeline_info).unwrap(deallocator);
	vk::Pipeline default_pipeline = littlevk::pipeline::compile(app->device, default_pipeline_info).unwrap(deallocator);

	// Syncronization primitives
	auto sync = littlevk::make_present_syncronization(app->device, 2).unwrap(deallocator);

	// Prepare camera and model matrices
	glm::mat4 proj = glm::perspective(
		glm::radians(45.0f),
		app->window->extent.width / (float) app->window->extent.height,
		0.1f, 100.0f * glm::length(max - min)
	);

	mouse_state.center = center;
	mouse_state.radius = glm::length(max - min);
	rotate_view(0.0f, 0.0f);

	// Pre render items
	bool pause_rotate = false;
	bool pause_resume_pressed = false;

	float previous_time = 0.0f;
	float current_time = 0.0f;

	// Link descriptor sets
	for (auto &vk_mesh : vk_meshes) {
		if (!vk_mesh.has_texture)
			continue;

		link_descriptor_set(app->device,
			descriptor_pool,
			render_layout,
			vk_mesh
		);
	}

	// Mouse actions
	glfwSetMouseButtonCallback(app->window->handle, mouse_callback);
	glfwSetCursorPosCallback(app->window->handle, cursor_callback);
	glfwSetScrollCallback(app->window->handle, scroll_callback);

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();

		// Event handling
                if (glfwWindowShouldClose(app->window->handle))
                        break;

		// Pause/resume rotation
		if (glfwGetKey(app->window->handle, GLFW_KEY_SPACE) == GLFW_PRESS) {
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
                op = littlevk::acquire_image(app->device, app->swapchain.swapchain, sync, frame);

		// Start empty render pass
		std::array <vk::ClearValue, 2> clear_values {
			vk::ClearColorValue { std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f } },
			vk::ClearDepthStencilValue { 1.0f, 0 }
		};

		vk::RenderPassBeginInfo render_pass_info {
			render_pass, framebuffers[op.index],
			vk::Rect2D { {}, app->window->extent },
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
		push_constants.view = mouse_state.view;
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

		app->graphics_queue.submit(submit_info, sync.in_flight[frame]);

                op = littlevk::present_image(app->present_queue, app->swapchain.swapchain, sync, op.index);

		frame = 1 - frame;
		// TODO: resize function
        }

	// Finish all pending operations
	app->device.waitIdle();

	// Free resources using automatic deallocator
	delete deallocator;

        // Delete application
	littlevk::destroy_application(app);
        delete app;

	// TODO: address santizer to check leaks...

        return 0;
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
