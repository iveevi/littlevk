#include "littlevk.hpp"

// GLM for vector math
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Shader sources
const std::string vertex_shader_source = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 color;

layout (push_constant) uniform MVP {
	mat4 model;
	mat4 view;
	mat4 proj;
};

layout (location = 0) out vec3 frag_color;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;
	frag_color = color;
}
)";

const std::string fragment_shader_source = R"(
#version 450

layout (location = 0) in vec3 frag_color;
layout (location = 0) out vec4 out_color;

void main()
{
	out_color = vec4(frag_color, 1.0);
}
)";

// Unit cube data
static const std::vector <std::array <float, 6>> cube_vertex_data {
	// Front
	{ { -1.0f, -1.0f, -1.0f,  1.0f, 0.0f, 0.0f } },
	{ {  1.0f, -1.0f, -1.0f,  1.0f, 0.0f, 0.0f } },
	{ {  1.0f,  1.0f, -1.0f,  1.0f, 0.0f, 0.0f } },
	{ { -1.0f,  1.0f, -1.0f,  1.0f, 0.0f, 0.0f } },

	// Back
	{ { -1.0f, -1.0f,  1.0f,  0.0f, 1.0f, 0.0f } },
	{ {  1.0f, -1.0f,  1.0f,  0.0f, 1.0f, 0.0f } },
	{ {  1.0f,  1.0f,  1.0f,  0.0f, 1.0f, 0.0f } },
	{ { -1.0f,  1.0f,  1.0f,  0.0f, 1.0f, 0.0f } },

	// Left
	{ { -1.0f, -1.0f, -1.0f,  0.0f, 0.0f, 1.0f } },
	{ { -1.0f, -1.0f,  1.0f,  0.0f, 0.0f, 1.0f } },
	{ { -1.0f,  1.0f,  1.0f,  0.0f, 0.0f, 1.0f } },
	{ { -1.0f,  1.0f, -1.0f,  0.0f, 0.0f, 1.0f } },

	// Right
	{ {  1.0f, -1.0f, -1.0f,  1.0f, 1.0f, 0.0f } },
	{ {  1.0f, -1.0f,  1.0f,  1.0f, 1.0f, 0.0f } },
	{ {  1.0f,  1.0f,  1.0f,  1.0f, 1.0f, 0.0f } },
	{ {  1.0f,  1.0f, -1.0f,  1.0f, 1.0f, 0.0f } },

	// Top
	{ { -1.0f, -1.0f, -1.0f,  0.0f, 1.0f, 1.0f } },
	{ { -1.0f, -1.0f,  1.0f,  0.0f, 1.0f, 1.0f } },
	{ {  1.0f, -1.0f,  1.0f,  0.0f, 1.0f, 1.0f } },
	{ {  1.0f, -1.0f, -1.0f,  0.0f, 1.0f, 1.0f } },

	// Bottom
	{ { -1.0f,  1.0f, -1.0f,  1.0f, 0.0f, 1.0f } },
	{ { -1.0f,  1.0f,  1.0f,  1.0f, 0.0f, 1.0f } },
	{ {  1.0f,  1.0f,  1.0f,  1.0f, 0.0f, 1.0f } },
	{ {  1.0f,  1.0f, -1.0f,  1.0f, 0.0f, 1.0f } }
};

static const std::vector <uint32_t> cube_index_data {
	0, 1, 2,	2, 3, 0,	// Front
	4, 6, 5,	6, 4, 7,	// Back
	8, 10, 9,	10, 8, 11,	// Left
	12, 13, 14,	14, 15, 12,	// Right
	16, 17, 18,	18, 19, 16,	// Top
	20, 22, 21,	22, 20, 23	// Bottom
};

int main()
{
	// Vulkan device extensions
	static const std::vector <const char *> EXTENSIONS {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev, EXTENSIONS);
	};

	vk::PhysicalDevice phdev = littlevk::pick_physical_device(predicate);
	vk::PhysicalDeviceMemoryProperties memory_properties = phdev.getMemoryProperties();

	// Create an application skeleton with the bare minimum
	littlevk::Skeleton app;
	app.skeletonize(phdev, { 800, 600 }, "Spinning Cube", EXTENSIONS);

	// Create a deallocator for automatic resource cleanup
	auto deallocator = littlevk::Deallocator { app.device };

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
		.image(app.window.extent,
			vk::Format::eD32Sfloat,
			vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::ImageAspectFlagBits::eDepth);

	// Create framebuffers from the swapchain
	littlevk::FramebufferGenerator generator(app.device, render_pass, app.window.extent, deallocator);
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

	// Simoultaneously allocate vertex and index buffers
	littlevk::Buffer vertex_buffer;
	littlevk::Buffer index_buffer;

	std::tie(vertex_buffer, index_buffer) = bind(app.device, memory_properties, deallocator)
		.buffer(cube_vertex_data, vk::BufferUsageFlagBits::eVertexBuffer)
		.buffer(cube_index_data, vk::BufferUsageFlagBits::eIndexBuffer);

	// Create a graphics pipeline
	struct MVP {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
	};

	auto vertex_layout = littlevk::VertexLayout <littlevk::rgb32f, littlevk::rgb32f> ();

	auto bundle = littlevk::ShaderStageBundle(app.device, deallocator)
		.source(vertex_shader_source, vk::ShaderStageFlagBits::eVertex)
		.source(fragment_shader_source, vk::ShaderStageFlagBits::eFragment);

	littlevk::Pipeline ppl = littlevk::PipelineAssembler <littlevk::eGraphics> (app.device, app.window, deallocator)
		.with_render_pass(render_pass, 0)
		.with_vertex_layout(vertex_layout)
		.with_shader_bundle(bundle)
		.with_push_constant <MVP> (vk::ShaderStageFlagBits::eVertex);

	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(deallocator);

	// Prepare camera and model matrices
	glm::mat4 view = glm::lookAt(
		glm::vec3 { 0.0f, 0.0f, 5.0f },
		glm::vec3 { 0.0f, 0.0f, 0.0f },
		glm::vec3 { 0.0f, 1.0f, 0.0f }
	);

	// Resize callback
	auto resize = [&]() {
		app.resize();

		// Recreate the depth buffer
		littlevk::Image depth_buffer = bind(app.device, memory_properties, deallocator)
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
                if (glfwWindowShouldClose(app.window.handle))
                        break;

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
		littlevk::viewport_and_scissor(command_buffers[frame], littlevk::RenderArea(app.window));

		littlevk::RenderPassBeginInfo(2)
			.with_render_pass(render_pass)
			.with_framebuffer(framebuffers[op.index])
			.with_extent(app.window.extent)
			.clear_color(0, std::array <float, 4> { 0, 0, 0, 0 })
			.clear_depth(1, 1)
			.begin(cmd);

		// Render the triangle
		glm::mat4 model = glm::mat4 { 1.0f };
		glm::mat4 proj = glm::perspective(glm::radians(45.0f), app.aspect_ratio(), 0.1f, 10.0f);

		// Rotate the model matrix
		model = glm::rotate(model, (float) glfwGetTime() * glm::radians(90.0f), glm::vec3 { 0.0f, 1.0f, 0.0f });

		MVP push_constants { model, view, proj };

		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ppl.handle);
		cmd.pushConstants <MVP> (ppl.layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
		cmd.bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		cmd.bindIndexBuffer(index_buffer.buffer, 0, vk::IndexType::eUint32);
		cmd.drawIndexed(cube_index_data.size(), 1, 0, 0, 0);

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

	// Finish all pending operations
	app.device.waitIdle();

	// Free resources using automatic deallocator
	deallocator.drop();

        // Delete application
	app.drop();

	// TODO: address santizer to check leaks...
        return 0;
}
