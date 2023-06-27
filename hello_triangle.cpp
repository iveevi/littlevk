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
	auto predicate = [](const vk::raii::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev,  {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
			VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
		});
	};

        // TODO: kobra::pick_first predicate...
	vk::raii::PhysicalDevice phdev = littlevk::pick_physical_device(predicate);

	littlevk::ApplicationSkeleton *app = new littlevk::ApplicationSkeleton;
        make_application(app, phdev, { 800, 600 }, "Hello Triangle");

	littlevk::PresentSyncronization sync(app->device, 2);

	// Create a dummy render pass
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

	vk::raii::RenderPass render_pass {
		app->device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	};

	// Create a dummy framebuffer
	std::vector <vk::raii::Framebuffer> framebuffers;

	for (vk::raii::ImageView &view : app->swapchain.image_views) {
		vk::ImageView views[] = { *view };

		vk::FramebufferCreateInfo fb_info {
			{}, *render_pass,
			1, views,
			app->window->extent.width, app->window->extent.height, 1
		};

		framebuffers.emplace_back(app->device, fb_info);
	}

	// Allocate command buffers
	vk::raii::CommandPool command_pool {
		app->device, {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	};

	vk::CommandBufferAllocateInfo alloc_info { *command_pool, vk::CommandBufferLevel::ePrimary, 2 };
	std::vector <vk::raii::CommandBuffer> command_buffers = app->device.allocateCommandBuffers(alloc_info);

	// Allocate triangle vertex buffer
	struct Vertex {
		float position[2];
		float color[3];

		// Bindings and attributes
		static constexpr vk::VertexInputBindingDescription bindings() {
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

	littlevk::Buffer vertex_buffer = littlevk::make_buffer(*app->device, sizeof(triangle), mem_props);
	upload(*app->device, vertex_buffer, triangle);

	// Compile shader modules
	littlevk::ShaderProgram vertex_shader { vertex_shader_source, vk::ShaderStageFlagBits::eVertex };
	littlevk::ShaderProgram fragment_shader { fragment_shader_source, vk::ShaderStageFlagBits::eFragment };

	vk::ShaderModule vertex_module = *vertex_shader.compile(*app->device);
	vk::ShaderModule fragment_module = *fragment_shader.compile(*app->device);

	// Create a graphics pipeline
	vk::PipelineShaderStageCreateInfo shader_stages[] = {
		vk::PipelineShaderStageCreateInfo {
			{}, vk::ShaderStageFlagBits::eVertex, vertex_module, "main"
		},
		vk::PipelineShaderStageCreateInfo {
			{}, vk::ShaderStageFlagBits::eFragment, fragment_module, "main"
		}
	};

	auto vertex_binding = Vertex::bindings();
	auto vertex_attributes = Vertex::attributes();

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {
		{}, vertex_binding, vertex_attributes
	};

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {
		{}, vk::PrimitiveTopology::eTriangleList
	};

	// TODO: dynamic state options
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) app->window->extent.width, (float) app->window->extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor {
		{}, app->window->extent
	};

	vk::PipelineViewportStateCreateInfo viewport_state {
		{}, 1, &viewport, 1, &scissor
	};

	vk::PipelineRasterizationStateCreateInfo rasterizer {
		{}, false, false,
		vk::PolygonMode::eFill,
		vk::CullModeFlagBits::eBack,
		vk::FrontFace::eClockwise,
		false, 0.0f, 0.0f, 0.0f, 1.0f
	};

	vk::PipelineMultisampleStateCreateInfo multisampling {
		{}, vk::SampleCountFlagBits::e1
	};

	vk::PipelineColorBlendAttachmentState color_blend_attachment {
		false,
		vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
	};

	vk::PipelineColorBlendStateCreateInfo color_blending {
		{}, false, vk::LogicOp::eCopy,
		1, &color_blend_attachment,
		{ 0.0f, 0.0f, 0.0f, 0.0f }
	};

	vk::PipelineLayoutCreateInfo pipeline_layout_info {
		{}, 0, nullptr, 0, nullptr
	};

	vk::raii::PipelineLayout pipeline_layout { app->device, pipeline_layout_info };

	vk::raii::Pipeline pipeline {
		app->device,
		nullptr,
		vk::GraphicsPipelineCreateInfo {
			{}, shader_stages,
			&vertex_input_info, &input_assembly,
			nullptr, &viewport_state, &rasterizer,
			&multisampling, nullptr, &color_blending,
			nullptr, *pipeline_layout, *render_pass,
			0, nullptr, -1
		}
	};

	// TODO: simple hello triangle...
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();
                if (glfwWindowShouldClose(app->window->handle))
                        break;

		littlevk::SurfaceOperation op;
                op = littlevk::acquire_image(app->device, app->swapchain.swapchain, sync, frame);

		// Start empty render pass
		vk::ClearValue clear_value = vk::ClearColorValue { std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f } };

		vk::RenderPassBeginInfo render_pass_info {
			*render_pass, *framebuffers[op.index],
			vk::Rect2D { {}, app->window->extent },
			1, &clear_value
		};

		command_buffers[frame].begin(vk::CommandBufferBeginInfo {});
		command_buffers[frame].beginRenderPass(render_pass_info, vk::SubpassContents::eInline);

		// Render the triangle
		command_buffers[frame].bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
		command_buffers[frame].bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		command_buffers[frame].draw(3, 1, 0, 0);

		command_buffers[frame].endRenderPass();
		command_buffers[frame].end();

		// Submit command buffer while signaling the semaphore
		vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submit_info {
			1, &*sync.image_available[frame],
			&wait_stage,
			1, &*command_buffers[frame],
			1, &*sync.render_finished[frame]
		};

		app->graphics_queue.submit(submit_info, *sync.in_flight[frame]);

                op = littlevk::present_image(app->present_queue, app->swapchain.swapchain, sync, op.index);
		frame = 1 - frame;

		// TODO: resize function
        }

        // Delete applica
	littlevk::destroy_application(app);
        delete app;

        return 0;
}
