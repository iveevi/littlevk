#include "littlevk.hpp"

// Shader sources
const std::string vertex_shader_source = R"(
#version 450

layout (location = 0) in vec2 position;
layout (location = 1) in vec3 color;

layout (location = 0) out vec3 frag_color;

void main() {
	gl_Position = vec4(position, 0.0, 1.0);
	frag_color = color;
}
)";

const std::string fragment_shader_source = R"(
#version 450

layout (location = 0) in vec3 frag_color;
layout (location = 0) out vec4 out_color;

void main() {
	out_color = vec4(frag_color, 1.0);
}
)";

// Vertex buffer; position (2) and color (3)
constexpr float triangles[][5] {
	{  0.0f, -0.5f, 1.0f, 0.0f, 0.0f },
	{  0.5f,  0.5f, 0.0f, 1.0f, 0.0f },
	{ -0.5f,  0.5f, 0.0f, 0.0f, 1.0f },
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
	app.skeletonize(phdev, { 800, 600 }, "Hello Triangle", EXTENSIONS);

	// Create a deallocator for automatic resource cleanup
	auto deallocator = littlevk::Deallocator { app.device };

	// Create a render pass
	vk::RenderPass render_pass = littlevk::RenderPassAssembler(app.device, deallocator)
		.add_attachment(littlevk::default_color_attachment(app.swapchain.format))
		.add_subpass(vk::PipelineBindPoint::eGraphics)
			.color_attachment(0, vk::ImageLayout::eColorAttachmentOptimal)
			.done();

	// Create framebuffers from the swapchain
	littlevk::FramebufferGenerator generator(app.device, render_pass, app.window.extent, deallocator);
	for (const auto &view : app.swapchain.image_views)
		generator.add(view);

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

	// Allocate triangle vertex buffer
	littlevk::Buffer vertex_buffer = littlevk::bind(app.device, memory_properties, deallocator)
		.buffer(triangles, sizeof(triangles), vk::BufferUsageFlagBits::eVertexBuffer);

	// Create a graphics pipeline
	auto vertex_layout = littlevk::VertexLayout <littlevk::rg32f, littlevk::rgb32f> ();

	auto bundle = littlevk::ShaderStageBundle(app.device, deallocator)
		.source(vertex_shader_source, vk::ShaderStageFlagBits::eVertex)
		.source(fragment_shader_source, vk::ShaderStageFlagBits::eFragment);

	littlevk::Pipeline ppl = littlevk::PipelineAssembler <littlevk::eGraphics> (app.device, app.window, deallocator)
		.with_render_pass(render_pass, 0)
		.with_vertex_layout(vertex_layout)
		.with_shader_bundle(bundle);

	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(deallocator);

	auto resize = [&]() {
		app.resize();

		// We can use the same generator; unpack() clears previously made framebuffers
		generator.extent = app.window.extent;
		for (const auto &view : app.swapchain.image_views)
			generator.add(view);

		framebuffers = generator.unpack();
	};

	// TODO: text render framerate
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

		// Start the render pass
		const auto &cmd = command_buffers[frame];
		cmd.begin(vk::CommandBufferBeginInfo {});

		// Set viewport and scissor
		littlevk::viewport_and_scissor(cmd, littlevk::RenderArea(app.window));

		const auto &rpbi = littlevk::default_rp_begin_info <1>
			(render_pass, framebuffers[op.index], app.window);

		cmd.beginRenderPass(rpbi, vk::SubpassContents::eInline);

		// Render the triangle
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ppl.handle);
		cmd.bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		cmd.draw(3, 1, 0, 0);

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
