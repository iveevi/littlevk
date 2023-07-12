#include "littlevk.hpp"

// GLM for vector math
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Shader sources
const std::string vertex_shader_source = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 color;

layout (push_constant) uniform PushConstants {
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
	vk::PhysicalDeviceMemoryProperties mem_props = phdev.getMemoryProperties();

	// Create an application skeleton with the bare minimum
	littlevk::Skeleton app;
	app.skeletonize(phdev, { 800, 600 }, "Spinning Cube");

	// Create a deallocator for automatic resource cleanup
	auto deallocator = new littlevk::Deallocator { app.device };
	
	// Create a render pass
	std::array <vk::AttachmentDescription, 2> attachments {
		vk::AttachmentDescription {
			{},
			app.swapchain.format,
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
		app.device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	).unwrap(deallocator);

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
		depth_info, mem_props
	).unwrap(deallocator);

	// Create framebuffers from the swapchain
	littlevk::FramebufferSetInfo fb_info;
	fb_info.swapchain = &app.swapchain;
	fb_info.render_pass = render_pass;
	fb_info.extent = app.window->extent;
	fb_info.depth_buffer = &depth_buffer.view;

	auto framebuffers = littlevk::framebuffers(app.device, fb_info).unwrap(deallocator);

	// Allocate command buffers
	vk::CommandPool command_pool = littlevk::command_pool(app.device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	).unwrap(deallocator);

	auto command_buffers = app.device.allocateCommandBuffers({
		command_pool, vk::CommandBufferLevel::ePrimary, 2
	});

	// Prepare cube vertex data
	struct Vertex {
		glm::vec3 postion;
		glm::vec3 color;

		static constexpr vk::VertexInputBindingDescription binding() {
			return vk::VertexInputBindingDescription {
				0, sizeof(Vertex), vk::VertexInputRate::eVertex
			};
		}

		static constexpr std::array <vk::VertexInputAttributeDescription, 2> attributes() {
			return std::array <vk::VertexInputAttributeDescription, 2> {
				vk::VertexInputAttributeDescription {
					0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, postion)
				},
				vk::VertexInputAttributeDescription {
					1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color)
				}
			};
		}
	};

	// Unit cube data
	// std::array <Vertex, 24> cube_vertex_data {
	std::vector <Vertex> cube_vertex_data {
		// Front
		Vertex { { -1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } },
		Vertex { {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } },
		Vertex { {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } },
		Vertex { { -1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } },

		// Back
		Vertex { { -1.0f, -1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f } },
		Vertex { {  1.0f, -1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f } },
		Vertex { {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f } },
		Vertex { { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f } },

		// Left
		Vertex { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, 1.0f } },
		Vertex { { -1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f } },
		Vertex { { -1.0f,  1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f } },
		Vertex { { -1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f, 1.0f } },

		// Right
		Vertex { {  1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 0.0f } },
		Vertex { {  1.0f, -1.0f,  1.0f }, { 1.0f, 1.0f, 0.0f } },
		Vertex { {  1.0f,  1.0f,  1.0f }, { 1.0f, 1.0f, 0.0f } },
		Vertex { {  1.0f,  1.0f, -1.0f }, { 1.0f, 1.0f, 0.0f } },

		// Top
		Vertex { { -1.0f, -1.0f, -1.0f }, { 0.0f, 1.0f, 1.0f } },
		Vertex { { -1.0f, -1.0f,  1.0f }, { 0.0f, 1.0f, 1.0f } },
		Vertex { {  1.0f, -1.0f,  1.0f }, { 0.0f, 1.0f, 1.0f } },
		Vertex { {  1.0f, -1.0f, -1.0f }, { 0.0f, 1.0f, 1.0f } },

		// Bottom
		Vertex { { -1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 1.0f } },
		Vertex { { -1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 1.0f } },
		Vertex { {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 1.0f } },
		Vertex { {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 1.0f } }
	};

	std::vector <uint32_t> cube_index_data {
		0, 1, 2,	2, 3, 0,	// Front
		4, 6, 5,	6, 4, 7,	// Back
		8, 10, 9,	10, 8, 11,	// Left
		12, 13, 14,	14, 15, 12,	// Right
		16, 17, 18,	18, 19, 16,	// Top
		20, 22, 21,	22, 20, 23	// Bottom
	};

	littlevk::Buffer vertex_buffer = littlevk::buffer(
		app.device,
		cube_vertex_data.size() * sizeof(Vertex),
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).unwrap(deallocator);

	littlevk::Buffer index_buffer = littlevk::buffer(
		app.device,
		cube_index_data.size() * sizeof(uint32_t),
		vk::BufferUsageFlagBits::eIndexBuffer,
		mem_props
	).unwrap(deallocator);

	littlevk::upload(app.device, vertex_buffer, cube_vertex_data);
	littlevk::upload(app.device, index_buffer, cube_index_data);
	
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
	struct PushConstants {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 proj;
	};

	vk::PushConstantRange push_constant_range {
		vk::ShaderStageFlagBits::eVertex,
		0, sizeof(PushConstants)
	};

	vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
		app.device,
		vk::PipelineLayoutCreateInfo {
			{}, nullptr,
			push_constant_range
		}
	).unwrap(deallocator);
	
	littlevk::pipeline::GraphicsCreateInfo pipeline_info;
	pipeline_info.vertex_binding = Vertex::binding();
	pipeline_info.vertex_attributes = Vertex::attributes();
	pipeline_info.vertex_shader = vertex_module;
	pipeline_info.fragment_shader = fragment_module;
	pipeline_info.extent = app.window->extent;
	pipeline_info.pipeline_layout = pipeline_layout;
	pipeline_info.render_pass = render_pass;

	vk::Pipeline pipeline = littlevk::pipeline::compile(app.device, pipeline_info).unwrap(deallocator);
	
	// Syncronization primitives
	auto sync = littlevk::present_syncronization(app.device, 2).unwrap(deallocator);

	// Prepare camera and model matrices
	glm::mat4 model = glm::mat4 { 1.0f };
	
	glm::mat4 view = glm::lookAt(
		glm::vec3 { 0.0f, 0.0f, 5.0f },
		glm::vec3 { 0.0f, 0.0f, 0.0f },
		glm::vec3 { 0.0f, 1.0f, 0.0f }
	);
	
	glm::mat4 proj = glm::perspective(
		glm::radians(45.0f),
		app.window->extent.width / (float) app.window->extent.height,
		0.1f, 10.0f
	);

	// Render loop
        uint32_t frame = 0;
        while (true) {
                glfwPollEvents();
                if (glfwWindowShouldClose(app.window->handle))
                        break;

		littlevk::SurfaceOperation op;
                op = littlevk::acquire_image(app.device, app.swapchain.swapchain, sync, frame);

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
		PushConstants push_constants {
			model, view, proj
		};

		// Rotate the model matrix
		push_constants.model = glm::rotate(
			push_constants.model,
			(float) glfwGetTime() * glm::radians(90.0f),
			glm::vec3 { 0.0f, 1.0f, 0.0f }
		);

		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		cmd.pushConstants <PushConstants> (pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, push_constants);
		cmd.bindVertexBuffers(0, vertex_buffer.buffer, { 0 });
		cmd.bindIndexBuffer(index_buffer.buffer, 0, vk::IndexType::eUint32);
		cmd.drawIndexed(cube_index_data.size(), 1, 0, 0, 0);

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

                op = littlevk::present_image(app.present_queue, app.swapchain.swapchain, sync, op.index);

		frame = 1 - frame;
		// TODO: resize function
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
