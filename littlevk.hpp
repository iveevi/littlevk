#pragma once

// Standard library
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>

// Miscellaneous standard library
#include <stdarg.h>

// Vulkan and GLFW
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>
#include <GLFW/glfw3.h>

// Glslang and SPIRV-Tools
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace littlevk {

namespace detail {

// Configuration parameters (free to user modification)
struct Config {
	std::vector<const char *> instance_extensions {};
	bool enable_validation_layers = true;
	bool abort_on_validation_error = true;
	bool enable_logging = true;
};

} // namespace detail

// Singleton config
inline detail::Config &config()
{
	static detail::Config config;
	return config;
}

} // namespace littlevk

// Logging
namespace microlog {

// Colors and formatting
// TODO: windows support
struct colors {
	static constexpr const char *error = "\033[31;1m";
	static constexpr const char *info = "\033[34;1m";
	static constexpr const char *reset = "\033[0m";
	static constexpr const char *warning = "\033[33;1m";
};

inline void error(const char *header, const char *format, ...)
{
	printf("%s[littlevk::error]%s (%s) ", colors::error, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void warning(const char *header, const char *format, ...)
{
	printf("%s[littlevk::warning]%s (%s) ", colors::warning, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void info(const char *header, const char *format, ...)
{
	if (!littlevk::config().enable_logging)
		return;

	printf("%s[littlevk::info]%s (%s) ", colors::info, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void assertion(bool cond, const char *header, const char *format, ...)
{
	if (cond)
		return;

	printf("%s[littlevk::assert]%s (%s) ", colors::error, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

} // namespace microlog

// Standalone utils, imported from other sources
namespace standalone {

inline const std::string readfile(const std::filesystem::path &path)
{
	std::ifstream f(path);
	if (!f.good()) {
		microlog::error(__FUNCTION__, "Could not open file: %s\n", path.c_str());
		return "";
	}

	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

struct DirectoryIncluder : public glslang::TShader::Includer {
	std::vector<std::string> directories;

	DirectoryIncluder() = default;

	IncludeResult *includeLocal(const char *header,
	                            const char *includer, size_t depth) override
	{
		return read_local_path(header, includer, (int) depth);
	}

	IncludeResult *includeSystem(const char *, const char *, size_t) override
	{
		return nullptr;
	}

	void releaseInclude(IncludeResult *result) override
	{
		if (result != nullptr) {
			delete[] static_cast<char *>(result->userData);
			delete result;
		}
	}

	void include(const std::string &dir) { directories.push_back(dir); }

	IncludeResult *read_local_path(const char *header, const char *includer,
				       int depth)
	{
		for (auto it = directories.rbegin(); it != directories.rend();
		     it++) {
			std::string path = *it + '/' + header;
			std::replace(path.begin(), path.end(), '\\', '/');
			std::ifstream file(path, std::ios_base::binary |
							 std::ios_base::ate);
			if (file) {
				int length = file.tellg();
				char *content = new char[length];
				file.seekg(0, file.beg);
				file.read(content, length);
				return new IncludeResult(path, content, length,
							 content);
			}
		}

		return nullptr;
	}
};

} // namespace standalone

namespace littlevk {

// Automatic deallocation system
using DeallocationQueue = std::queue<std::function<void(vk::Device)>>;

struct Deallocator {
	vk::Device device;

	// TODO: map addresses (void *) to functions with timers (-1 for indefinite)
	DeallocationQueue device_deallocators;

	Deallocator(vk::Device device = nullptr) : device(device) {}

	void drop() {
		while (!device_deallocators.empty()) {
			device_deallocators.front()(device);
			device_deallocators.pop();
		}
	}
};

// Return proxy for device objects
template <typename T, void destructor(const vk::Device &, const T &)>
struct DeviceReturnProxy {
	T value;
	bool failed;

	DeviceReturnProxy(T value) : value(value), failed(false) {}
	DeviceReturnProxy(bool failed) : failed(failed) {}

	T unwrap(Deallocator &deallocator) {
		if (failed)
			return {};

		T val = value;
		deallocator.device_deallocators.push(
			[val](vk::Device device) {
				destructor(device, val);
			});

		return value;
	}

	T defer(DeallocationQueue &queue) {
		if (failed)
			return {};

		T val = value;
		queue.push([val](vk::Device device) {
			destructor(device, val);
		});

		return value;
	}
};

// Return proxy helper for structures composed of multiple device objects
template <typename T>
struct ComposedReturnProxy {
	T value;
	bool failed;
	DeallocationQueue queue;

	ComposedReturnProxy(T value, DeallocationQueue queue)
	    : value(value), failed(false), queue(queue)
	{
	}
	ComposedReturnProxy(bool failed) : failed(failed) {}

	T unwrap(Deallocator &deallocator) {
		if (failed)
			return {};

		while (!queue.empty()) {
			deallocator.device_deallocators.push(queue.front());
			queue.pop();
		}

		return this->value;
	}

	T defer(DeallocationQueue &q) {
		if (failed)
			return {};

		while (!queue.empty()) {
			q.push(queue.front());
			queue.pop();
		}

		return value;
	}
};

namespace validation {

// Create debug messenger
static bool check_validation_layer_support(
	const std::vector<const char *> &validation_layers)
{
	uint32_t layer_count;
	vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

	std::vector<VkLayerProperties> available_layers(layer_count);

	vkEnumerateInstanceLayerProperties(&layer_count,
					   available_layers.data());
	for (const char *layer : validation_layers) {
		bool layerFound = false;

		for (const auto &properties : available_layers) {
			if (strcmp(layer, properties.layerName) == 0) {
				layerFound = true;
				break;
			}
		}

		if (!layerFound)
			return false;
	}

	return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_logger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	     VkDebugUtilsMessageTypeFlagsEXT messageType,
	     const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
	     void *pUserData)
{
	// Errors
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		microlog::error("validation", "%s\n", pCallbackData->pMessage);
		if (config().abort_on_validation_error)
			__builtin_trap();
	}
	else if (messageSeverity >=
		 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		microlog::warning("validation", "%s\n",
				  pCallbackData->pMessage);
	}
	else if (config().enable_logging) {
		microlog::info("validation", "%s\n", pCallbackData->pMessage);
	}

	return VK_FALSE;
}

} // namespace validation

namespace detail {

// Initialize GLFW statically
inline void initialize_glfw()
{
	static bool initialized = false;

	if (!initialized) {
		glfwInit();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		initialized = true;
	}
}

// Get (or generate) the required extensions
inline const std::vector <const char *> &get_required_extensions()
{
	// Vector to return
	static std::vector<const char *> extensions;

	// Add if empty
	if (extensions.empty()) {
		// Add glfw extensions
		uint32_t glfw_extension_count;
		const char **glfw_extensions = glfwGetRequiredInstanceExtensions( &glfw_extension_count);
		extensions.insert(extensions.end(), glfw_extensions,
				  glfw_extensions + glfw_extension_count);

		// Additional extensions
		extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		// TODO: add config extensions

		if (config().enable_validation_layers) {
			// Add validation layers
			extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
	}

	return extensions;
}

// Instance singleton
static struct {
	bool initialized = false;
	vk::Instance instance;
} global_instance;

// Debug messenger singleton
static struct debug_messenger_singleton {
	bool initialized = false;
	vk::DebugUtilsMessengerEXT messenger;

	~debug_messenger_singleton()
	{
		if (initialized) {
			if (!global_instance.initialized) {
				microlog::error(
					"fatal",
					"debug messenger singleton destroyed "
					"without valid instance singleton\n");
				return;
			}

			global_instance.instance.destroyDebugUtilsMessengerEXT(
				messenger);
		}
	}
} global_messenger;

// Get (or create) the singleton instance
inline const vk::Instance &get_vulkan_instance()
{
	// TODO: from config...
	static vk::ApplicationInfo app_info {
		"LittleVk", VK_MAKE_VERSION(1, 0, 0), "LittelVk",
		VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_3};

	// Skip if already initialized
	if (global_instance.initialized)
		return global_instance.instance;

	// Make sure GLFW is initialized
	initialize_glfw();

	static const std::vector<const char *> validation_layers = {
		"VK_LAYER_KHRONOS_validation"};

	static vk::InstanceCreateInfo instance_info {
		vk::InstanceCreateFlags(),
		&app_info,
		0,
		nullptr,
		(uint32_t) get_required_extensions().size(),
		get_required_extensions().data()};

	if (config().enable_validation_layers) {
		// Check if validation layers are available
		if (!validation::check_validation_layer_support(
			    validation_layers)) {
			microlog::error(
				"instance initialization",
				"Validation layers are not available!\n");
			config().enable_validation_layers = false;
		}

		if (config().enable_validation_layers) {
			instance_info.enabledLayerCount =
				(uint32_t) validation_layers.size();
			instance_info.ppEnabledLayerNames =
				validation_layers.data();
		}
	}

	global_instance.instance = vk::createInstance(instance_info);

	// Loading the debug messenger
	if (config().enable_validation_layers) {
		// Create debug messenger
		static constexpr vk::DebugUtilsMessengerCreateInfoEXT
			debug_messenger_info {
				vk::DebugUtilsMessengerCreateFlagsEXT(),
				vk::DebugUtilsMessageSeverityFlagBitsEXT::
						eError |
					vk::DebugUtilsMessageSeverityFlagBitsEXT::
						eWarning |
					vk::DebugUtilsMessageSeverityFlagBitsEXT::
						eVerbose |
					vk::DebugUtilsMessageSeverityFlagBitsEXT::
						eInfo,
				vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
					vk::DebugUtilsMessageTypeFlagBitsEXT::
						ePerformance |
					vk::DebugUtilsMessageTypeFlagBitsEXT::
						eValidation,
				validation::debug_logger};

		global_messenger.messenger = global_instance.instance
			.createDebugUtilsMessengerEXT(debug_messenger_info);
		global_messenger.initialized = true;
	}

	global_instance.initialized = true;

	return global_instance.instance;
}

// Explicit shutdown routine, worst case for users...
inline void shutdown_now()
{
	// Kill the messenger, then the instance
	if (global_messenger.initialized) {
		global_instance.instance.destroyDebugUtilsMessengerEXT(
			global_messenger.messenger);
		global_messenger.initialized = false;
	}

	if (global_instance.initialized) {
		global_instance.instance.destroy();
		global_instance.initialized = false;
	}
}

} // namespace detail

// Window typebackend
struct Window {
	GLFWwindow *handle = nullptr;
	std::string title;
	vk::Extent2D extent;

	void drop() {
		if (handle)
			glfwDestroyWindow(handle);
		handle = nullptr;
	}
};

// Creating windows and surfaces
inline Window make_window(const vk::Extent2D &extent, const std::string &title)
{
	// Make sure GLFW is initialized
	detail::initialize_glfw();

	// Create the window
	GLFWwindow *handle = glfwCreateWindow(extent.width, extent.height,
					      title.c_str(), nullptr, nullptr);

	// Get the actual size of the window
	glfwGetFramebufferSize(handle, (int *) &extent.width, (int *) &extent.height);

	microlog::assertion(handle != nullptr, __FUNCTION__, "Failed to create window\n");
	microlog::info(__FUNCTION__, "New GLFW window (@%p) created: \'%s\', %dx%d\n",
			handle, title.c_str(), extent.width, extent.height);

	return Window { handle, title, extent };
}

inline vk::SurfaceKHR make_surface(const Window &window)
{
	// Create the surface
	VkSurfaceKHR surface;
	VkResult result = glfwCreateWindowSurface(detail::get_vulkan_instance(),
			window.handle, nullptr, &surface);

	microlog::assertion(result == VK_SUCCESS, __FUNCTION__, "Failed to create a surface\n");
	microlog::info(__FUNCTION__, "New Vulkan surface (@%p) created\n", surface);

	return static_cast <vk::SurfaceKHR> (surface);
}

// Coupling graphics and present queue families
struct QueueFamilyIndices {
	uint32_t graphics;
	uint32_t present;
};

// Find graphics queue family
inline uint32_t find_graphics_queue_family(const vk::PhysicalDevice &phdev)
{
	// Get the queue families
	std::vector<vk::QueueFamilyProperties> queue_families =
		phdev.getQueueFamilyProperties();

	// Find the first one that supports graphics
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics)
			return i;
	}

	// If none found, throw an error
	throw std::runtime_error("[Vulkan] No graphics queue family found");
}

// Find present queue family
inline uint32_t find_present_queue_family(const vk::PhysicalDevice &phdev,
					  const vk::SurfaceKHR &surface)
{
	// Get the queue families
	std::vector<vk::QueueFamilyProperties> queue_families =
		phdev.getQueueFamilyProperties();

	// Find the first one that supports presentation
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (phdev.getSurfaceSupportKHR(i, surface))
			return i;
	}

	microlog::assertion(false, __FUNCTION__, "No presentation queue family found\n");

	return -1;
}

// Get both graphics and present queue families
inline QueueFamilyIndices find_queue_families(const vk::PhysicalDevice &phdev,
					      const vk::SurfaceKHR &surface)
{
	return {find_graphics_queue_family(phdev),
		find_present_queue_family(phdev, surface)};
}

// Swapchain structure
struct Swapchain {
	vk::Format format;
	vk::SwapchainKHR handle;
	std::vector<vk::Image> images;
	std::vector<vk::ImageView> image_views;
	vk::SwapchainCreateInfoKHR info;

	vk::SwapchainKHR operator*() const { return handle; }
};

// Pick a surface format
inline vk::SurfaceFormatKHR pick_surface_format(const vk::PhysicalDevice &phdev,
						const vk::SurfaceKHR &surface)
{
	// Constant formats
	// TODO: add more flexibility
	static const std::vector <vk::SurfaceFormatKHR> target_formats = {
		{ vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear },
	};

	// Get the surface formats
	std::vector <vk::SurfaceFormatKHR> formats = phdev.getSurfaceFormatsKHR(surface);

	// If there is only one format, return it
	if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined) {
		return {
			vk::Format::eB8G8R8A8Unorm,
			vk::ColorSpaceKHR::eSrgbNonlinear
		};
	}

	// Find the first one that is supported
	for (const vk::SurfaceFormatKHR &format : formats) {
		if (std::find_if(target_formats.begin(), target_formats.end(),
				 [&format](const vk::SurfaceFormatKHR &target) {
					 return format.format == target.format
					 	&& format.colorSpace == target.colorSpace;
				 }) != target_formats.end()) {
			return format;
		}
	}

	microlog::assertion(false, __FUNCTION__, "No supported surface format found\n");

	return vk::SurfaceFormatKHR();
}

// Pick a present mode
inline vk::PresentModeKHR pick_present_mode(const vk::PhysicalDevice &phdev,
					    const vk::SurfaceKHR &surface)
{
	// Constant modes
	static const std::vector<vk::PresentModeKHR> target_modes = {
		vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eImmediate,
		vk::PresentModeKHR::eFifo};

	// Get the present modes
	std::vector<vk::PresentModeKHR> modes =
		phdev.getSurfacePresentModesKHR(surface);

	// Prioritize mailbox mode
	if (std::find(modes.begin(), modes.end(),
		      vk::PresentModeKHR::eMailbox) != modes.end()) {
		return vk::PresentModeKHR::eMailbox;
	}

	// Find the first one that is supported
	for (const vk::PresentModeKHR &mode : modes) {
		if (std::find(target_modes.begin(), target_modes.end(), mode) !=
		    target_modes.end()) {
			return mode;
		}
	}

	// If none found, throw an error
	microlog::assertion(false, "vulkan", "No supported present mode found");

	return vk::PresentModeKHR::eFifo;
}

// Swapchain allocation and destruction
// TODO: info struct...
inline Swapchain swapchain(const vk::PhysicalDevice &phdev,
		           const vk::Device &device,
			   const vk::SurfaceKHR &surface,
			   const vk::Extent2D &extent,
			   const QueueFamilyIndices &indices,
			   const std::optional <vk::PresentModeKHR> &priority_mode = std::nullopt,
			   const std::optional <vk::SwapchainKHR> old_swapchain = std::nullopt)
{
	Swapchain swapchain;

	// Pick a surface format
	auto surface_format = pick_surface_format(phdev, surface);
	swapchain.format = surface_format.format;

	microlog::info(__FUNCTION__, "Picked format %s for swapchain\n", vk::to_string(swapchain.format).c_str());

	// Surface capabilities and extent
	vk::SurfaceCapabilitiesKHR capabilities = phdev.getSurfaceCapabilitiesKHR(surface);

	// Set the surface extent
	vk::Extent2D swapchain_extent = extent;
	if (capabilities.currentExtent.width == std::numeric_limits <uint32_t> ::max()) {
		swapchain_extent.width = std::clamp(swapchain_extent.width,
				   capabilities.minImageExtent.width,
				   capabilities.maxImageExtent.width);

		swapchain_extent.height = std::clamp(swapchain_extent.height,
				   capabilities.minImageExtent.height,
				   capabilities.maxImageExtent.height);
	} else {
		swapchain_extent = capabilities.currentExtent;
	}

	// Transform, etc
	vk::SurfaceTransformFlagBitsKHR transform = (capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity)
			? vk::SurfaceTransformFlagBitsKHR::eIdentity
			: capabilities.currentTransform;

	// Composite alpha
	vk::CompositeAlphaFlagBitsKHR composite_alpha = (capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
			? vk::CompositeAlphaFlagBitsKHR::eOpaque
			: vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;

	// Present mode
	vk::PresentModeKHR present_mode = priority_mode.value_or(pick_present_mode(phdev, surface));

	microlog::info(__FUNCTION__, "Picked present mode %s for swapchain\n", vk::to_string(present_mode).c_str());

	// Creation info
	swapchain.info = vk::SwapchainCreateInfoKHR {
		{},
		surface,
		capabilities.minImageCount,
		swapchain.format,
		surface_format.colorSpace,
		swapchain_extent,
		1,
		// TODO: pass these as options
		vk::ImageUsageFlagBits::eColorAttachment
			| vk::ImageUsageFlagBits::eTransferSrc
			| vk::ImageUsageFlagBits::eTransferDst,
		vk::SharingMode::eExclusive,
		{},
		transform,
		composite_alpha,
		present_mode,
		true,
		old_swapchain.value_or(nullptr)
	};

	// In case graphics and present queues are different
	if (indices.graphics != indices.present) {
		swapchain.info.imageSharingMode = vk::SharingMode::eConcurrent;
		swapchain.info.queueFamilyIndexCount = 2;
		swapchain.info.pQueueFamilyIndices = &indices.graphics;
	}

	// Create the swapchain
	swapchain.handle = device.createSwapchainKHR(swapchain.info);

	// Get the swapchain images
	swapchain.images = device.getSwapchainImagesKHR(swapchain.handle);

	// Create image views
	vk::ImageViewCreateInfo create_view_info {
		{}, {}, vk::ImageViewType::e2D,
		swapchain.format, {},
		vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
	};

	for (size_t i = 0; i < swapchain.images.size(); i++) {
		create_view_info.image = swapchain.images[i];
		swapchain.image_views.emplace_back(device.createImageView(create_view_info));
	}

	return swapchain;
}

inline void resize(const vk::Device &device, Swapchain &swapchain, const vk::Extent2D &extent)
{
	// First free the old swapchain resources
	for (const vk::ImageView &view : swapchain.image_views)
		device.destroyImageView(view);

	device.destroySwapchainKHR(swapchain.handle);

	// We simply need to modify the swapchain info and rebuild it
	swapchain.info.imageExtent = extent;
	swapchain.handle = device.createSwapchainKHR(swapchain.info);
	swapchain.images = device.getSwapchainImagesKHR(swapchain.handle);

	// Recreate image views
	vk::ImageViewCreateInfo create_view_info {
		{},
		{},
		vk::ImageViewType::e2D,
		swapchain.format,
		{},
		vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1,
					  0, 1)};

	swapchain.image_views.clear();
	for (size_t i = 0; i < swapchain.images.size(); i++) {
		create_view_info.image = swapchain.images[i];
		swapchain.image_views.emplace_back(device.createImageView(create_view_info));
	}
}

inline void destroy_swapchain(const vk::Device &device, Swapchain &swapchain)
{
	// Destroy image views
	for (const vk::ImageView &view : swapchain.image_views)
		device.destroyImageView(view);

	// Destroy swapchain
	device.destroySwapchainKHR(swapchain.handle);
}

// Return proxy for framebuffer(s)
inline void destroy_framebuffer(const vk::Device &device, const vk::Framebuffer &framebuffer)
{
	device.destroyFramebuffer(framebuffer);
}

using FramebufferReturnProxy = DeviceReturnProxy <vk::Framebuffer, destroy_framebuffer>;

// TODO: template render pass assembler to check with the attachments?
// Framebuffer generator wrapper
struct FramebufferGenerator {
	const vk::Device &device;
	const vk::RenderPass &render_pass;
	vk::Extent2D extent;
	Deallocator &dal;

	std::vector<vk::Framebuffer> framebuffers;

	FramebufferGenerator(const vk::Device &device_,
			     const vk::RenderPass &render_pass_,
			     const vk::Extent2D &extent_,
			     Deallocator &dal_)
		: device(device_),
		render_pass(render_pass_),
		extent(extent_),
		dal(dal_) {}

	template <typename... Args>
	requires(std::is_trivially_constructible_v <vk::ImageView, Args> && ...)
	void add(const Args &...args)
	{
		const std::array <vk::ImageView, sizeof...(Args)> views { args... };

		vk::FramebufferCreateInfo info {
			{}, render_pass, views,
			extent.width, extent.height, 1
		};

		FramebufferReturnProxy ret = device.createFramebuffer(info);
		framebuffers.push_back(ret.unwrap(dal));
	}

	std::vector <vk::Framebuffer> unpack()
	{
		auto ret = framebuffers;
		framebuffers.clear();
		return ret;
	}
};

// Vulkan description/create info wrappers
struct AttachmentDescription {
	vk::Format m_format;
	vk::SampleCountFlagBits m_samples;
	vk::AttachmentLoadOp m_load_op;
	vk::AttachmentStoreOp m_store_op;
	vk::AttachmentLoadOp m_stencil_load_op;
	vk::AttachmentStoreOp m_stencil_store_op;
	vk::ImageLayout m_initial_layout;
	vk::ImageLayout m_final_layout;

	constexpr operator vk::AttachmentDescription() const {
		return vk::AttachmentDescription(
			{}, m_format, m_samples, m_load_op, m_store_op,
			m_stencil_load_op, m_stencil_store_op, m_initial_layout,
			m_final_layout);
	}

	constexpr AttachmentDescription &format(vk::Format format) {
		this->m_format = format;
		return *this;
	}

	constexpr AttachmentDescription &samples(vk::SampleCountFlagBits samples) {
		this->m_samples = samples;
		return *this;
	}

	constexpr AttachmentDescription &load_op(vk::AttachmentLoadOp load_op) {
		this->m_load_op = load_op;
		return *this;
	}

	constexpr AttachmentDescription &store_op(vk::AttachmentStoreOp store_op) {
		this->m_store_op = store_op;
		return *this;
	}

	constexpr AttachmentDescription &stencil_load_op(vk::AttachmentLoadOp stencil_load_op) {
		this->m_stencil_load_op = stencil_load_op;
		return *this;
	}

	constexpr AttachmentDescription &stencil_store_op(vk::AttachmentStoreOp stencil_store_op) {
		this->m_stencil_store_op = stencil_store_op;
		return *this;
	}

	constexpr AttachmentDescription &initial_layout(vk::ImageLayout initial_layout) {
		this->m_initial_layout = initial_layout;
		return *this;
	}

	constexpr AttachmentDescription &final_layout(vk::ImageLayout final_layout) {
		this->m_final_layout = final_layout;
		return *this;
	}
};

// Preset attachment descriptions
constexpr AttachmentDescription default_color_attachment(const vk::Format &swapchain_format)
{
	return AttachmentDescription()
		.format(swapchain_format)
		.samples(vk::SampleCountFlagBits::e1)
		.load_op(vk::AttachmentLoadOp::eClear)
		.store_op(vk::AttachmentStoreOp::eStore)
		.stencil_load_op(vk::AttachmentLoadOp::eDontCare)
		.stencil_store_op(vk::AttachmentStoreOp::eDontCare)
		.initial_layout(vk::ImageLayout::eUndefined)
		.final_layout(vk::ImageLayout::ePresentSrcKHR);
}

constexpr AttachmentDescription default_depth_attachment()
{
	return AttachmentDescription()
		.format(vk::Format::eD32Sfloat)
		.samples(vk::SampleCountFlagBits::e1)
		.load_op(vk::AttachmentLoadOp::eClear)
		.store_op(vk::AttachmentStoreOp::eDontCare)
		.stencil_load_op(vk::AttachmentLoadOp::eDontCare)
		.stencil_store_op(vk::AttachmentStoreOp::eDontCare)
		.initial_layout(vk::ImageLayout::eUndefined)
		.final_layout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

// Present render passes
static void destroy_render_pass(const vk::Device &device,
				const vk::RenderPass &render_pass)
{
	device.destroyRenderPass(render_pass);
}

using RenderPassReturnProxy = DeviceReturnProxy <vk::RenderPass, destroy_render_pass>;

inline RenderPassReturnProxy render_pass(const vk::Device &device,
					 const vk::RenderPassCreateInfo &info)
{
	vk::RenderPass render_pass;
	if (device.createRenderPass(&info, nullptr, &render_pass) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(render_pass);
}

inline RenderPassReturnProxy render_pass(const vk::Device &device,
					 const vk::ArrayProxyNoTemporaries <vk::AttachmentDescription> &descriptions,
					 const vk::ArrayProxyNoTemporaries <vk::SubpassDescription> &subpasses,
					 const vk::ArrayProxyNoTemporaries <vk::SubpassDependency> &dependencies)
{
	vk::RenderPassCreateInfo info {
		{}, descriptions,
		subpasses, dependencies
	};

	vk::RenderPass render_pass;
	if (device.createRenderPass(&info, nullptr, &render_pass) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(render_pass);
}

// Render pass assembly
struct RenderPassAssembler {
	const vk::Device &device;
	littlevk::Deallocator &dal;

	std::vector <vk::SubpassDescription> subpasses;
	std::vector <vk::AttachmentDescription> attachments;
	std::vector <vk::SubpassDependency> dependencies;

	RenderPassAssembler(const vk::Device &device_, littlevk::Deallocator &dal_)
		: device(device_), dal(dal_) {}

	RenderPassAssembler &add_attachment(const vk::AttachmentDescription &description) {
		attachments.push_back(description);
		return *this;
	}

	struct SubpassAssembler {
		RenderPassAssembler &parent;

		vk::PipelineBindPoint bindpoint;

		std::vector <vk::AttachmentReference> inputs;
		std::vector <vk::AttachmentReference> colors;
		std::optional <vk::AttachmentReference> depth = {};

		SubpassAssembler(RenderPassAssembler &parent_,
				 const vk::PipelineBindPoint &bindpoint_)
		    : parent(parent_), bindpoint(bindpoint_) {}

		SubpassAssembler &input_attachment(uint32_t attachment, vk::ImageLayout layout) {
			inputs.emplace_back(attachment, layout);
			return *this;
		}

		SubpassAssembler &color_attachment(uint32_t attachment, vk::ImageLayout layout) {
			colors.emplace_back(attachment, layout);
			return *this;
		}

		SubpassAssembler &depth_attachment(uint32_t attachment, vk::ImageLayout layout) {
			depth = vk::AttachmentReference(attachment, layout);
			return *this;
		}

		RenderPassAssembler &done() {
			// TODO: push this struct onto the rp assembler
			parent.subpasses.push_back(vk::SubpassDescription {
				{},
				bindpoint,
				inputs,
				colors,
				{},
				depth ? &(*depth) : nullptr,
				{}});

			return parent;
		}
	};

	SubpassAssembler add_subpass(const vk::PipelineBindPoint &bindpoint) {
		return SubpassAssembler(*this, bindpoint);
	}

	RenderPassAssembler &add_dependency(uint32_t src,
					    uint32_t dst,
					    const vk::PipelineStageFlags &src_mask,
					    const vk::PipelineStageFlags &dst_mask) {
		dependencies.emplace_back(src, dst, src_mask, dst_mask);
		return *this;
	}

	operator vk::RenderPass() {
		return littlevk::render_pass(device,
			attachments, subpasses, dependencies).unwrap(dal);
	}
};

// Vulkan render pass begin info wrapper
struct RenderPassBeginInfo {
	vk::RenderPass render_pass;
	vk::Framebuffer framebuffer;
	vk::Extent2D extent;
	std::vector <vk::ClearValue> clear_values;

	RenderPassBeginInfo(size_t N) : clear_values(N) {}

	operator vk::RenderPassBeginInfo() const {
		return vk::RenderPassBeginInfo {
			render_pass,
			framebuffer,
			vk::Rect2D { { 0, 0 }, extent },
			clear_values
		};
	}

	RenderPassBeginInfo &with_render_pass(vk::RenderPass render_pass_) {
		render_pass = render_pass_;
		return *this;
	}

	RenderPassBeginInfo &with_framebuffer(vk::Framebuffer framebuffer_) {
		framebuffer = framebuffer_;
		return *this;
	}

	RenderPassBeginInfo &with_extent(vk::Extent2D extent_) {
		extent = extent_;
		return *this;
	}

	template <typename... Args>
	requires std::is_constructible_v <vk::ClearColorValue, Args...>
	RenderPassBeginInfo &clear_color(size_t index, const Args &...args) {
		clear_values[index] = vk::ClearColorValue(args...);
		return *this;
	}

	template <typename... Args>
	requires std::is_constructible_v <vk::ClearDepthStencilValue, Args...>
	RenderPassBeginInfo &clear_depth(size_t index, const Args &...args) {
		clear_values[index] = vk::ClearDepthStencilValue(args...);
		return *this;
	}

	RenderPassBeginInfo &clear_value(size_t index, vk::ClearValue clear_value) {
		clear_values[index] = clear_value;
		return *this;
	}

	RenderPassBeginInfo &begin(const vk::CommandBuffer &cmd, const vk::SubpassContents contents = vk::SubpassContents::eInline) {
		cmd.beginRenderPass((vk::RenderPassBeginInfo) *this, contents);
		return *this;
	}
};

// Configuring viewport and scissor
struct RenderArea {
	vk::Extent2D extent;

	RenderArea() = delete;
	RenderArea(const vk::Extent2D &extent_) : extent(extent_) {}
	RenderArea(const Window &window) : extent(window.extent) {}
};

inline void viewport_and_scissor(const vk::CommandBuffer &cmd,
				 const vk::Extent2D &extent)
{
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) extent.width,
		(float) extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor { {}, extent };

	cmd.setViewport(0, viewport);
	cmd.setScissor(0, scissor);
};

inline void viewport_and_scissor(const vk::CommandBuffer &cmd,
				 const RenderArea &area)
{
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) area.extent.width,
		(float) area.extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor { {}, area.extent };

	cmd.setViewport(0, viewport);
	cmd.setScissor(0, scissor);
};

// Syncronization primitive for presentation
struct PresentSyncronization {
	std::vector <vk::Semaphore> image_available;
	std::vector <vk::Semaphore> render_finished;
	std::vector <vk::Fence> in_flight;

	struct Frame {
		const vk::Semaphore &image_available;
		const vk::Semaphore &render_finished;
		const vk::Fence &in_flight;
	};

	Frame operator[](size_t index) const {
		return Frame {
			image_available[index],
			render_finished[index],
			in_flight[index]
		};
	}
};

inline void destroy_present_syncronization(const vk::Device &device,
					   const PresentSyncronization &sync)
{
	for (const vk::Semaphore &semaphore : sync.image_available)
		device.destroySemaphore(semaphore);

	for (const vk::Semaphore &semaphore : sync.render_finished)
		device.destroySemaphore(semaphore);

	for (const vk::Fence &fence : sync.in_flight)
		device.destroyFence(fence);
}

// Return proxy for present syncronization
using PresentSyncronizationReturnProxy = DeviceReturnProxy<PresentSyncronization, destroy_present_syncronization>;

inline PresentSyncronizationReturnProxy
present_syncronization(const vk::Device &device, uint32_t frames_in_flight)
{
	PresentSyncronization sync;

	// Default semaphores
	vk::SemaphoreCreateInfo semaphore_info {};

	// Signaled fences
	vk::FenceCreateInfo fence_info {};
	fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

	for (uint32_t i = 0; i < frames_in_flight; i++) {
		sync.image_available.push_back(
			device.createSemaphore(semaphore_info));
		sync.render_finished.push_back(
			device.createSemaphore(semaphore_info));
		sync.in_flight.push_back(device.createFence(fence_info));
	}

	return sync;
}

struct SurfaceOperation {
	enum {
		eOk,
		eResize,
		eFailed
	} status;

	uint32_t index;
};

inline SurfaceOperation acquire_image(const vk::Device &device,
				      const vk::SwapchainKHR &swapchain,
				      const PresentSyncronization::Frame &sync_frame)
{
	// Wait for previous frame to finish
	(void) device.waitForFences(sync_frame.in_flight, VK_TRUE, UINT64_MAX);

	// Acquire image
	vk::Result result;
	uint32_t image_index;

	try {
		std::tie(result, image_index) = device.acquireNextImageKHR(swapchain,
			UINT64_MAX, sync_frame.image_available, nullptr);
	} catch (vk::OutOfDateKHRError &) {
		microlog::warning("acquire_image", "Swapchain out of date\n");
		return { SurfaceOperation::eResize, 0 };
	}

	if (result == vk::Result::eErrorOutOfDateKHR) {
		microlog::warning("acquire_image", "Swapchain out of date\n");
		return { SurfaceOperation::eResize, 0 };
	} else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
		microlog::error("acquire_image", "Failed to acquire swapchain image\n");
		return { SurfaceOperation::eFailed, 0 };
	}

	// Reset fence to prepare for next frame
	device.resetFences(sync_frame.in_flight);

	return { SurfaceOperation::eOk, image_index };
}

inline SurfaceOperation present_image(const vk::Queue &queue,
		                      const vk::SwapchainKHR &swapchain,
				      const std::optional <PresentSyncronization::Frame> &sync_frame,
				      uint32_t index)
{
	std::vector <vk::Semaphore> wait_semaphores;
	if (sync_frame)
		wait_semaphores.push_back(sync_frame->render_finished);

	vk::PresentInfoKHR present_info { wait_semaphores, swapchain, index };

	try {
		// TODO: check return value here
		(void) queue.presentKHR(present_info);
	} catch (vk::OutOfDateKHRError &) {
		microlog::warning("present_image", "Swapchain out of date\n");
		return { SurfaceOperation::eResize, 0 };
	}

	return { SurfaceOperation::eOk, 0 };
}

// Check if a physical device supports a set of extensions
inline bool physical_device_able(const vk::PhysicalDevice &phdev,
				 const std::vector<const char *> &extensions)
{
	// Get the device extensions
	std::vector<vk::ExtensionProperties> exts;
	exts = phdev.enumerateDeviceExtensionProperties();

	// Check if all the extensions are supported
	for (const char *extension : extensions) {
		auto finder = [&extension](const vk::ExtensionProperties &prop) {
			return !strcmp(prop.extensionName, extension);
		};

		if (std::find_if(exts.begin(), exts.end(), finder) == exts.end()) {
			microlog::warning("physical_device_able",
					  "Extension \"%s\" is not supported\n",
					  extension);
			return false;
		}
	}

	return true;
}

// Pick physical device according to some criteria
inline vk::PhysicalDevice pick_physical_device(
	const std::function<bool(const vk::PhysicalDevice &)> &predicate)
{
	// Get all the physical devices
	auto devices = detail::get_vulkan_instance().enumeratePhysicalDevices();

	// Find the first one that satisfies the predicate
	for (const vk::PhysicalDevice &device : devices) {
		if (predicate(device))
			return device;
	}

	// If none found, throw an error
	microlog::error("pick_physical_device", "No physical device found\n");
	return nullptr;
}

// Create logical device on an arbitrary queue
inline vk::Device device(const vk::PhysicalDevice &phdev,
		         const uint32_t queue_family,
			 const uint32_t queue_count,
			 const std::vector <const char *> &extensions,
			 const std::optional <vk::PhysicalDeviceFeatures2KHR> &features = std::nullopt)
{
	// Queue priorities
	std::vector<float> queue_priorities(queue_count, 1.0f);

	// Create the device info
	vk::DeviceQueueCreateInfo queue_info {vk::DeviceQueueCreateFlags(),
					      queue_family, queue_count,
					      queue_priorities.data()};

	// Device features
	vk::PhysicalDeviceFeatures device_features;
	device_features.independentBlend = true;
	device_features.fillModeNonSolid = true;
	device_features.geometryShader = true;

	vk::PhysicalDeviceFeatures2KHR secondary_features;

	// TODO: use the C++ initializer
	vk::DeviceCreateInfo device_info {
		vk::DeviceCreateFlags(), queue_info, {}, extensions,
		&device_features,	 nullptr};

	if (features) {
		secondary_features = *features;
		device_info.pNext = &secondary_features;
		device_info.pEnabledFeatures = nullptr;
	}

	return phdev.createDevice(device_info);
}

// Create a logical device
inline vk::Device device(const vk::PhysicalDevice &phdev,
		         const QueueFamilyIndices &indices,
			 const std::vector <const char *> &extensions,
			 const std::optional <vk::PhysicalDeviceFeatures2KHR> &features = std::nullopt)
{
	auto families = phdev.getQueueFamilyProperties();
	uint32_t count = families[indices.graphics].queueCount;
	return device(phdev, indices.graphics, count, extensions, features);
}

// Imperative initialization approach
[[gnu::always_inline]] inline std::tuple<vk::SurfaceKHR, Window>
surface_handles(const vk::Extent2D &extent, const std::string &title)
{
	Window window = make_window(extent, title);
	return { make_surface(window), window };
}

// Class based initialization approach
struct Skeleton {
	vk::Device device;
	vk::PhysicalDevice phdev = nullptr;
	vk::SurfaceKHR surface;

	vk::Queue graphics_queue;
	vk::Queue present_queue;

	Swapchain swapchain;
	Window window;

	// TODO: no default constructor, this turns into a constructor...
	bool skeletonize(const vk::PhysicalDevice &,
			 const vk::Extent2D &,
			 const std::string &, const std::vector<const char *> &,
			 const std::optional<vk::PhysicalDeviceFeatures2KHR> & = std::nullopt,
			 const std::optional<vk::PresentModeKHR> & = std::nullopt);

	// TODO: virtual destructor
	virtual bool drop();

	void resize();
	float aspect_ratio() const;
};

inline bool Skeleton::skeletonize(
	const vk::PhysicalDevice &phdev_, const vk::Extent2D &extent,
	const std::string &title,
	const std::vector<const char *> &device_extensions,
	const std::optional<vk::PhysicalDeviceFeatures2KHR> &features,
	const std::optional<vk::PresentModeKHR> &priority_present_mode)
{
	phdev = phdev_;
	window = make_window(extent, title);
	surface = make_surface(window);

	QueueFamilyIndices queue_family = find_queue_families(phdev, surface);
	device = littlevk::device(phdev, queue_family, device_extensions, features);
	swapchain = littlevk::swapchain(phdev, device, surface, window.extent,
					queue_family, priority_present_mode);

	graphics_queue = device.getQueue(queue_family.graphics, 0);
	present_queue = device.getQueue(queue_family.present, 0);

	return true;
}

inline bool Skeleton::drop()
{
	device.waitIdle();
	destroy_swapchain(device, swapchain);
	detail::get_vulkan_instance().destroySurfaceKHR(surface);
	device.destroy();
	return true;
}

inline void Skeleton::resize()
{
	int new_width = 0;
	int new_height = 0;

	int current_width = 0;
	int current_height = 0;

	do {
		glfwGetFramebufferSize(window.handle, &current_width,
				       &current_height);
		while (current_width == 0 || current_height == 0) {
			glfwWaitEvents();
			glfwGetFramebufferSize(window.handle, &current_width,
					       &current_height);
		}

		glfwGetFramebufferSize(window.handle, &new_width, &new_height);
	} while (new_width != current_width || new_height != current_height);

	// Resize only after stable sizes
	vk::SurfaceCapabilitiesKHR caps =
		phdev.getSurfaceCapabilitiesKHR(surface);
	new_width = std::clamp(new_width, int(caps.minImageExtent.width),
			       int(caps.maxImageExtent.width));
	new_height = std::clamp(new_height, int(caps.minImageExtent.height),
				int(caps.maxImageExtent.height));

	device.waitIdle();

	vk::Extent2D new_extent = {uint32_t(new_width), uint32_t(new_height)};
	littlevk::resize(device, swapchain, new_extent);
	window.extent = new_extent;
}

inline float Skeleton::aspect_ratio() const
{
	return (float) window.extent.width / (float) window.extent.height;
}

// Primary rendering loop
inline void swapchain_render_loop(const vk::Device &device,
				  const vk::Queue &graphics_queue,
				  const vk::Queue &present_queue,
				  const vk::CommandPool &command_pool,
				  const Window &window,
				  const Swapchain &swapchain,
				  Deallocator &deallocator,
				  const std::function <void (const vk::CommandBuffer &, uint32_t)> &render,
				  const std::function <void ()> &resize)
{
	uint32_t count = swapchain.images.size();

	auto sync = littlevk::present_syncronization(device, count).unwrap(deallocator);

	auto command_buffers = device.allocateCommandBuffers({
		command_pool,
		vk::CommandBufferLevel::ePrimary,
		count
	});

	uint32_t frame = 0;

	while (!glfwWindowShouldClose(window.handle)) {
		glfwPollEvents();

		littlevk::SurfaceOperation op;
		op = littlevk::acquire_image(device, swapchain.handle, sync[frame]);
		if (op.status == littlevk::SurfaceOperation::eResize) {
			resize();
			continue;
		}

		// Start the render pass
		const auto &cmd = command_buffers[frame];

		cmd.begin(vk::CommandBufferBeginInfo {});
			render(cmd, op.index);
		cmd.end();

		// Submit command buffer while signaling the semaphore
		constexpr vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

		vk::SubmitInfo submit_info {
			sync.image_available[frame],
			wait_stage, cmd,
			sync.render_finished[frame]
		};

		graphics_queue.submit(submit_info, sync.in_flight[frame]);

		op = littlevk::present_image(present_queue, swapchain.handle, sync[frame], op.index);
		if (op.status == littlevk::SurfaceOperation::eResize)
			resize();

		frame = (frame + 1) % count;
	}

	device.waitIdle();
}

// Get memory file decriptor
inline int find_memory_fd(const vk::Device &device,
			  const vk::DeviceMemory &memory)
{
	vk::MemoryGetFdInfoKHR info {};
	info.memory = memory;
	info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	VkMemoryGetFdInfoKHR cinfo = static_cast<VkMemoryGetFdInfoKHR>(info);

	int fd = -1;
	vkGetMemoryFdKHR(device, &cinfo, &fd);
	return fd;
}

// Vulkan buffer wrapper
struct Buffer {
	vk::Buffer buffer;
	vk::DeviceMemory memory;
	vk::MemoryRequirements requirements = {};

	vk::Buffer operator*() const {
		return buffer;
	}

	vk::DeviceSize device_size() const {
		return requirements.size;
	}

	vk::DescriptorBufferInfo descriptor() const {
		return vk::DescriptorBufferInfo()
			.setBuffer(buffer)
			.setRange(requirements.size)
			.setOffset(0);
	}
};

// Return proxy for buffers
static void destroy_buffer(const vk::Device &device, const Buffer &buffer)
{
	device.destroyBuffer(buffer.buffer);
	device.freeMemory(buffer.memory);
}

using BufferReturnProxy = DeviceReturnProxy<Buffer, destroy_buffer>;

// Find memory type
inline uint32_t
find_memory_type(const vk::PhysicalDeviceMemoryProperties &mem_props,
		 uint32_t type_filter, vk::MemoryPropertyFlags properties)
{
	uint32_t type_index = uint32_t(~0);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_filter & 1) &&
		    (mem_props.memoryTypes[i].propertyFlags & properties) ==
			    properties) {
			type_index = i;
			break;
		}

		type_filter >>= 1;
	}

	if (type_index == uint32_t(~0)) {
		microlog::error("find_memory_type", "No memory type found\n");
		return std::numeric_limits<uint32_t>::max();
	}

	return type_index;
}

inline BufferReturnProxy
buffer(const vk::Device &device,
       const vk::PhysicalDeviceMemoryProperties &properties,
       size_t size,
       const vk::BufferUsageFlags &flags,
       bool external = false)
{
	Buffer buffer;

	vk::BufferCreateInfo buffer_info {
		{}, size, flags, vk::SharingMode::eExclusive, 0, nullptr};

	// Exporting the buffer data for other APIs (e.g. CUDA)
	vk::ExternalMemoryBufferCreateInfo external_info {};
	if (external) {
		external_info.handleTypes =
			vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		buffer_info.pNext = &external_info;
	}

	// Allocate buffer info
	buffer.buffer = device.createBuffer(buffer_info);
	buffer.requirements = device.getBufferMemoryRequirements(buffer.buffer);

	vk::MemoryAllocateInfo buffer_alloc_info {
		buffer.requirements.size,
		find_memory_type(
			properties, buffer.requirements.memoryTypeBits,
			vk::MemoryPropertyFlagBits::eHostVisible |
				vk::MemoryPropertyFlagBits::eHostCoherent)};

	// Export the buffer data for other APIs (e.g. CUDA)
	// TODO: general function
	vk::ExportMemoryAllocateInfo export_info {};
	if (external) {
		export_info.handleTypes =
			vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		buffer_alloc_info.pNext = &export_info;
	}

	// Allocate the device buffer
	buffer.memory = device.allocateMemory(buffer_alloc_info);
	device.bindBufferMemory(buffer.buffer, buffer.memory, 0);

	return buffer;
}

using FilledBufferReturnProxy = ComposedReturnProxy<Buffer>;

template <typename T>
inline FilledBufferReturnProxy
buffer(const vk::Device &device,
       const vk::PhysicalDeviceMemoryProperties &properties,
       const std::vector <T> &vec,
       const vk::BufferUsageFlags &flags,
       bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties,
		vec.size() * sizeof(T), flags, external).defer(dq);
	upload(device, buffer, vec);
	return { buffer, dq };
}

template <typename T, size_t N>
inline FilledBufferReturnProxy
buffer(const vk::Device &device,
       const vk::PhysicalDeviceMemoryProperties &properties,
       const std::array <T, N> &array,
       const vk::BufferUsageFlags &flags,
       bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties,
		array.size() * sizeof(T), flags, external).defer(dq);
	upload(device, buffer, array);
	return { buffer, dq };
}

template <typename T>
inline FilledBufferReturnProxy
buffer(const vk::Device &device,
       const vk::PhysicalDeviceMemoryProperties &properties,
       const T *const data,
       size_t size,
       const vk::BufferUsageFlags &flags,
       bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties,
		size, flags, external).defer(dq);
	upload(device, buffer, data);
	return { buffer, dq };
}

// TODO: overload with size
inline void upload(const vk::Device &device, const Buffer &buffer,
		   const void *data)
{
	void *mapped = device.mapMemory(buffer.memory, 0, buffer.requirements.size);
	std::memcpy(mapped, data, buffer.requirements.size);
	device.unmapMemory(buffer.memory);
}

template <typename T>
inline void upload(const vk::Device &device,
		   const Buffer &buffer,
		   const std::vector <T> &vec)
{
	size_t size = std::min(buffer.requirements.size, vec.size() * sizeof(T));
	void *mapped = device.mapMemory(buffer.memory, 0, size);
	std::memcpy(mapped, vec.data(), size);
	device.unmapMemory(buffer.memory);

	// Warn if fewer elements were transferred
	if (size < vec.size() * sizeof(T)) {
		microlog::warning("upload",
			"Fewer elements were transferred from the "
			"vector than may have been expected\n");
	}
}

template <typename T, size_t N>
inline void upload(const vk::Device &device, const Buffer &buffer,
		   const std::array <T, N> &arr)
{
	size_t size = std::min(buffer.requirements.size, arr.size() * sizeof(T));
	void *mapped = device.mapMemory(buffer.memory, 0, size);
	std::memcpy(mapped, arr.data(), size);
	device.unmapMemory(buffer.memory);

	// Warn if fewer elements were transferred
	if (size < N * sizeof(T)) {
		microlog::warning("upload",
			"Fewer elements were transferred from the "
			"array than may have been expected");
	}
}

inline void download(const vk::Device &device, const Buffer &buffer, void *data)
{
	void *mapped = device.mapMemory(buffer.memory, 0, buffer.requirements.size);
	std::memcpy(data, mapped, buffer.requirements.size);
	device.unmapMemory(buffer.memory);
}

template <typename T>
inline void download(const vk::Device &device, const Buffer &buffer,
		     std::vector<T> &vec)
{
	size_t size =
		std::min(buffer.requirements.size, vec.size() * sizeof(T));
	void *mapped = device.mapMemory(buffer.memory, 0, size);
	std::memcpy(vec.data(), mapped, size);
	device.unmapMemory(buffer.memory);

	// TODO: warn
}

// Vulkan image wrapper
struct Image {
	vk::Image image;
	vk::ImageView view;
	vk::DeviceMemory memory;
	vk::MemoryRequirements requirements;
	vk::Extent2D extent;
	vk::ImageLayout layout;

	Image() : image(VK_NULL_HANDLE),
		  view(VK_NULL_HANDLE),
		  memory(VK_NULL_HANDLE),
		  extent(0, 0) {}

	vk::Image operator*() const {
		return image;
	}

	vk::DeviceSize device_size() const {
		return requirements.size;
	}

	operator bool() const {
		return image;
	}

	void transition(const vk::CommandBuffer &, const vk::ImageLayout &);
};

// Return proxy for images
inline void destroy_image(const vk::Device &device, const Image &image)
{
	device.destroyImageView(image.view);
	device.destroyImage(image.image);
	device.freeMemory(image.memory);
}

using ImageReturnProxy = DeviceReturnProxy <Image, destroy_image>;

// Create image
struct ImageCreateInfo {
	uint32_t width;
	uint32_t height;
	vk::Format format;
	vk::ImageUsageFlags usage;
	vk::ImageAspectFlags aspect;
	vk::ImageType type;
	vk::ImageViewType view;
	bool external;

	constexpr ImageCreateInfo(uint32_t width_,
				  uint32_t height_,
				  vk::Format format_,
				  vk::ImageUsageFlags usage_, vk::ImageAspectFlags aspect_,
				  vk::ImageType type_ = vk::ImageType::e2D,
				  vk::ImageViewType view_ = vk::ImageViewType::e2D,
				  bool external_ = false)
	    : width(width_), height(height_), format(format_), usage(usage_),
	      aspect(aspect_), type(type_), view(view_), external(external_) {}

	constexpr ImageCreateInfo(vk::Extent2D extent,
				  vk::Format format_,
				  vk::ImageUsageFlags usage_, vk::ImageAspectFlags aspect_,
				  vk::ImageType type_ = vk::ImageType::e2D,
				  vk::ImageViewType view_ = vk::ImageViewType::e2D,
				  bool external_ = false)
	    : width(extent.width), height(extent.height), format(format_),
	      usage(usage_), aspect(aspect_), type(type_), view(view_),
	      external(external_) {}
};

inline ImageReturnProxy image(const vk::Device &device,
			      const ImageCreateInfo &info,
			      const vk::PhysicalDeviceMemoryProperties &properties)
{
	Image image;

	vk::ImageCreateInfo image_info {
		{},
		info.type, info.format,
		vk::Extent3D { info.width, info.height, 1 },
		1, 1,
		vk::SampleCountFlagBits::e1,
		vk::ImageTiling::eOptimal,
		info.usage,
		vk::SharingMode::eExclusive,
		0, nullptr,
		vk::ImageLayout::eUndefined
	};

	vk::ExternalMemoryImageCreateInfo external_info {};
	if (info.external) {
		external_info.handleTypes =
			vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		image_info.pNext = &external_info;
	}

	image.image = device.createImage(image_info);
	image.requirements = device.getImageMemoryRequirements(image.image);

	vk::MemoryAllocateInfo alloc_info {
		image.requirements.size,
		find_memory_type(properties, image.requirements.memoryTypeBits,
				 vk::MemoryPropertyFlagBits::eDeviceLocal)};

	vk::ExportMemoryAllocateInfo export_info {};
	if (info.external) {
		export_info.handleTypes =
			vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		alloc_info.pNext = &export_info;
	}

	image.memory = device.allocateMemory(alloc_info);
	device.bindImageMemory(image.image, image.memory, 0);

	vk::ImageViewCreateInfo view_info {
		{}, image.image,
		info.view, info.format,
		vk::ComponentMapping {
			vk::ComponentSwizzle::eIdentity,
			vk::ComponentSwizzle::eIdentity,
			vk::ComponentSwizzle::eIdentity,
			vk::ComponentSwizzle::eIdentity
		},
		vk::ImageSubresourceRange {
			info.aspect, 0, 1, 0, 1
		}
	};

	image.view = device.createImageView(view_info);
	image.extent = vk::Extent2D { info.width, info.height };
	image.layout = vk::ImageLayout::eUndefined;

	return image;
}

// TODO: pure template version that skips switch statements
template <typename ImageType>
inline void transition(const vk::CommandBuffer &cmd,
		       const ImageType &image,
		       const vk::ImageLayout old_layout,
		       const vk::ImageLayout new_layout)
{
	static_assert(std::is_same_v <ImageType, Image>
			|| std::is_same_v <ImageType, vk::Image>,
			"littlevk::transition: ImageType must be either "
			"littlevk::Image or vk::Image");

	// Source stage
	vk::AccessFlags src_access_mask = {};

	switch (old_layout) {
	case vk::ImageLayout::eColorAttachmentOptimal:
		src_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::ePresentSrcKHR:
		src_access_mask = vk::AccessFlagBits::eMemoryRead;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
		src_access_mask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		src_access_mask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::ePreinitialized:
		src_access_mask = vk::AccessFlagBits::eHostWrite;
		break;
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::eUndefined:
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		src_access_mask = vk::AccessFlagBits::eShaderRead;
		break;
	default:
		microlog::error("transition layout",
				"Unsupported old layout %s",
				vk::to_string(old_layout).c_str());
		break;
	}

	// Pipeline stage
	vk::PipelineStageFlags source_stage;
	switch (old_layout) {
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePreinitialized:
		source_stage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eColorAttachmentOptimal:
		source_stage =
			vk::PipelineStageFlagBits::eColorAttachmentOutput;
		break;
	case vk::ImageLayout::ePresentSrcKHR:
		source_stage = vk::PipelineStageFlagBits::eBottomOfPipe;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		source_stage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eUndefined:
		source_stage = vk::PipelineStageFlagBits::eTopOfPipe;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		source_stage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	default:
		microlog::error("transition layout",
				"Unsupported old layout %s",
				vk::to_string(old_layout).c_str());
		break;
	}

	// Destination stage
	vk::AccessFlags dst_access_mask = {};
	switch (new_layout) {
	case vk::ImageLayout::eColorAttachmentOptimal:
		dst_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		dst_access_mask =
			vk::AccessFlagBits::eDepthStencilAttachmentRead |
			vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		break;
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePresentSrcKHR:
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		dst_access_mask = vk::AccessFlagBits::eShaderRead;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		dst_access_mask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
		dst_access_mask = vk::AccessFlagBits::eTransferWrite;
		break;
	default:
		microlog::error("transition layout",
				"Unsupported new layout %s",
				vk::to_string(new_layout).c_str());
		break;
	}

	// Destination stage
	vk::PipelineStageFlags destination_stage;
	switch (new_layout) {
	case vk::ImageLayout::eColorAttachmentOptimal:
		destination_stage =
			vk::PipelineStageFlagBits::eColorAttachmentOutput;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destination_stage =
			vk::PipelineStageFlagBits::eEarlyFragmentTests;
		break;
	case vk::ImageLayout::eGeneral:
		destination_stage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::ePresentSrcKHR:
		destination_stage = vk::PipelineStageFlagBits::eBottomOfPipe;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destination_stage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		destination_stage = vk::PipelineStageFlagBits::eTransfer;
		break;
	default:
		microlog::error("transition layout",
				"Unsupported new layout %s",
				vk::to_string(new_layout).c_str());
		break;
	}

	// Aspect mask
	vk::ImageAspectFlags aspect_mask;
	if (new_layout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
		aspect_mask = vk::ImageAspectFlagBits::eDepth;
	else
		aspect_mask = vk::ImageAspectFlagBits::eColor;

	// Create the barrier
	vk::ImageSubresourceRange image_subresource_range {
		aspect_mask,
		0, 1, 0, 1
	};

	vk::Image target_image;
	if constexpr (std::is_same_v<ImageType, littlevk::Image>)
		target_image = *image;
	else
		target_image = image;

	vk::ImageMemoryBarrier barrier {
		src_access_mask,
		dst_access_mask,
		old_layout,
		new_layout,
		VK_QUEUE_FAMILY_IGNORED,
		VK_QUEUE_FAMILY_IGNORED,
		target_image,
		image_subresource_range
	};

	// Add the barrier
	return cmd.pipelineBarrier(source_stage, destination_stage, {}, {}, {}, barrier);
}

// Same for the methods
inline void Image::transition(const vk::CommandBuffer &cmd, const vk::ImageLayout &layout_)
{
	littlevk::transition(cmd, *this, layout, layout_);
	layout = layout_;
}

// Copying buffer to image
inline void copy_buffer_to_image(const vk::CommandBuffer &cmd,
				 const vk::Image &image,
				 const Buffer &buffer,
				 const vk::Extent2D &extent,
				 const vk::ImageLayout &layout)
{
	// TODO: ensure same sizes...,
	// TODO: pass format as well...
	vk::BufferImageCopy region {
		0, 0, 0,
		vk::ImageSubresourceLayers {
			vk::ImageAspectFlagBits::eColor,
			0, 0, 1
		},
		vk::Offset3D { 0, 0, 0 },
		vk::Extent3D { extent.width, extent.height, 1 }
	};

	cmd.copyBufferToImage(*buffer, image, layout, region);
}

inline void copy_buffer_to_image(const vk::CommandBuffer &cmd,
				 const Image &image,
				 const Buffer &buffer,
				 const vk::ImageLayout &layout)
{
	// TODO: ensure same sizes...,
	vk::BufferImageCopy region {
		0, 0, 0,
		vk::ImageSubresourceLayers {
			vk::ImageAspectFlagBits::eColor,
			0, 0, 1
		},
		vk::Offset3D { 0, 0, 0 },
		vk::Extent3D { image.extent.width, image.extent.height, 1 }
	};

	cmd.copyBufferToImage(*buffer, *image, layout, region);
}

// Copying image to buffer
inline void copy_image_to_buffer(const vk::CommandBuffer &cmd,
				 const vk::Image &image,
				 const Buffer &buffer,
				 const vk::Extent2D &extent,
				 const vk::ImageLayout &layout)
{
	vk::BufferImageCopy region {
		0, 0, 0,
		vk::ImageSubresourceLayers {
			vk::ImageAspectFlagBits::eColor,
			0, 0, 1
		},
		vk::Offset3D { 0, 0, 0 },
		vk::Extent3D { extent.width, extent.height, 1 }
	};

	cmd.copyImageToBuffer(image, layout, *buffer, region);
}

inline void copy_image_to_buffer(const vk::CommandBuffer &cmd,
				 const Image &image,
				 const Buffer &buffer,
				 const vk::ImageLayout &layout)
{
	vk::BufferImageCopy region {
		0,
		0,
		0,
		vk::ImageSubresourceLayers {
			vk::ImageAspectFlagBits::eColor,
			0, 0, 1
		},
		vk::Offset3D { 0, 0, 0 },
		vk::Extent3D { image.extent.width, image.extent.height, 1 }
	};

	cmd.copyImageToBuffer(*image, layout, *buffer, region);
}

// Binding resources to descriptor sets
inline void bind_descriptor_set(const vk::Device &device,
		                const vk::DescriptorSet &dset,
				const Image &img,
				const vk::Sampler &sampler)
{
	vk::DescriptorImageInfo image_info {
		sampler, img.view,
		vk::ImageLayout::eShaderReadOnlyOptimal
	};

	vk::WriteDescriptorSet write {
		dset, 0, 0, 1,
		vk::DescriptorType::eCombinedImageSampler,
		&image_info
	};

	device.updateDescriptorSets({write}, {});
}

inline void bind_descriptor_set(const vk::Device &device,
		                const vk::DescriptorSet &dset,
				const Buffer &buffer,
				uint32_t binding)
{
	vk::DescriptorBufferInfo buffer_info {
		*buffer, 0, vk::WholeSize
	};

	vk::WriteDescriptorSet write {
		dset, binding,
		0, 1, vk::DescriptorType::eStorageBuffer,
		{}, &buffer_info
	};

	device.updateDescriptorSets({write}, {});
}

// Construct framebuffer from image
inline FramebufferReturnProxy framebuffer(const vk::Device &device,
					  const vk::RenderPass &rp,
					  const littlevk::Image &image)
{
	std::array <vk::ImageView, 1> attachments = {image.view};

	vk::FramebufferCreateInfo framebuffer_info {
		{}, rp,
		(uint32_t) attachments.size(), attachments.data(),
		image.extent.width, image.extent.height, 1
	};

	return device.createFramebuffer(framebuffer_info);
}

// Single-time command buffer submission
inline void submit_now(const vk::Device &device,
		       const vk::CommandPool &pool,
		       const vk::Queue &queue,
		       const std::function <void (const vk::CommandBuffer &)> &function)
{
	vk::CommandBuffer cmd = device
		.allocateCommandBuffers(vk::CommandBufferAllocateInfo {
			pool, vk::CommandBufferLevel::ePrimary, 1
		}).front();

	cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
		function(cmd);
	cmd.end();

	vk::SubmitInfo submit_info {
		0, nullptr, nullptr,
		1, &cmd, 0, nullptr
	};

	// TODO: check
	(void) queue.submit(1, &submit_info, nullptr);
	queue.waitIdle();

	device.freeCommandBuffers(pool, 1, &cmd);
}

// Other companion functions with automatic memory management
static void destroy_command_pool(const vk::Device &device,
				 const vk::CommandPool &pool)
{
	device.destroyCommandPool(pool);
}

using CommandPoolReturnProxy = DeviceReturnProxy <vk::CommandPool, destroy_command_pool>;

inline CommandPoolReturnProxy command_pool(const vk::Device &device,
		                           const vk::CommandPoolCreateInfo &info)
{
	vk::CommandPool pool;
	if (device.createCommandPool(&info, nullptr, &pool) != vk::Result::eSuccess)
		return true;

	return std::move(pool);
}

template <typename ... Args>
inline CommandPoolReturnProxy command_pool(const vk::Device &device, const Args &... args)
{
	vk::CommandPoolCreateInfo info { args... };

	vk::CommandPool pool;
	if (device.createCommandPool(&info, nullptr, &pool) != vk::Result::eSuccess)
		return true;

	return std::move(pool);
}

template <typename ... Args>
inline auto command_buffers(const vk::Device &device, const vk::CommandPool &pool, const Args &... args)
{
	return device.allocateCommandBuffers({ pool, args... });
}

static void destroy_descriptor_pool(const vk::Device &device,
				    const vk::DescriptorPool &pool)
{
	device.destroyDescriptorPool(pool);
}

using DescriptorPoolReturnProxy = DeviceReturnProxy <vk::DescriptorPool, destroy_descriptor_pool>;

inline DescriptorPoolReturnProxy descriptor_pool(const vk::Device &device,
		                                 const vk::DescriptorPoolCreateInfo &info)
{
	vk::DescriptorPool pool;
	if (device.createDescriptorPool(&info, nullptr, &pool) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(pool);
}

static void destroy_descriptor_set_layout(const vk::Device &device,
					  const vk::DescriptorSetLayout &layout)
{
	device.destroyDescriptorSetLayout(layout);
}

using DescriptorSetLayoutReturnProxy = DeviceReturnProxy<vk::DescriptorSetLayout, destroy_descriptor_set_layout>;

inline DescriptorSetLayoutReturnProxy
descriptor_set_layout(const vk::Device &device,
		      const vk::DescriptorSetLayoutCreateInfo &info)
{
	vk::DescriptorSetLayout layout;
	if (device.createDescriptorSetLayout(&info, nullptr, &layout) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(layout);
}

static void destroy_pipeline_layout(const vk::Device &device,
				    const vk::PipelineLayout &layout)
{
	device.destroyPipelineLayout(layout);
}

using PipelineLayoutReturnProxy =
	DeviceReturnProxy<vk::PipelineLayout, destroy_pipeline_layout>;

inline PipelineLayoutReturnProxy
pipeline_layout(const vk::Device &device,
		const vk::PipelineLayoutCreateInfo &info)
{
	vk::PipelineLayout layout;
	if (device.createPipelineLayout(&info, nullptr, &layout) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(layout);
}

static void destroy_sampler(const vk::Device &device,
			    const vk::Sampler &sampler)
{
	device.destroySampler(sampler);
}

using SamplerReturnProxy = DeviceReturnProxy<vk::Sampler, destroy_sampler>;

inline SamplerReturnProxy sampler(const vk::Device &device,
				  const vk::SamplerCreateInfo &info)
{
	vk::Sampler sampler;
	if (device.createSampler(&info, nullptr, &sampler) !=
	    vk::Result::eSuccess)
		return true;

	return std::move(sampler);
}

// Default sampler builder
struct SamplerAssembler {
	const vk::Device &device;
	Deallocator &dal;

	vk::Filter mag = vk::Filter::eLinear;
	vk::Filter min = vk::Filter::eLinear;
	vk::SamplerMipmapMode mip = vk::SamplerMipmapMode::eLinear;

	SamplerAssembler(const vk::Device &device, Deallocator &dal)
		: device(device), dal(dal) {}

	SamplerAssembler &filtering(vk::Filter mode) {
		mag = mode;
		min = mode;
		return *this;
	}

	SamplerAssembler &mipping(vk::SamplerMipmapMode mode) {
		mip = mode;
		return *this;
	}

	operator vk::Sampler() const {
		vk::SamplerCreateInfo info {
			vk::SamplerCreateFlags {},
			mag,
			min,
			mip,
			vk::SamplerAddressMode::eRepeat,
			vk::SamplerAddressMode::eRepeat,
			vk::SamplerAddressMode::eRepeat,
			0.0f,
			vk::False,
			1.0f,
			vk::False,
			vk::CompareOp::eAlways,
			0.0f,
			0.0f,
			vk::BorderColor::eIntOpaqueBlack,
			vk::False
		};

		return sampler(device, info).unwrap(dal);
	}
};

// Bind pattern for all physical-logical device operations
struct LinkedDevices {
	const vk::PhysicalDevice &phdev;
	const vk::Device &device;

	constexpr LinkedDevices(const vk::PhysicalDevice &phdev_,
				const vk::Device &device_)
		: phdev(phdev_), device(device_) {}

	LinkedDevices &resize(const vk::SurfaceKHR &surface,
			      Window &window,
			      Swapchain &swapchain) {
		int new_width = 0;
		int new_height = 0;

		int current_width = 0;
		int current_height = 0;

		do {
			glfwGetFramebufferSize(window.handle, &current_width, &current_height);
			while (current_width == 0 || current_height == 0) {
				glfwWaitEvents();
				glfwGetFramebufferSize(window.handle,
						       &current_width,
						       &current_height);
			}

			glfwGetFramebufferSize(window.handle, &new_width, &new_height);
		} while (new_width != current_width || new_height != current_height);

		// Resize only after stable sizes
		vk::SurfaceCapabilitiesKHR caps = phdev.getSurfaceCapabilitiesKHR(surface);
		new_width = std::clamp(new_width, int(caps.minImageExtent.width), int(caps.maxImageExtent.width));
		new_height = std::clamp(new_height, int(caps.minImageExtent.height), int(caps.maxImageExtent.height));

		device.waitIdle();

		vk::Extent2D new_extent = {uint32_t(new_width),
					   uint32_t(new_height)};
		littlevk::resize(device, swapchain, new_extent);
		window.extent = new_extent;

		return *this;
	}

	// TODO: replace?
	Swapchain swapchain(const vk::SurfaceKHR &surface, const vk::Extent2D &extent, const QueueFamilyIndices &indices) {
		return littlevk::swapchain(phdev, device, surface, extent, indices);
	}

	Swapchain swapchain(const vk::SurfaceKHR &surface, const QueueFamilyIndices &indices) {
		vk::SurfaceCapabilitiesKHR capabilities = phdev.getSurfaceCapabilitiesKHR(surface);
		return littlevk::swapchain(phdev, device, surface, capabilities.currentExtent, indices);
	}
};

constexpr inline LinkedDevices bind(const vk::PhysicalDevice &phdev,
				    const vk::Device &device)
{
	return LinkedDevices(phdev, device);
}

// Bind pattern for command submission
struct LinkedCommandQueue {
	const vk::Device &device;
	const vk::CommandPool &pool;
	const vk::Queue &queue;

	template <typename F>
	requires std::is_invocable_r_v <void, F, vk::CommandBuffer>
	LinkedCommandQueue &submit(const F &ftn) {
		vk::CommandBuffer cmd = device.allocateCommandBuffers(
			vk::CommandBufferAllocateInfo {
				pool, vk::CommandBufferLevel::ePrimary, 1
			}
		).front();

		cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
			ftn(cmd);
		cmd.end();

		vk::SubmitInfo submit_info {
			0, nullptr, nullptr,
			1, &cmd, 0, nullptr
		};

		// TODO: check
		(void) queue.submit(1, &submit_info, nullptr);

		return *this;
	}

	template <typename F>
	requires std::is_invocable_r_v <void, F, vk::CommandBuffer>
	LinkedCommandQueue &submit_and_wait(const F &ftn) {
		vk::CommandBuffer cmd = device.allocateCommandBuffers(
			vk::CommandBufferAllocateInfo {
				pool, vk::CommandBufferLevel::ePrimary, 1
			}
		).front();

		cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
			ftn(cmd);
		cmd.end();

		vk::SubmitInfo submit_info {
			0, nullptr, nullptr,
			1, &cmd, 0, nullptr
		};

		// TODO: check
		(void) queue.submit(1, &submit_info, nullptr);

		device.waitIdle();

		return *this;
	}
};

constexpr inline LinkedCommandQueue bind(const vk::Device &device,
		                         const vk::CommandPool &pool,
					 const vk::Queue &queue)
{
	return LinkedCommandQueue(device, pool, queue);
}

// Bind pattern to do all allocations at once, then unpack
template <typename... Args>
struct LinkedDeviceAllocator : std::tuple <Args...> {
	const vk::Device &device;
	const vk::PhysicalDeviceMemoryProperties &properties;
	littlevk::Deallocator &dal;

	constexpr LinkedDeviceAllocator(const vk::Device &device_,
			                const vk::PhysicalDeviceMemoryProperties &properties_,
					littlevk::Deallocator &dal_, const Args &...args)
		: std::tuple <Args...> (args...), device(device_), properties(properties_), dal(dal_) {}

	constexpr LinkedDeviceAllocator(const vk::Device &device_,
			                const vk::PhysicalDeviceMemoryProperties &properties_,
					littlevk::Deallocator &dal_, const std::tuple <Args...> &args)
		: std::tuple <Args...> (args), device(device_), properties(properties_), dal(dal_) {}

	operator const auto &() const {
		if constexpr (sizeof...(Args) == 1)
			return std::get <0> (*this);
		else
			return *this;
	}

	template <typename... InfoArgs>
	requires std::is_constructible_v <ImageCreateInfo, InfoArgs...>
	[[nodiscard]] LinkedDeviceAllocator <Args..., littlevk::Image>
	image(const InfoArgs &...args) {
		ImageCreateInfo info(args...);
		auto image = littlevk::image(device, info, properties).unwrap(dal);
		auto new_values = std::tuple_cat((std::tuple <Args...>) *this, std::make_tuple(image));
		return {device, properties, dal, new_values};
	}

	template <typename... BufferArgs>
	[[nodiscard]] LinkedDeviceAllocator <Args..., littlevk::Buffer>
	buffer(const BufferArgs &...args) {
		auto buffer = littlevk::buffer(device, properties, args...).unwrap(dal);
		auto new_values = std::tuple_cat((std::tuple <Args...>) *this, std::make_tuple(buffer));
		return {device, properties, dal, new_values};
	}
};

// Starts with nothing
constexpr LinkedDeviceAllocator <> bind(const vk::Device &device,
		                        const vk::PhysicalDeviceMemoryProperties &properties,
					littlevk::Deallocator &dal)
{
	return {device, properties, dal};
}

struct LinkedDeviceDescriptorPool {
	const vk::Device &device;
	const vk::DescriptorPool &pool;

	[[nodiscard]] std::vector <vk::DescriptorSet>
	allocate_descriptor_sets(const vk::DescriptorSetLayout &dsl) const
	{
		return device.allocateDescriptorSets(
			vk::DescriptorSetAllocateInfo {pool, dsl});
	}

	[[nodiscard]] std::vector <vk::DescriptorSet>
	allocate_descriptor_sets(const std::vector <vk::DescriptorSetLayout> &dsls) const
	{
		return device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo { pool, dsls });
	}
};

constexpr LinkedDeviceDescriptorPool bind(const vk::Device &device, const vk::DescriptorPool &pool)
{
	return { device, pool };
}

// Descriptor set update structure
struct DescriptorUpdateQueue {
	vk::DescriptorSet descriptor;
	std::vector <vk::DescriptorSetLayoutBinding> bindings;

	DescriptorUpdateQueue(const vk::DescriptorSet &descriptor_,
			      const std::vector <vk::DescriptorSetLayoutBinding> &bindings_)
			: descriptor(descriptor_), bindings(bindings_) {}

	DescriptorUpdateQueue(const vk::DescriptorSet &descriptor_,
			      const std::map <uint32_t, vk::DescriptorSetLayoutBinding> &bindings_)
			: descriptor(descriptor_) {
		for (auto &[k, v] : bindings_) {
			if (k >= bindings.size())
				bindings.resize(k + 1);

			bindings[k] = v;
		}
	}

	// Allow for arbitrarily many updates; enable partial/full updates
	std::list <vk::DescriptorImageInfo> image_infos;
	std::list <vk::DescriptorBufferInfo> buffer_infos;
	std::vector <vk::WriteDescriptorSet> writes;

	DescriptorUpdateQueue &queue_update(uint32_t binding,
					    uint32_t element,
					    const vk::Sampler &sampler,
					    const vk::ImageView &view,
					    const vk::ImageLayout &layout) {
		image_infos.emplace_back(sampler, view, layout);

		auto &info = image_infos.back();
		writes.emplace_back(descriptor, binding, element,
			bindings[binding].descriptorCount,
			bindings[binding].descriptorType,
			&info, nullptr, nullptr);

		return *this;
	}

	DescriptorUpdateQueue &queue_update(uint32_t binding,
					    uint32_t element,
					    const vk::Buffer &buffer,
					    uint32_t offset,
					    uint32_t range) {
		buffer_infos.emplace_back(buffer, offset, range);

		auto &info = buffer_infos.back();
		writes.emplace_back(descriptor, binding, element,
			bindings[binding].descriptorCount,
			bindings[binding].descriptorType,
			nullptr, &info, nullptr);

		return *this;
	}

	void apply(const vk::Device &device) const {
		device.updateDescriptorSets(writes, nullptr);
	}
};

// Descriptor set update structures
// TODO: deprecate
struct LinkedDescriptorUpdater {
	const vk::Device &device;
	const vk::DescriptorSet &dset;
	const std::vector <vk::DescriptorSetLayoutBinding> &bindings;

	// Allow for arbitrarily many updates; enable partial/full updates
	std::vector <vk::DescriptorImageInfo> image_infos;
	std::vector <vk::DescriptorBufferInfo> buffer_infos;
	std::vector <size_t> image_indices;
	std::vector <size_t> buffer_indices;
	std::vector <vk::WriteDescriptorSet> writes;

	LinkedDescriptorUpdater(const vk::Device &device_,
			        const vk::DescriptorSet &dset_,
				const std::vector <vk::DescriptorSetLayoutBinding> &bindings_)
		: device(device_), dset(dset_), bindings(bindings_) {}

	~LinkedDescriptorUpdater() {
		if (writes.empty())
			return;

		microlog::warning("linked_descriptor_updator",
			"Updates to descriptor set (handle = %p) where invoked, "
			"but never finalized.\n", (void *) dset);
	}

	LinkedDescriptorUpdater &queue_update(uint32_t binding,
					      uint32_t element,
					      const vk::Sampler &sampler,
					      const vk::ImageView &view,
					      const vk::ImageLayout &layout) {
		image_infos.emplace_back(sampler, view, layout);
		image_indices.push_back(writes.size());
		writes.emplace_back(dset, binding, element,
				    bindings[binding].descriptorCount,
				    bindings[binding].descriptorType);
		return *this;
	}

	LinkedDescriptorUpdater &queue_update(uint32_t binding,
			                      uint32_t element,
					      const vk::Buffer &buffer,
					      uint64_t offset,
					      uint64_t range) {
		buffer_infos.emplace_back(buffer, offset, range);
		buffer_indices.push_back(writes.size());
		writes.emplace_back(dset, binding, element,
				    bindings[binding].descriptorCount,
				    bindings[binding].descriptorType);
		return *this;
	}

	void finalize() {
		// Assign the addresses
		for (size_t i = 0; i < image_infos.size(); i++) {
			size_t index = image_indices[i];
			writes[index].pImageInfo = &image_infos[i];
		}

		for (size_t i = 0; i < buffer_infos.size(); i++) {
			size_t index = buffer_indices[i];
			writes[index].pBufferInfo = &buffer_infos[i];
		}

		// Execute the update
		device.updateDescriptorSets(writes, nullptr);

		// Clear to indicate finished business
		image_infos.clear();
		buffer_infos.clear();
		image_indices.clear();
		buffer_indices.clear();
		writes.clear();
	}

	void offload(std::vector <vk::WriteDescriptorSet> &other) {
		other.insert(other.end(), writes.begin(), writes.end());
	}
};

inline LinkedDescriptorUpdater bind(const vk::Device &device,
		        	    const vk::DescriptorSet &dset,
				    const std::vector <vk::DescriptorSetLayoutBinding> &bindings)
{
	return { device, dset, bindings };
}

namespace shader {

// TODO: detail here as well...
using Defines = std::map<std::string, std::string>;
using Includes = std::set<std::string>;

// Local structs
struct compile_result {
	std::vector <unsigned int> spirv = {};
	std::string log = "";
	std::string source = "";
};

// Compiling shaders
inline EShLanguage translate_shader_stage(const vk::ShaderStageFlagBits &stage)
{
	switch (stage) {
	case vk::ShaderStageFlagBits::eVertex:
		return EShLangVertex;
	case vk::ShaderStageFlagBits::eTessellationControl:
		return EShLangTessControl;
	case vk::ShaderStageFlagBits::eTessellationEvaluation:
		return EShLangTessEvaluation;
	case vk::ShaderStageFlagBits::eGeometry:
		return EShLangGeometry;
	case vk::ShaderStageFlagBits::eFragment:
		return EShLangFragment;
	case vk::ShaderStageFlagBits::eCompute:
		return EShLangCompute;
	case vk::ShaderStageFlagBits::eTaskEXT:
		return EShLangTask;
	case vk::ShaderStageFlagBits::eMeshNV:
		return EShLangMesh;
	case vk::ShaderStageFlagBits::eRaygenNV:
		return EShLangRayGenNV;
	case vk::ShaderStageFlagBits::eAnyHitNV:
		return EShLangAnyHitNV;
	case vk::ShaderStageFlagBits::eClosestHitNV:
		return EShLangClosestHitNV;
	case vk::ShaderStageFlagBits::eMissNV:
		return EShLangMissNV;
	case vk::ShaderStageFlagBits::eIntersectionNV:
		return EShLangIntersectNV;
	case vk::ShaderStageFlagBits::eCallableNV:
		return EShLangCallableNV;
	default:
		break;
	}

	microlog::error("translate_shader_stage", "Unknown shader stage %s\n", vk::to_string(stage).c_str());

	return EShLangVertex;
}

inline compile_result glsl_to_spirv(const std::string &source,
				    const std::set <std::string> &paths,
				    const std::map <std::string, std::string> &defines,
				    const vk::ShaderStageFlagBits &shader_type)
{
	// Output
	compile_result out;

	// Compile shader
	EShLanguage stage = translate_shader_stage(shader_type);

	std::string preprocessed = source;

	// Cut the version string
	// TODO: only assumes its the very first string
	std::string version;
	for (size_t i = 0; i < preprocessed.size(); i++) {
		version += preprocessed[i];
		if (preprocessed[i] == '\n')
			break;
	}

	preprocessed = preprocessed.substr(version.size());
	for (auto &[symbol, value] : defines)
		preprocessed = "#define " + symbol + " " + value + "\n" + preprocessed;

	preprocessed = version + preprocessed;

	// TODO: use &
	const char *shaderStrings[1];
	shaderStrings[0] = preprocessed.data();

	glslang::SpvOptions options;
	options.generateDebugInfo = true;

	glslang::TShader shader(stage);

	shader.setEnvTarget(glslang::EShTargetLanguage::EShTargetSpv,
			    glslang::EShTargetLanguageVersion::EShTargetSpv_1_6);
	shader.setStrings(shaderStrings, 1);

	// Enable SPIR-V and Vulkan rules when parsing GLSL
	EShMessages messages = (EShMessages) (EShMsgDefault | EShMsgSpvRules
			| EShMsgVulkanRules | EShMsgDebugInfo);

	// Include directories
	standalone::DirectoryIncluder includer;
	for (const auto &path : paths)
		includer.include(path);

	// ShaderIncluder includer;
	if (!shader.parse(GetDefaultResources(), 450, false, messages, includer)) {
		out.log = shader.getInfoLog();
		out.source = preprocessed;
		return out;
	}

	// Link the program
	glslang::TProgram program;
	program.addShader(&shader);

	if (!program.link(messages)) {
		out.log = program.getInfoLog();
		return out;
	}

	glslang::GlslangToSpv(*program.getIntermediate(stage), out.spirv, &options);

	return out;
}

inline std::string fmt_lines(const std::string &str)
{
	// Add line numbers to each line
	std::string out = "";
	std::string line;

	std::istringstream stream(str);

	int line_num = 1;
	while (std::getline(stream, line)) {
		char buffer[1024];
		memset(buffer, 0, sizeof(buffer));
		snprintf(buffer, sizeof(buffer), "%4d: %s\n", line_num++, line.c_str());
		out += buffer;
	}

	return out;
}

// Return proxy specialization for shader modules
static void destroy_shader_module(const vk::Device &device,
				  const vk::ShaderModule &shader)
{
	device.destroyShaderModule(shader);
}

using ShaderModuleReturnProxy = DeviceReturnProxy <vk::ShaderModule, destroy_shader_module>;

// Compile shader
inline ShaderModuleReturnProxy
compile(const vk::Device &device,
	const std::string &source,
	const vk::ShaderStageFlagBits &shader_type,
	const Includes &includes = {},
	const Defines &defines = {})
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	compile_result out = glsl_to_spirv(source, includes, defines, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		microlog::error("shader",
				"Shader compilation failed:\n%s\nSource:\n%s",
				out.log.c_str(), fmt_lines(out.source).c_str());
		return true;
	}

	vk::ShaderModuleCreateInfo create_info;
	create_info.pCode = out.spirv.data();
	create_info.codeSize = out.spirv.size() * sizeof(uint32_t);

	return device.createShaderModule(create_info);
}

inline ShaderModuleReturnProxy
compile(const vk::Device &device,
	const std::filesystem::path &path,
	const vk::ShaderStageFlagBits &shader_type,
	const Includes &includes = {},
	const Defines &defines = {})
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	std::string source = standalone::readfile(path);
	compile_result out = glsl_to_spirv(source, includes, defines, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		microlog::error(__FUNCTION__,
				"Shader compilation failed:\n%s\nSource:\n%s",
				out.log.c_str(), fmt_lines(out.source).c_str());
		return true;
	}

	vk::ShaderModuleCreateInfo create_info;
	create_info.pCode = out.spirv.data();
	create_info.codeSize = out.spirv.size() * sizeof(uint32_t);

	return device.createShaderModule(create_info);
}

} // namespace shader

namespace pipeline {

inline void destroy_pipeline(const vk::Device &device, const vk::Pipeline &pipeline)
{
	device.destroyPipeline(pipeline);
}

using PipelineReturnProxy = DeviceReturnProxy <vk::Pipeline, destroy_pipeline>;

// Regular graphics pipeline
struct GraphicsCreateInfo {
	using vertex_binding_t = vk::VertexInputBindingDescription;
	using vertex_attribute_t = vk::ArrayProxy <vk::VertexInputAttributeDescription>;

	std::optional <vertex_binding_t> vertex_binding = std::nullopt;
	std::optional <vertex_attribute_t> vertex_attributes = std::nullopt;

	std::vector <vk::PipelineShaderStageCreateInfo> shader_stages;

	vk::Extent2D extent;

	vk::PolygonMode fill_mode = vk::PolygonMode::eFill;
	vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eBack;

	bool dynamic_viewport = false;
	bool alpha_blend = false;
	bool depth_test = true;
	bool depth_write = true;

	vk::PipelineLayout pipeline_layout;
	vk::RenderPass render_pass;
	uint32_t subpass;
};

inline PipelineReturnProxy compile(const vk::Device &device, const GraphicsCreateInfo &info)
{
	if (!info.shader_stages.size())
		microlog::error("pipeline::compile", "Empty shader stages\n");

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {
		{}, nullptr, nullptr
	};

	if (info.vertex_binding && info.vertex_attributes)
		vertex_input_info = { {}, *info.vertex_binding, *info.vertex_attributes };

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {
		{}, vk::PrimitiveTopology::eTriangleList
	};

	// Configuring the viewport
	vk::PipelineViewportStateCreateInfo viewport_state {
		{}, 1, nullptr, 1, nullptr
	};

	vk::PipelineDynamicStateCreateInfo dynamic_state {
		{}, 0, nullptr
	};

	// Fixed viewport
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) info.extent.width,
		(float) info.extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor { {}, info.extent };

	// Dynamic viewport
	std::array <vk::DynamicState, 2> dynamic_states {
		vk::DynamicState::eViewport,
		vk::DynamicState::eScissor
	};

	// Select according to the configuration
	if (info.dynamic_viewport) {
		dynamic_state = vk::PipelineDynamicStateCreateInfo {
			{},
			(uint32_t) dynamic_states.size(),
			dynamic_states.data()
		};
	} else {
		viewport_state = vk::PipelineViewportStateCreateInfo {
			{}, 1, &viewport, 1, &scissor
		};
	}

	vk::PipelineRasterizationStateCreateInfo rasterizer {
		{},
		false, false,
		info.fill_mode,
		info.cull_mode,
		vk::FrontFace::eClockwise,
		false,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	vk::PipelineMultisampleStateCreateInfo multisampling {
		{}, vk::SampleCountFlagBits::e1
	};

	vk::PipelineDepthStencilStateCreateInfo depth_stencil {
		{},
		info.depth_test,
		info.depth_write,
		vk::CompareOp::eLess,
		false, false,
		{}, {},
		0.0f, 1.0f
	};

	vk::PipelineColorBlendAttachmentState color_blend_attachment;
	if (info.alpha_blend) {
		color_blend_attachment = {
			true,
			vk::BlendFactor::eSrcAlpha,
			vk::BlendFactor::eOneMinusSrcAlpha,
			vk::BlendOp::eAdd,
			vk::BlendFactor::eOne,
			vk::BlendFactor::eZero,
			vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR
			| vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB
			| vk::ColorComponentFlagBits::eA
		};
	} else {
		color_blend_attachment = {
			false,
			vk::BlendFactor::eOne,
			vk::BlendFactor::eZero,
			vk::BlendOp::eAdd,
			vk::BlendFactor::eOne,
			vk::BlendFactor::eZero,
			vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR
			| vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB
			| vk::ColorComponentFlagBits::eA
		};
	}

	vk::PipelineColorBlendStateCreateInfo color_blending {
		{},
		false,
		vk::LogicOp::eCopy, 1,
		&color_blend_attachment,
		{ 0.0f, 0.0f, 0.0f, 0.0f }
	};

	return device.createGraphicsPipeline(nullptr,
		vk::GraphicsPipelineCreateInfo {
			{}, info.shader_stages,
			&vertex_input_info,
			&input_assembly,
			nullptr,
			&viewport_state,
			&rasterizer,
			&multisampling,
			&depth_stencil,
			&color_blending,
			&dynamic_state,
			info.pipeline_layout,
			info.render_pass,
			info.subpass
		}).value;
}

// Compute pipeline
struct ComputeCreateInfo {
	vk::PipelineShaderStageCreateInfo shader_stage;
	vk::PipelineLayout pipeline_layout;
};

inline PipelineReturnProxy compile(const vk::Device &device, const ComputeCreateInfo &info)
{
	return device.createComputePipeline(nullptr,
		vk::ComputePipelineCreateInfo {
			{}, info.shader_stage, info.pipeline_layout
		}).value;
}

} // namespace pipeline

// Pre defined Vulkan types; not intended for concrete usage
struct r32f {
	char _data[sizeof(float)];
};

struct rg32f {
	char _data[2 * sizeof(float)];
};

struct rgb32f {
	char _data[3 * sizeof(float)];
};

struct rgba32f {
	char _data[4 * sizeof(float)];
};

// Easier vertex layout, using templates only
template <typename T, typename... Args>
constexpr size_t sizeof_all()
{
	if constexpr (sizeof...(Args))
		return sizeof(T) + sizeof_all <Args...> ();
	else
		return sizeof(T);
}

template <typename T, bool instantiated = true>
struct type_translator {
	static_assert(!instantiated, "Unsupported format translation for this type");
	static constexpr vk::Format format = vk::Format::eUndefined;
};

template <uint32_t index, uint32_t offset, typename T>
constexpr vk::VertexInputAttributeDescription attribute_for()
{
	return vk::VertexInputAttributeDescription {
		index, 0,
		type_translator <T> ::format,
		offset
	};
}

template <uint32_t index, uint32_t offset, typename T, typename... Args>
constexpr std::array <vk::VertexInputAttributeDescription, 1 + sizeof...(Args)>
attributes_for()
{
	if constexpr (sizeof...(Args)) {
		auto previous = attributes_for <index + 1, offset + sizeof(T), Args...> ();
		std::array <vk::VertexInputAttributeDescription, 1 + sizeof...(Args)> out;
		out[0] = attribute_for<index, offset, T>();
		for (uint32_t i = 0; i < sizeof...(Args); i++)
			out[i + 1] = previous[i];
		return out;
	} else {
		return { attribute_for <index, offset, T> () };
	}
}

template <typename... Args>
struct VertexLayout {
	static constexpr size_t size = sizeof_all <Args...> ();

	static constexpr vk::VertexInputBindingDescription binding {
		0, size, vk::VertexInputRate::eVertex
	};

	static constexpr std::array <vk::VertexInputAttributeDescription, sizeof...(Args)> attributes {
		attributes_for <0, 0, Args...> ()
	};
};

// Group of shaders for a pipeline
struct ShaderStageBundle {
	vk::Device device;
	littlevk::Deallocator &dal;

	std::vector <vk::PipelineShaderStageCreateInfo> stages;

	ShaderStageBundle(const vk::Device &device, littlevk::Deallocator &dal)
		: device(device), dal(dal) {}

	// TODO: entry points
	ShaderStageBundle &source(const std::string &glsl,
				  vk::ShaderStageFlagBits flags,
				  const char *entry = "main",
				  const shader::Includes &includes = {},
				  const shader::Defines &defines = {}) {
		vk::ShaderModule module = littlevk::shader::compile(device, glsl, flags, includes, defines).unwrap(dal);
		stages.push_back({ {}, flags, module, entry });
		return *this;
	}

	ShaderStageBundle &file(const std::filesystem::path &path,
				vk::ShaderStageFlagBits flags,
				const char *entry = "main",
				const shader::Includes &includes = {},
				const shader::Defines &defines = {}) {
		std::filesystem::path parent = path.parent_path();
		std::string glsl = standalone::readfile(path);

		auto copy_includes = includes;
		copy_includes.insert(parent.string());
		vk::ShaderModule module = littlevk::shader::compile(device, glsl, flags, copy_includes, defines).unwrap(dal);
		stages.push_back({ {}, flags, module, entry });
		return *this;
	}
};

// General purpose pipeline type
struct Pipeline {
	vk::Pipeline handle;
	vk::PipelineLayout layout;
	std::optional <vk::DescriptorSetLayout> dsl;
	std::map <uint32_t, vk::DescriptorSetLayoutBinding> bindings;
};

// General purpose pipeline compiler
enum class PipelineType {
	eGraphics,
	eRayTracing,
	eCompute,
};

template <typename Up>
struct PipelineAssemblerBase {
	// Essential
	const vk::Device &device;
	littlevk::Deallocator &dal;

	// Shader information
	std::optional <std::reference_wrapper <const ShaderStageBundle>> bundle;

	// Pipeline layout information
	std::vector <vk::DescriptorSetLayoutBinding> dsl_bindings;
	std::vector <vk::PushConstantRange> push_constants;

	PipelineAssemblerBase(const vk::Device &device_, littlevk::Deallocator &dal_)
			: device(device_), dal(dal_)  {}

	Up &with_shader_bundle(const ShaderStageBundle &sb) {
		bundle = sb;
		return static_cast <Up &> (*this);
	}
	
	Up &with_dsl_binding(uint32_t binding, vk::DescriptorType type,
			     uint32_t count, vk::ShaderStageFlagBits stage) {
		dsl_bindings.emplace_back(binding, type, count, stage);
		return static_cast <Up &> (*this);
	}

	template <size_t N>
	Up &with_dsl_bindings(const std::array<vk::DescriptorSetLayoutBinding, N> &bindings) {
		for (const auto &binding : bindings)
			dsl_bindings.push_back(binding);
		return static_cast <Up &> (*this);
	}
	
	Up &with_dsl_bindings(const std::vector <vk::DescriptorSetLayoutBinding> &bindings) {
		for (const auto &binding : bindings)
			dsl_bindings.push_back(binding);
		return static_cast <Up &> (*this);
	}
	
	template <typename T>
	Up &with_push_constant(vk::ShaderStageFlags stage, uint32_t offset = 0) {
		push_constants.push_back({ stage, offset, sizeof(T) });
		return static_cast <Up &> (*this);
	}
};

template <PipelineType T>
struct PipelineAssembler {};

template <>
struct PipelineAssembler <PipelineType::eGraphics> : PipelineAssemblerBase <PipelineAssembler <PipelineType::eGraphics>> {
	using base = PipelineAssemblerBase <PipelineAssembler <PipelineType::eGraphics>>;

	const littlevk::Window &window;

	vk::RenderPass render_pass;
	uint32_t subpass;

	std::optional <vk::VertexInputBindingDescription> vertex_binding;
	std::vector <vk::VertexInputAttributeDescription> vertex_attributes;
	vk::PolygonMode fill;
	vk::CullModeFlags culling;

	bool depth_test;
	bool depth_write;
	bool alpha_blend;

	PipelineAssembler(const vk::Device &device_,
			  const littlevk::Window &window_,
			  littlevk::Deallocator &dal_)
			  : base(device_, dal_),
			  window(window_),
			  subpass(0),
			  fill(vk::PolygonMode::eFill),
			  culling(vk::CullModeFlagBits::eBack),
			  depth_test(true),
			  depth_write(true),
			  alpha_blend(true) {}

	PipelineAssembler &with_render_pass(const vk::RenderPass &render_pass_, uint32_t subpass_) {
		render_pass = render_pass_;
		subpass = subpass_;
		return *this;
	}

	template <typename ... Args>
	PipelineAssembler &with_vertex_layout(const VertexLayout <Args...> &) {
		using layout = VertexLayout <Args...>;
		vertex_binding = layout::binding;
		vertex_attributes = std::vector <vk::VertexInputAttributeDescription>
			(layout::attributes.begin(), layout::attributes.end());
		return *this;
	}

	PipelineAssembler &with_vertex_binding(const vk::VertexInputBindingDescription &binding) {
		vertex_binding = binding;
		return *this;
	}

	PipelineAssembler &with_vertex_attributes(const std::vector <vk::VertexInputAttributeDescription> &attributes) {
		vertex_attributes = attributes;
		return *this;
	}

	PipelineAssembler &alpha_blending(bool blend) {
		alpha_blend = blend;
		return *this;
	}

	PipelineAssembler &polygon_mode(vk::PolygonMode pmode) {
		fill = pmode;
		return *this;
	}

	PipelineAssembler &cull_mode(vk::CullModeFlags cmode) {
		culling = cmode;
		return *this;
	}

	PipelineAssembler &depth_stencil(bool test, bool write) {
		depth_test = test;
		depth_write = write;
		return *this;
	}

	Pipeline compile() const {
		Pipeline pipeline;

		std::vector <vk::DescriptorSetLayout> dsls;
		if (dsl_bindings.size()) {
			vk::DescriptorSetLayout dsl = descriptor_set_layout(device,
				vk::DescriptorSetLayoutCreateInfo {
					{}, dsl_bindings
				}).unwrap(dal);

			dsls.push_back(dsl);
			pipeline.dsl = dsl;
		}

		pipeline.layout = littlevk::pipeline_layout(device,
			vk::PipelineLayoutCreateInfo {
				{}, dsls, push_constants
			}).unwrap(dal);

		pipeline::GraphicsCreateInfo pipeline_info;

		pipeline_info.shader_stages = bundle.value().get().stages;
		pipeline_info.vertex_binding = vertex_binding;
		pipeline_info.vertex_attributes = vertex_attributes;
		pipeline_info.extent = window.extent;
		pipeline_info.pipeline_layout = pipeline.layout;
		pipeline_info.render_pass = render_pass;
		pipeline_info.subpass = subpass;
		pipeline_info.fill_mode = fill;
		pipeline_info.cull_mode = culling;
		pipeline_info.dynamic_viewport = true;
		pipeline_info.alpha_blend = alpha_blend;
		pipeline_info.depth_test = depth_test;
		pipeline_info.depth_write = depth_write;

		pipeline.handle = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);

		// Build the binding map
		for (auto &dslb : dsl_bindings)
			pipeline.bindings[dslb.binding] = dslb;

		return pipeline;
	}

	operator Pipeline() const {
		return compile();
	}
};

template <>
struct PipelineAssembler <PipelineType::eCompute> : PipelineAssemblerBase <PipelineAssembler <PipelineType::eCompute>> {
	using base = PipelineAssemblerBase <PipelineAssembler <PipelineType::eCompute>>;

	PipelineAssembler(const vk::Device &device_, littlevk::Deallocator &dal_)
			  : base(device_, dal_) {}
	
	Pipeline compile() const {
		Pipeline pipeline;

		std::vector <vk::DescriptorSetLayout> dsls;
		if (dsl_bindings.size()) {
			vk::DescriptorSetLayout dsl = descriptor_set_layout(device,
				vk::DescriptorSetLayoutCreateInfo {
					{}, dsl_bindings
				}).unwrap(dal);

			dsls.push_back(dsl);
			pipeline.dsl = dsl;
		}

		pipeline.layout = littlevk::pipeline_layout(device,
			vk::PipelineLayoutCreateInfo {
				{}, dsls, push_constants
			}).unwrap(dal);

		pipeline::ComputeCreateInfo pipeline_info;

		pipeline_info.shader_stage = bundle.value().get().stages.front();
		pipeline_info.pipeline_layout = pipeline.layout;

		pipeline.handle =
			littlevk::pipeline::compile(device, pipeline_info)
				.unwrap(dal);

		return pipeline;
	}

	operator Pipeline() const {
		return compile();
	}
};

} // namespace littlevk

// Specializing formats
template <>
struct littlevk::type_translator <littlevk::rg32f, true> {
	static constexpr vk::Format format = vk::Format::eR32G32Sfloat;
};

template <>
struct littlevk::type_translator <littlevk::rgb32f, true> {
	static constexpr vk::Format format = vk::Format::eR32G32B32Sfloat;
};

template <>
struct littlevk::type_translator <littlevk::rgba32f, true> {
	static constexpr vk::Format format = vk::Format::eR32G32B32A32Sfloat;
};

// Specializing for GLM types if defined
#ifdef LITTLEVK_GLM_TRANSLATOR

template <>
struct littlevk::type_translator <glm::vec2, true> {
	static constexpr vk::Format format = vk::Format::eR32G32Sfloat;
};

template <>
struct littlevk::type_translator <glm::vec3, true> {
	static constexpr vk::Format format = vk::Format::eR32G32B32Sfloat;
};

template <>
struct littlevk::type_translator <glm::vec4, true> {
	static constexpr vk::Format format = vk::Format::eR32G32B32A32Sfloat;
};

#endif

// Extension wrappers
#define PFN_SETUP(name, ...)										\
	using PFN = PFN_##name;										\
	static PFN handle = 0;										\
	if (!handle) {											\
		handle = (PFN) vkGetInstanceProcAddr(littlevk::detail::get_vulkan_instance(), #name);	\
		microlog::assertion(handle, #name, "invalid PFN: %p", (void *) handle);			\
	}												\
	return handle(__VA_ARGS__)

VKAPI_ATTR
VKAPI_CALL
inline VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance,
					       const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
					       const VkAllocationCallbacks *pAllocator,
					       VkDebugUtilsMessengerEXT *pMessenger)
{
	PFN_SETUP(vkCreateDebugUtilsMessengerEXT,
		instance,
		pCreateInfo,
		pAllocator,
		pMessenger);
}

VKAPI_ATTR
VKAPI_CALL
inline void vkDestroyDebugUtilsMessengerEXT(VkInstance instance,
					    VkDebugUtilsMessengerEXT messenger,
					    const VkAllocationCallbacks *pAllocator)
{
	PFN_SETUP(vkDestroyDebugUtilsMessengerEXT,
		instance,
		messenger,
		pAllocator);
}

VKAPI_ATTR
VKAPI_CALL
inline void vkGetAccelerationStructureBuildSizesKHR(VkDevice device,
						    VkAccelerationStructureBuildTypeKHR buildType,
						    const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
						    const uint32_t* pMaxPrimitiveCounts,
						    VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo)
{
	PFN_SETUP(vkGetAccelerationStructureBuildSizesKHR,
		device,
		buildType,
		pBuildInfo,
		pMaxPrimitiveCounts,
		pSizeInfo);
}

VKAPI_ATTR
VKAPI_CALL
inline VkResult vkCreateAccelerationStructureKHR(VkDevice device,
						 const VkAccelerationStructureCreateInfoKHR *pCreateInfo,
						 const VkAllocationCallbacks *pAllocator,
						 VkAccelerationStructureKHR *pAccelerationStructure)
{
	PFN_SETUP(vkCreateAccelerationStructureKHR,
		device,
		pCreateInfo,
		pAllocator,
		pAccelerationStructure);
}

VKAPI_ATTR
VKAPI_CALL
inline void vkCmdBuildAccelerationStructuresKHR(VkCommandBuffer commandBuffer,
						uint32_t infoCount,
						const VkAccelerationStructureBuildGeometryInfoKHR *pInfos,
						const VkAccelerationStructureBuildRangeInfoKHR *const *ppBuildRangeInfos)
{
	PFN_SETUP(vkCmdBuildAccelerationStructuresKHR,
		commandBuffer,
		infoCount,
		pInfos,
		ppBuildRangeInfos);
}

VKAPI_ATTR
VKAPI_CALL
inline VkDeviceAddress vkGetAccelerationStructureDeviceAddressKHR(VkDevice device,
								  const VkAccelerationStructureDeviceAddressInfoKHR *pInfo)
{
	PFN_SETUP(vkGetAccelerationStructureDeviceAddressKHR,
		device,
		pInfo);
}

VKAPI_ATTR
VKAPI_CALL
inline VkResult vkGetRayTracingShaderGroupHandlesKHR(VkDevice device,
						     VkPipeline pipeline,
						     uint32_t firstGroup,
						     uint32_t groupCount,
						     size_t dataSize,
						     void *pData)
{
	PFN_SETUP(vkGetRayTracingShaderGroupHandlesKHR,
		device,
		pipeline,
		firstGroup,
		groupCount,
		dataSize,
		pData);
}

VKAPI_ATTR
VKAPI_CALL
inline VkResult vkCreateRayTracingPipelinesKHR(VkDevice device,
					       VkDeferredOperationKHR deferredOperation,
					       VkPipelineCache pipelineCache,
					       uint32_t createInfoCount,
					       const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
					       const VkAllocationCallbacks *pAllocator,
					       VkPipeline *pPipelines)
{
	PFN_SETUP(vkCreateRayTracingPipelinesKHR,
		device,
		deferredOperation,
		pipelineCache,
		createInfoCount,
		pCreateInfos,
		pAllocator,
		pPipelines);
}

VKAPI_ATTR
VKAPI_CALL
inline void vkCmdTraceRaysKHR(VkCommandBuffer commandBuffer,
			      const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
			      const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
			      const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
			      const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
			      uint32_t width,
			      uint32_t height,
			      uint32_t depth)
{
	PFN_SETUP(vkCmdTraceRaysKHR,
		commandBuffer,
		pRaygenShaderBindingTable,
		pMissShaderBindingTable,
		pHitShaderBindingTable,
		pCallableShaderBindingTable,
		width,
		height,
		depth);
}

VKAPI_ATTR
VKAPI_CALL
inline VkResult vkSetDebugUtilsObjectNameEXT(VkDevice device,
					     const VkDebugUtilsObjectNameInfoEXT *pNameInfo)
{
	PFN_SETUP(vkSetDebugUtilsObjectNameEXT,
		device,
		pNameInfo);
}

VKAPI_ATTR
VKAPI_CALL
inline void vkCmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer,
				  uint32_t groupCountX,
				  uint32_t groupCountY,
				  uint32_t groupCountZ)
{
	PFN_SETUP(vkCmdDrawMeshTasksEXT,
		commandBuffer,
		groupCountX,
		groupCountY,
		groupCountZ);
}