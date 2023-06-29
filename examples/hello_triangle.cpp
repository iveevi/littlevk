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

int main()
{
	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev,  {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		});
	};

	vk::PhysicalDevice phdev = littlevk::pick_physical_device(predicate);

	// Create an application skeleton with the bare minimum
	littlevk::ApplicationSkeleton *app = new littlevk::ApplicationSkeleton;
        make_application(app, phdev, { 800, 600 }, "Hello Triangle");

	// Create a deallocator for automatic resource cleanup
	auto deallocator = new littlevk::Deallocator { app->device };

	// Create a render pass
	std::array <vk::AttachmentDescription, 1> attachments {
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
		}
	};

	std::array <vk::AttachmentReference, 1> color_attachments {
		vk::AttachmentReference {
			0,
			vk::ImageLayout::eColorAttachmentOptimal,
		}
	};

	vk::SubpassDescription subpass {
		{}, vk::PipelineBindPoint::eGraphics,
		{}, color_attachments,
		{}, nullptr
	};

	vk::RenderPass render_pass = littlevk::render_pass(
		app->device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	).unwrap(deallocator);

	// Create framebuffers from the swapchain
	littlevk::FramebufferSetInfo fb_info;
	fb_info.swapchain = &app->swapchain;
	fb_info.render_pass = render_pass;
	fb_info.extent = app->window->extent;

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

	// Allocate triangle vertex buffer
	struct Vertex {
		float position[2];
		float color[3];

		// Bindings and attributes
		static constexpr vk::VertexInputBindingDescription binding() {
			return {
				0, sizeof(Vertex), vk::VertexInputRate::eVertex
			};
		}

		static constexpr std::array <vk::VertexInputAttributeDescription, 2> attributes() {
			return {
				vk::VertexInputAttributeDescription {
					0, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, position)
				},
				vk::VertexInputAttributeDescription {
					1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color)
				}
			};
		}
	};

	constexpr Vertex triangle[] {
		{ {  0.0f, -0.5f }, { 1.0f, 0.0f, 0.0f } },
		{ {  0.5f,  0.5f }, { 0.0f, 1.0f, 0.0f } },
		{ { -0.5f,  0.5f }, { 0.0f, 0.0f, 1.0f } },
	};

	vk::PhysicalDeviceMemoryProperties mem_props = phdev.getMemoryProperties();

	littlevk::Buffer vertex_buffer = littlevk::buffer(
		app->device,
		sizeof(triangle),
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).unwrap(deallocator);

	littlevk::upload(app->device, vertex_buffer, triangle);

	// Compile shader modules
	vk::ShaderModule vertex_module = littlevk::shader::compile(
		app->device, vertex_shader_source,
		vk::ShaderStageFlagBits::eVertex
	).unwrap(deallocator);

	vk::ShaderModule fragment_module = littlevk::shader::compile(
		app->device, fragment_shader_source,
		vk::ShaderStageFlagBits::eFragment
	).unwrap(deallocator);

	// Create a graphics pipeline
	vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
		app->device,
		vk::PipelineLayoutCreateInfo {
			{}, 0, nullptr, 0, nullptr
		}
	).unwrap(deallocator);

	littlevk::pipeline::GraphicsCreateInfo pipeline_info;
	pipeline_info.vertex_binding = Vertex::binding();
	pipeline_info.vertex_attributes = Vertex::attributes();
	pipeline_info.vertex_shader = vertex_module;
	pipeline_info.fragment_shader = fragment_module;
	pipeline_info.extent = app->window->extent;
	pipeline_info.pipeline_layout = pipeline_layout;
	pipeline_info.render_pass = render_pass;

	vk::Pipeline pipeline = littlevk::pipeline::compile(app->device, pipeline_info).unwrap(deallocator);

	// Syncronization primitives
	auto sync = littlevk::make_present_syncronization(app->device, 2).unwrap(deallocator);

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();
                if (glfwWindowShouldClose(app->window->handle))
                        break;

		littlevk::SurfaceOperation op;
                op = littlevk::acquire_image(app->device, app->swapchain.swapchain, sync, frame);

		// Start empty render pass
		vk::ClearValue clear_value = vk::ClearColorValue {
			std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f }
		};

		vk::RenderPassBeginInfo render_pass_info {
			render_pass, framebuffers[op.index],
			vk::Rect2D { {}, app->window->extent },
			1, &clear_value
		};

		command_buffers[frame].begin(vk::CommandBufferBeginInfo {});
		command_buffers[frame].beginRenderPass(render_pass_info, vk::SubpassContents::eInline);

		// Render the triangle
		command_buffers[frame].bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		command_buffers[frame].bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		command_buffers[frame].draw(3, 1, 0, 0);

		command_buffers[frame].endRenderPass();
		command_buffers[frame].end();

		// Submit command buffer while signaling the semaphore
		vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submit_info {
			1, &sync.image_available[frame],
			&wait_stage,
			1, &command_buffers[frame],
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
