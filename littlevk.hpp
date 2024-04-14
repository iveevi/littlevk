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
#include <regex>
#include <set>
#include <variant>

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
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

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
	printf("%s[littlevk::error]%s (%s) ",
		colors::error, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void warning(const char *header, const char *format, ...)
{
	printf("%s[littlevk::warning]%s (%s) ",
		colors::warning, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void info(const char *header, const char *format, ...)
{
	printf("%s[littlevk::info]%s (%s) ",
		colors::info, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void assertion(bool cond, const char *header, const char *format, ...)
{
	if (!cond) {
		printf("%s[littlevk::assert]%s (%s) ",
			colors::error, colors::reset, header);

		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
}

}

// Loading Vulkan extensions
struct Extensions {
	// TODO: template to make easier?
	static auto &vkCreateDebugUtilsMessengerEXT() {
		static PFN_vkCreateDebugUtilsMessengerEXT handle = 0;
		return handle;
	}

	static auto &vkDestroyDebugUtilsMessengerEXT() {
		static PFN_vkDestroyDebugUtilsMessengerEXT handle = 0;
		return handle;
	}

	static auto &vkCmdDrawMeshTasksEXT() {
		static PFN_vkCmdDrawMeshTasksEXT handle = 0;
		return handle;
	}

	static auto &vkCmdDrawMeshTasksNV() {
		static PFN_vkCmdDrawMeshTasksNV handle = 0;
		return handle;
	}

	static auto &vkGetMemoryFdKHR() {
		static PFN_vkGetMemoryFdKHR handle = 0;
		return handle;
	}
};

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
	std::vector <std::string> directories;
	// std::set <std::string> includedFiles;

	DirectoryIncluder() = default;

	IncludeResult *includeLocal(const char *header, const char *includer, size_t depth) override {
		return readLocalPath(header, includer, (int) depth);
	}

	IncludeResult *includeSystem(const char *header, const char *includer, size_t depth) override {
		return nullptr;
	}

	void releaseInclude(IncludeResult *result) override {
		if (result != nullptr) {
			delete[] static_cast <char *> (result->userData);
			delete result;
		}
	}

	void include(const std::string &dir) {
		directories.push_back(dir);
	}

	IncludeResult *readLocalPath(const char *header, const char *includer, int depth) {
		for (auto it = directories.rbegin(); it != directories.rend(); it++) {
			std::string path = *it + '/' + header;
			std::replace(path.begin(), path.end(), '\\', '/');
			std::ifstream file(path, std::ios_base::binary | std::ios_base::ate);
			if (file) {
				int length = file.tellg();
				char *content = new char[length];
				file.seekg(0, file.beg);
				file.read(content, length);
				return new IncludeResult(path, content, length, content);
			}
		}

		return nullptr;
	}
};

}

namespace littlevk {

namespace detail {

// Configuration parameters (free to user modification)
struct Config {
	std::vector <const char *> instance_extensions {};
	bool enable_validation_layers = true;
	bool abort_on_validation_error = true;
	bool enable_logging = true;
};

} // namespace detail

// Singleton config
inline detail::Config *config()
{
	static detail::Config config;
	return &config;
}

// Automatic deallocation system
using DeallocationQueue = std::queue <std::function <void (vk::Device)>>;

struct Deallocator {
	vk::Device device;
	DeallocationQueue device_deallocators;

	Deallocator(vk::Device device) : device(device) {}
	~Deallocator() {
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

	T unwrap(Deallocator *deallocator) {
		if (this->failed)
			return {};

		T value = this->value;
		deallocator->device_deallocators.push(
			[value](vk::Device device) {
				destructor(device, value);
			}
		);

		return this->value;
	}

	T defer(DeallocationQueue &queue) {
		if (this->failed)
			return {};

		T value = this->value;
		queue.push(
			[value](vk::Device device) {
				destructor(device, value);
			}
		);

		return this->value;
	}
};

// Return proxy helper for structures composed of multiple device objects
template <typename T>
struct ComposedReturnProxy {
	T value;
	bool failed;
	DeallocationQueue queue;

	ComposedReturnProxy(T value, DeallocationQueue queue)
			: value(value), failed(false), queue(queue) {}
	ComposedReturnProxy(bool failed) : failed(failed) {}

	T unwrap(Deallocator *deallocator) {
		if (this->failed)
			return {};

		while (!this->queue.empty()) {
			deallocator->device_deallocators.push(this->queue.front());
			this->queue.pop();
		}

		return this->value;
	}

	T defer(DeallocationQueue &queue) {
		if (this->failed)
			return {};

		while (!this->queue.empty()) {
			queue.push(this->queue.front());
			this->queue.pop();
		}

		return this->value;
	}
};

namespace validation {

// Create debug messenger
static bool check_validation_layer_support(const std::vector <const char *> &validation_layers)
{
	uint32_t layer_count;
	vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

	std::vector <VkLayerProperties> available_layers(layer_count);

	vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
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

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_logger
(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
	void *pUserData
)
{
	// Errors
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		microlog::error("validation", "%s\n", pCallbackData->pMessage);
		if (config()->abort_on_validation_error)
			abort();
	} else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		microlog::warning("validation", "%s\n", pCallbackData->pMessage);
	} else {
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
		// glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
		initialized = true;
	}
}

// Get (or generate) the required extensions
inline const std::vector <const char *> &get_required_extensions()
{
	// Vector to return
	static std::vector <const char *> extensions;

	// Add if empty
	if (extensions.empty()) {
		// Add glfw extensions
		uint32_t glfw_extension_count;
		const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
		extensions.insert(extensions.end(), glfw_extensions, glfw_extensions + glfw_extension_count);

		// Additional extensions
		extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		// TODO: add config extensions

		if (config()->enable_validation_layers) {
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
				microlog::error("fatal", "debug messenger singleton destroyed without valid instance singleton\n");
				return;
			}

			global_instance.instance.destroyDebugUtilsMessengerEXT(messenger);
		}
	}
} global_messenger;

// Get (or create) the singleton instance
inline const vk::Instance &get_vulkan_instance()
{
	// TODO: from config...
	static vk::ApplicationInfo app_info {
		"LittleVk",
		VK_MAKE_VERSION(1, 0, 0),
		"LittelVk",
		VK_MAKE_VERSION(1, 0, 0),
		VK_API_VERSION_1_3
	};

	// Skip if already initialized
	if (global_instance.initialized)
		return global_instance.instance;

	// Make sure GLFW is initialized
	initialize_glfw();

	static const std::vector <const char *> validation_layers = {
		"VK_LAYER_KHRONOS_validation"
	};

	static vk::InstanceCreateInfo instance_info {
		vk::InstanceCreateFlags(),
		&app_info,
		0, nullptr,
		(uint32_t) get_required_extensions().size(),
		get_required_extensions().data()
	};

	if (config()->enable_validation_layers) {
		// Check if validation layers are available
		if (!validation::check_validation_layer_support(validation_layers)) {
			microlog::error("instance initialization", "Validation layers are not available!\n");
			config()->enable_validation_layers = false;
		}

		if (config()->enable_validation_layers) {
			instance_info.enabledLayerCount = (uint32_t) validation_layers.size();
			instance_info.ppEnabledLayerNames = validation_layers.data();
		}
	}

	global_instance.instance = vk::createInstance(instance_info);

	// Post initialization; load extensions
	Extensions::vkCreateDebugUtilsMessengerEXT() = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(global_instance.instance, "vkCreateDebugUtilsMessengerEXT");
	microlog::assertion(Extensions::vkCreateDebugUtilsMessengerEXT(), "vkCreateDebugUtilsMessengerEXT", "Null function address\n");

	Extensions::vkDestroyDebugUtilsMessengerEXT() = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(global_instance.instance, "vkDestroyDebugUtilsMessengerEXT");
	microlog::assertion(Extensions::vkDestroyDebugUtilsMessengerEXT(), "vkDestroyDebugUtilsMessengerEXT", "Null function address\n");

	Extensions::vkCmdDrawMeshTasksEXT() = (PFN_vkCmdDrawMeshTasksEXT) vkGetInstanceProcAddr(global_instance.instance, "vkCmdDrawMeshTasksEXT");
	microlog::assertion(Extensions::vkCmdDrawMeshTasksEXT(), "vkCmdDrawMeshTasksEXT", "Null function address\n");

	Extensions::vkCmdDrawMeshTasksNV() = (PFN_vkCmdDrawMeshTasksNV) vkGetInstanceProcAddr(global_instance.instance, "vkCmdDrawMeshTasksNV");
	microlog::assertion(Extensions::vkCmdDrawMeshTasksNV(), "vkCmdDrawMeshTasksNV", "Null function address\n");

	Extensions::vkGetMemoryFdKHR() = (PFN_vkGetMemoryFdKHR) vkGetInstanceProcAddr(global_instance.instance, "vkGetMemoryFdKHR");
	microlog::assertion(Extensions::vkGetMemoryFdKHR(), "vkGetMemoryFdKHR", "Null function address\n");

	// Ensure these are loaded properly
	microlog::assertion(Extensions::vkCreateDebugUtilsMessengerEXT(), "get_vulkan_instance", "Failed to load extension function: vkCreateDebugUtilsMessengerEXT\n");

	// Log function addresses
	microlog::info("get_vulkan_instance", "Loaded address %p for vkCreateDebugUtilsMessengerEXT\n", Extensions::vkCreateDebugUtilsMessengerEXT());
	microlog::info("get_vulkan_instance", "Loaded address %p for vkDestroyDebugUtilsMessengerEXT\n", Extensions::vkDestroyDebugUtilsMessengerEXT());
	microlog::info("get_vulkan_instance", "Loaded address %p for vkGetMemoryFdKHR\n", Extensions::vkGetMemoryFdKHR());

	// Loading the debug messenger
	if (config()->enable_validation_layers) {
		// Create debug messenger
		static constexpr vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_info {
			vk::DebugUtilsMessengerCreateFlagsEXT(),
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eError
				| vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
				| vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
				| vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo,
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
				| vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
				| vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,
			validation::debug_logger
		};

		global_messenger.messenger = global_instance.instance.createDebugUtilsMessengerEXT(debug_messenger_info);
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
		global_instance.instance.destroyDebugUtilsMessengerEXT(global_messenger.messenger);
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
	GLFWwindow	*handle = nullptr;
	std::string	title;
	vk::Extent2D	extent;
};

// TODO: littlevk logging using termcolor

// Creating windows and surfaces
inline Window *make_window(const vk::Extent2D &extent, const std::string &title)
{
        // Make sure GLFW is initialized
	detail::initialize_glfw();

        // Create the window
        GLFWwindow *handle = glfwCreateWindow
	(
                extent.width, extent.height,
                title.c_str(),
                nullptr, nullptr
        );

        // Check the actual size of the window
        glfwGetFramebufferSize(handle, (int *) &extent.width, (int *) &extent.height);
        // KOBRA_ASSERT(handle != nullptr, "Failed to create window");
        return new Window { handle, title, extent };
}

inline void destroy_window(Window *window)
{
        if (window == nullptr)
                return;

        if (window->handle != nullptr)
                glfwDestroyWindow(window->handle);

        delete window;
}

inline vk::SurfaceKHR make_surface(const Window &window)
{
	// Create the surface
	VkSurfaceKHR surface;
	VkResult result = glfwCreateWindowSurface(
		detail::get_vulkan_instance(),
		window.handle, nullptr, &surface
	);

	microlog::assertion(result == VK_SUCCESS, __FUNCTION__, "Failed to create a surface\n");

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
	std::vector <vk::QueueFamilyProperties> queue_families = phdev.getQueueFamilyProperties();

	// Find the first one that supports graphics
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics)
			return i;
	}

	// If none found, throw an error
	throw std::runtime_error("[Vulkan] No graphics queue family found");
}

// Find present queue family
inline uint32_t find_present_queue_family(const vk::PhysicalDevice &phdev, const vk::SurfaceKHR &surface)
{
	// Get the queue families
	std::vector <vk::QueueFamilyProperties> queue_families = phdev.getQueueFamilyProperties();

	// Find the first one that supports presentation
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (phdev.getSurfaceSupportKHR(i, surface))
			return i;
	}

	// If none found, throw an error
	// KOBRA_LOG_FUNC(Log::ERROR) << "No presentation queue family found\n";
	throw std::runtime_error("[Vulkan] No presentation queue family found");
}

// Get both graphics and present queue families
inline QueueFamilyIndices find_queue_families(const vk::PhysicalDevice &phdev, const vk::SurfaceKHR &surface)
{
	return {
		find_graphics_queue_family(phdev),
		find_present_queue_family(phdev, surface)
	};
}

// Swapchain structure
struct Swapchain {
	vk::Format format;
	vk::SwapchainKHR swapchain;
	std::vector <vk::Image> images;
	std::vector <vk::ImageView> image_views;
	vk::SwapchainCreateInfoKHR info;

	vk::SwapchainKHR operator*() const {
		return swapchain;
	}
};

// Pick a surface format
inline vk::SurfaceFormatKHR pick_surface_format(const vk::PhysicalDevice &phdev, const vk::SurfaceKHR &surface)
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
					return format.format == target.format &&
						format.colorSpace == target.colorSpace;
				}) != target_formats.end()) {
			return format;
		}
	}

	// If none found, throw an error
	// KOBRA_LOG_FUNC(Log::ERROR) << "No supported surface format found\n";
	throw std::runtime_error("[Vulkan] No supported surface format found");
}

// Pick a present mode
inline vk::PresentModeKHR pick_present_mode(const vk::PhysicalDevice &phdev, const vk::SurfaceKHR &surface)
{
	// Constant modes
	static const std::vector <vk::PresentModeKHR> target_modes = {
		vk::PresentModeKHR::eMailbox,
		vk::PresentModeKHR::eImmediate,
		vk::PresentModeKHR::eFifo
	};

	// Get the present modes
	std::vector <vk::PresentModeKHR> modes = phdev.getSurfacePresentModesKHR(surface);

	// Prioritize mailbox mode
	if (std::find(modes.begin(), modes.end(), vk::PresentModeKHR::eMailbox) !=
			modes.end()) {
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
inline Swapchain swapchain
(
 	const vk::PhysicalDevice &phdev,
	const vk::Device &device,
	const vk::SurfaceKHR &surface,
	const vk::Extent2D &extent,
	const QueueFamilyIndices &indices,
	const std::optional <vk::PresentModeKHR> &priority_mode = std::nullopt,
	const vk::SwapchainKHR *old_swapchain = nullptr
)
{
	Swapchain swapchain;

        // Pick a surface format
        auto surface_format = pick_surface_format(phdev, surface);
        swapchain.format = surface_format.format;
	microlog::info("vulkan", "Picked format %s for swapchain\n", vk::to_string(swapchain.format).c_str());

        // Surface capabilities and extent
        vk::SurfaceCapabilitiesKHR capabilities = phdev.getSurfaceCapabilitiesKHR(surface);

        // Set the surface extent
        vk::Extent2D swapchain_extent = extent;
        if (capabilities.currentExtent.width == std::numeric_limits <uint32_t> ::max()) {
                swapchain_extent.width = std::clamp(
                        swapchain_extent.width,
                        capabilities.minImageExtent.width,
                        capabilities.maxImageExtent.width
                );

                swapchain_extent.height = std::clamp(
                        swapchain_extent.height,
                        capabilities.minImageExtent.height,
                        capabilities.maxImageExtent.height
                );
        } else {
                swapchain_extent = capabilities.currentExtent;
        }

        // Transform, etc
        vk::SurfaceTransformFlagBitsKHR transform =
                (capabilities.supportedTransforms &
                vk::SurfaceTransformFlagBitsKHR::eIdentity) ?
                vk::SurfaceTransformFlagBitsKHR::eIdentity :
                capabilities.currentTransform;

        // Composite alpha
        vk::CompositeAlphaFlagBitsKHR composite_alpha =
                (capabilities.supportedCompositeAlpha &
                vk::CompositeAlphaFlagBitsKHR::eOpaque) ?
                vk::CompositeAlphaFlagBitsKHR::eOpaque :
                vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;

        // Present mode
        vk::PresentModeKHR present_mode = priority_mode ? *priority_mode : pick_present_mode(phdev, surface);
	microlog::info("vulkan", "Picked present mode %s for swapchain\n", vk::to_string(present_mode).c_str());

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
                (old_swapchain ? *old_swapchain : nullptr)
        };

        // In case graphics and present queues are different
        if (indices.graphics != indices.present) {
                swapchain.info.imageSharingMode = vk::SharingMode::eConcurrent;
                swapchain.info.queueFamilyIndexCount = 2;
                swapchain.info.pQueueFamilyIndices = &indices.graphics;
        }

        // Create the swapchain
        swapchain.swapchain = device.createSwapchainKHR(swapchain.info);

        // Get the swapchain images
	swapchain.images = device.getSwapchainImagesKHR(swapchain.swapchain);

        // Create image views
        vk::ImageViewCreateInfo create_view_info {
                {}, {},
                vk::ImageViewType::e2D,
                swapchain.format,
                {},
                vk::ImageSubresourceRange(
                        vk::ImageAspectFlagBits::eColor,
                        0, 1, 0, 1
                )
        };

        for (size_t i = 0; i < swapchain.images.size(); i++) {
                create_view_info.image = swapchain.images[i];
                swapchain.image_views.emplace_back(device.createImageView(create_view_info));
        }

	return swapchain;
}

inline void resize
(
 	const vk::Device &device,
	Swapchain &swapchain,
	const vk::Extent2D &extent
)
{
	// First free the old swapchain resources
	for (const vk::ImageView &view : swapchain.image_views)
		device.destroyImageView(view);

	device.destroySwapchainKHR(swapchain.swapchain);

	// We simply need to modify the swapchain info and rebuild it
	swapchain.info.imageExtent = extent;
	swapchain.swapchain = device.createSwapchainKHR(swapchain.info);
	swapchain.images = device.getSwapchainImagesKHR(swapchain.swapchain);

	// Recreate image views
	vk::ImageViewCreateInfo create_view_info {
		{}, {},
		vk::ImageViewType::e2D,
		swapchain.format,
		{},
		vk::ImageSubresourceRange(
			vk::ImageAspectFlagBits::eColor,
			0, 1, 0, 1
		)
	};

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
	device.destroySwapchainKHR(swapchain.swapchain);
}

// Return proxy for framebuffer(s)
static void destroy_framebuffer(const vk::Device &device, const vk::Framebuffer &framebuffer)
{
	device.destroyFramebuffer(framebuffer);
}

static void destroy_framebuffer_set(const vk::Device &device, const std::vector <vk::Framebuffer> &framebuffers)
{
	for (const vk::Framebuffer &fb : framebuffers)
		device.destroyFramebuffer(fb);
}

using FramebufferReturnProxy = DeviceReturnProxy <vk::Framebuffer, destroy_framebuffer>;

// TODO: template render pass assembler to check with the attachments?
// Framebuffer generator wrapper
struct FramebufferGenerator {
	const vk::Device &device;
	const vk::RenderPass &render_pass;
	vk::Extent2D extent;
	Deallocator *dal;

	std::vector <vk::Framebuffer> framebuffers;

	FramebufferGenerator(const vk::Device &device_, const vk::RenderPass &render_pass_,
		const vk::Extent2D &extent_, Deallocator *dal_)
			: device(device_), render_pass(render_pass_), extent(extent_), dal(dal_) {}

	template <typename ... Args>
	requires (std::is_trivially_constructible_v <vk::ImageView, Args> && ...)
	void add(const Args & ... args) {
		std::array <vk::ImageView, sizeof...(Args)> views { args... };

		vk::FramebufferCreateInfo info {
			{}, render_pass, views,
			extent.width, extent.height, 1
		};

		FramebufferReturnProxy ret = device.createFramebuffer(info);
		framebuffers.push_back(ret.unwrap(dal));
	}

	std::vector <vk::Framebuffer> unpack() {
		auto ret = framebuffers;
		framebuffers.clear();
		return ret;
	}
};

// Vulkan description/create info wrappers
struct AttachmentDescription {
	vk::Format              m_format;
	vk::SampleCountFlagBits m_samples;
	vk::AttachmentLoadOp    m_load_op;
	vk::AttachmentStoreOp   m_store_op;
	vk::AttachmentLoadOp    m_stencil_load_op;
	vk::AttachmentStoreOp   m_stencil_store_op;
	vk::ImageLayout         m_initial_layout;
	vk::ImageLayout         m_final_layout;

	constexpr operator vk::AttachmentDescription() const {
		return vk::AttachmentDescription(
			{},
			m_format,
			m_samples,
			m_load_op,
			m_store_op,
			m_stencil_load_op,
			m_stencil_store_op,
			m_initial_layout,
			m_final_layout
		);
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
static void destroy_render_pass(const vk::Device &device, const vk::RenderPass &render_pass)
{
	device.destroyRenderPass(render_pass);
}

using RenderPassReturnProxy = DeviceReturnProxy <vk::RenderPass, destroy_render_pass>;

inline RenderPassReturnProxy render_pass(const vk::Device &device, const vk::RenderPassCreateInfo &info)
{
	vk::RenderPass render_pass;
	if (device.createRenderPass(&info, nullptr, &render_pass) != vk::Result::eSuccess)
		return  true;

	return std::move(render_pass);
}

inline RenderPassReturnProxy render_pass
(
	const vk::Device &device,
	const vk::ArrayProxyNoTemporaries <vk::AttachmentDescription> &descriptions,
	const vk::ArrayProxyNoTemporaries <vk::SubpassDescription> &subpasses,
	const vk::ArrayProxyNoTemporaries <vk::SubpassDependency> &dependencies
)
{
	vk::RenderPass render_pass;
	vk::RenderPassCreateInfo info {
		{},
		descriptions, subpasses, dependencies
	};

	if (device.createRenderPass(&info, nullptr, &render_pass) != vk::Result::eSuccess)
		return  true;

	return std::move(render_pass);
}

// Render pass assembly
struct RenderPassAssembler {
	const vk::Device &device;
	// TODO: alternative to pointer?
	littlevk::Deallocator *dal;

	std::vector <vk::SubpassDescription> subpasses;
	std::vector <vk::AttachmentDescription> attachments;
	std::vector <vk::SubpassDependency> dependencies;

	RenderPassAssembler(const vk::Device &device_, littlevk::Deallocator *dal_)
			: device(device_), dal(dal_) {}

	// TODO: use a tuple builder or so to ctime record the attachments and verifying everything
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

		SubpassAssembler(RenderPassAssembler &parent_, const vk::PipelineBindPoint &bindpoint_)
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
				{}, bindpoint,
				inputs, colors,
				{}, depth ? &(*depth) : nullptr, {}
			});

			return parent;
		}
	};

	SubpassAssembler add_subpass(const vk::PipelineBindPoint &bindpoint) {
		return SubpassAssembler(*this, bindpoint);
	}

	RenderPassAssembler &add_dependency(uint32_t src, uint32_t dst, const vk::PipelineStageFlags &src_mask, const vk::PipelineStageFlags &dst_mask) {
		dependencies.emplace_back(src, dst, src_mask, dst_mask);
		return *this;
	}

	operator vk::RenderPass() {
		return littlevk::render_pass(device, attachments, subpasses, dependencies).unwrap(dal);
	}
};

// Vulkan render pass begin info wrapper
template <size_t AttachmentCount>
struct RenderPassBeginInfo {
	vk::RenderPass                               m_render_pass;
	vk::Framebuffer                              m_framebuffer;
	vk::Extent2D                                 m_extent;
	std::array <vk::ClearValue, AttachmentCount> m_clear_values;

	operator vk::RenderPassBeginInfo() const {
		return vk::RenderPassBeginInfo(
			m_render_pass,
			m_framebuffer,
			{ { 0, 0 }, m_extent },
			m_clear_values
		);
	}

	RenderPassBeginInfo render_pass(vk::RenderPass render_pass) && {
		m_render_pass = render_pass;
		return std::move(*this);
	}

	RenderPassBeginInfo framebuffer(vk::Framebuffer framebuffer) && {
		m_framebuffer = framebuffer;
		return std::move(*this);
	}

	RenderPassBeginInfo extent(vk::Extent2D extent) && {
		m_extent = extent;
		return std::move(*this);
	}

	template <typename ... Args>
	requires std::is_constructible_v <vk::ClearColorValue, Args...>
	RenderPassBeginInfo clear_color(size_t index, const Args &... args) && {
		m_clear_values[index] = vk::ClearColorValue(args...);
		return std::move(*this);
	}

	template <typename ... Args>
	requires std::is_constructible_v <vk::ClearDepthStencilValue, Args...>
	RenderPassBeginInfo clear_depth(size_t index, const Args &... args) && {
		m_clear_values[index] = vk::ClearDepthStencilValue(args...);
		return std::move(*this);
	}

	RenderPassBeginInfo clear_value(size_t index, vk::ClearValue clear_value) && {
		m_clear_values[index] = clear_value;
		return std::move(*this);
	}
};

// Presets
template <size_t AttachmentCount>
inline RenderPassBeginInfo <AttachmentCount> default_rp_begin_info
(
	const vk::RenderPass &render_pass,
	const vk::Framebuffer &framebuffer,
	const vk::Extent2D &extent
)
{
	// Infers attachment layouts
	static_assert(AttachmentCount == 1 || AttachmentCount == 2,
		"Can only infer up to two attachments");

	// 1: Color only
	if constexpr (AttachmentCount == 1) {
		return RenderPassBeginInfo <AttachmentCount> ()
			.render_pass(render_pass)
			.framebuffer(framebuffer)
			.extent(extent)
			.clear_color(0, std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f });
	}

	// 2: Color + Depth
	if constexpr (AttachmentCount == 2) {
		return RenderPassBeginInfo <AttachmentCount> ()
			.render_pass(render_pass)
			.framebuffer(framebuffer)
			.extent(extent)
			.clear_color(0, std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f })
			.clear_depth(1, 1.0f, 0);
	}
}

template <size_t AttachmentCount>
inline RenderPassBeginInfo <AttachmentCount> default_rp_begin_info
(
	const vk::RenderPass &render_pass,
	const vk::Framebuffer &framebuffer,
	const Window *window
)
{
	return default_rp_begin_info <AttachmentCount> (render_pass, framebuffer, window->extent);
}

// Configuring viewport and scissor
struct RenderArea {
	// float x, y, w, h;
	vk::Extent2D extent;

	RenderArea() = delete;
	RenderArea(const Window *window) : extent(window->extent) {}
};

inline void viewport_and_scissor(const vk::CommandBuffer &cmd, const RenderArea &area)
{
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) area.extent.width,
		(float) area.extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor {
		{}, area.extent
	};

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

inline void destroy_present_syncronization(const vk::Device &device, const PresentSyncronization &sync)
{
	for (const vk::Semaphore &semaphore : sync.image_available)
		device.destroySemaphore(semaphore);

	for (const vk::Semaphore &semaphore : sync.render_finished)
		device.destroySemaphore(semaphore);

	for (const vk::Fence &fence : sync.in_flight)
		device.destroyFence(fence);
}

// Return proxy for present syncronization
using PresentSyncronizationReturnProxy = DeviceReturnProxy <PresentSyncronization, destroy_present_syncronization>;

inline PresentSyncronizationReturnProxy present_syncronization(const vk::Device &device, uint32_t frames_in_flight)
{
	PresentSyncronization sync;

	// Default semaphores
	vk::SemaphoreCreateInfo semaphore_info {};

	// Signaled fences
	vk::FenceCreateInfo fence_info {};
	fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

	for (uint32_t i = 0; i < frames_in_flight; i++) {
		sync.image_available.push_back(device.createSemaphore(semaphore_info));
		sync.render_finished.push_back(device.createSemaphore(semaphore_info));
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
        auto [result, image_index] = device.acquireNextImageKHR(swapchain, UINT64_MAX, sync_frame.image_available, nullptr);

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

        vk::PresentInfoKHR present_info {
		wait_semaphores,
                swapchain,
                index
        };

        try {
		// TODO: check return value here
                (void) queue.presentKHR(present_info);
        } catch (vk::OutOfDateKHRError &e) {
		microlog::warning("present_image", "Swapchain out of date\n");
                return { SurfaceOperation::eResize, 0 };
        }

        return { SurfaceOperation::eOk, 0 };
}

// Check if a physical device supports a set of extensions
inline bool physical_device_able(const vk::PhysicalDevice &phdev, const std::vector <const char *> &extensions)
{
	// Get the device extensions
	std::vector <vk::ExtensionProperties> device_extensions =
			phdev.enumerateDeviceExtensionProperties();

	// Check if all the extensions are supported
	for (const char *extension : extensions) {
		if (std::find_if(device_extensions.begin(), device_extensions.end(),
				[&extension](const vk::ExtensionProperties &prop) {
					return !strcmp(prop.extensionName, extension);
				}) == device_extensions.end()) {
			microlog::warning("physical_device_able", "Extension \"%s\" is not supported\n", extension);
			return false;
		}
	}

	return true;
}

// Pick physical device according to some criteria
inline vk::PhysicalDevice pick_physical_device(const std::function <bool (const vk::PhysicalDevice &)> &predicate)
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

struct Skeleton {
        vk::Device device;
        vk::PhysicalDevice phdev = nullptr;
        vk::SurfaceKHR surface;

        vk::Queue graphics_queue;
        vk::Queue present_queue;

        Swapchain swapchain;
        Window *window = nullptr;

	// TODO: no default constructor, this turns into a constructor...
	bool skeletonize
	(
		const vk::PhysicalDevice &,
                const vk::Extent2D &,
                const std::string &,
		const std::vector <const char *> &,
		const std::optional <vk::PhysicalDeviceFeatures2KHR> & = std::nullopt,
		const std::optional <vk::PresentModeKHR> & = std::nullopt
	);

	// TODO: virtual destructor
	virtual bool destroy();

	void resize();
	float aspect_ratio() const;
};

// Create logical device on an arbitrary queue
inline vk::Device device
(
		const vk::PhysicalDevice &phdev,
		const uint32_t queue_family,
		const uint32_t queue_count,
		const std::vector <const char *> &extensions,
		const std::optional <vk::PhysicalDeviceFeatures2KHR> &features = {}
)
{
	// Queue priorities
	std::vector <float> queue_priorities(queue_count, 1.0f);

	// Create the device info
	vk::DeviceQueueCreateInfo queue_info {
		vk::DeviceQueueCreateFlags(),
		queue_family, queue_count,
		queue_priorities.data()
	};

	// Device features
	vk::PhysicalDeviceFeatures device_features;
	device_features.independentBlend = true;
	device_features.fillModeNonSolid = true;
	device_features.geometryShader = true;

	vk::PhysicalDeviceFeatures2KHR secondary_features;
	vk::DeviceCreateInfo device_info {
		vk::DeviceCreateFlags(), queue_info,
		{}, extensions, &device_features, nullptr
	};

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
		const std::optional <vk::PhysicalDeviceFeatures2KHR> &features = {})
{
	auto families = phdev.getQueueFamilyProperties();
	uint32_t count = families[indices.graphics].queueCount;
	return device(phdev, indices.graphics, count, extensions, features);
}

// TODO: pass extensions
inline bool Skeleton::skeletonize
(
		const vk::PhysicalDevice &phdev_,
                const vk::Extent2D &extent,
                const std::string &title,
		const std::vector <const char *> &device_extensions,
		const std::optional <vk::PhysicalDeviceFeatures2KHR> &features,
		const std::optional <vk::PresentModeKHR> &priority_present_mode
)
{
        phdev = phdev_;
        window = make_window(extent, title);
        surface = make_surface(*window);

        QueueFamilyIndices queue_family = find_queue_families(phdev, surface);
        device = littlevk::device(phdev, queue_family, device_extensions, features);
	swapchain = littlevk::swapchain(
                phdev, device, surface,
                window->extent, queue_family,
		priority_present_mode
	);

        graphics_queue = device.getQueue(queue_family.graphics, 0);
        present_queue = device.getQueue(queue_family.present, 0);

	return true;
}

inline bool Skeleton::destroy()
{
	device.waitIdle();
        destroy_window(window);
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
		glfwGetFramebufferSize(window->handle, &current_width, &current_height);
		while (current_width == 0 || current_height == 0) {
			glfwWaitEvents();
			glfwGetFramebufferSize(window->handle, &current_width, &current_height);
		}

		glfwGetFramebufferSize(window->handle, &new_width, &new_height);
	} while (new_width != current_width || new_height != current_height);

	// Resize only after stable sizes
	vk::SurfaceCapabilitiesKHR caps = phdev.getSurfaceCapabilitiesKHR(surface);
	new_width = std::clamp(new_width, int(caps.minImageExtent.width), int(caps.maxImageExtent.width));
	new_height = std::clamp(new_height, int(caps.minImageExtent.height), int(caps.maxImageExtent.height));

	device.waitIdle();

	vk::Extent2D new_extent = { uint32_t(new_width), uint32_t(new_height) };
	littlevk::resize(device, swapchain, new_extent);
	window->extent = new_extent;
}

inline float Skeleton::aspect_ratio() const
{
	return (float) window->extent.width / (float) window->extent.height;
}

// Get memory file decriptor
inline int find_memory_fd(const vk::Device &device, const vk::DeviceMemory &memory)
{
	vk::MemoryGetFdInfoKHR info {};
	info.memory = memory;
	info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;

	VkMemoryGetFdInfoKHR cinfo = static_cast <VkMemoryGetFdInfoKHR> (info);

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
};

// Return proxy for buffers
static void destroy_buffer(const vk::Device &device, const Buffer &buffer)
{
	device.destroyBuffer(buffer.buffer);
	device.freeMemory(buffer.memory);
}

using BufferReturnProxy = DeviceReturnProxy <Buffer, destroy_buffer>;

// Find memory type
inline uint32_t find_memory_type(const vk::PhysicalDeviceMemoryProperties &mem_props,
		uint32_t type_filter,
		vk::MemoryPropertyFlags properties)
{
	uint32_t type_index = uint32_t(~0);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_filter & 1) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
			type_index = i;
			break;
		}

		type_filter >>= 1;
	}

	if (type_index == uint32_t(~0)) {
		microlog::error("find_memory_type", "No memory type found\n");
		return std::numeric_limits <uint32_t> ::max();
	}

	return type_index;
}

inline BufferReturnProxy buffer(const vk::Device &device, const vk::PhysicalDeviceMemoryProperties &properties, size_t size, const vk::BufferUsageFlags &flags, bool external = false)
{
        Buffer buffer;

        vk::BufferCreateInfo buffer_info {
                {}, size, flags,
                vk::SharingMode::eExclusive, 0, nullptr
        };

	// Exporting the buffer data for other APIs (e.g. CUDA)
	vk::ExternalMemoryBufferCreateInfo external_info {};
	if (external) {
		external_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		buffer_info.pNext = &external_info;
	}

	// Allocate buffer info
        buffer.buffer = device.createBuffer(buffer_info);
        buffer.requirements = device.getBufferMemoryRequirements(buffer.buffer);

        vk::MemoryAllocateInfo buffer_alloc_info {
                buffer.requirements.size,
		find_memory_type
		(
                        properties,
			buffer.requirements.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eHostVisible
				| vk::MemoryPropertyFlagBits::eHostCoherent
                )
        };

	// Export the buffer data for other APIs (e.g. CUDA)
	// TODO: general function
	vk::ExportMemoryAllocateInfo export_info {};
	if (external) {
		export_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		buffer_alloc_info.pNext = &export_info;
	}

	// Allocate the device buffer
        buffer.memory = device.allocateMemory(buffer_alloc_info);
        device.bindBufferMemory(buffer.buffer, buffer.memory, 0);

        return buffer;
}

using FilledBufferReturnProxy = ComposedReturnProxy <Buffer>;

template <typename T>
inline FilledBufferReturnProxy buffer(const vk::Device &device, const vk::PhysicalDeviceMemoryProperties &properties, const std::vector <T> &vec, const vk::BufferUsageFlags &flags, bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties, vec.size() * sizeof(T), flags, external).defer(dq);
	upload(device, buffer, vec);
	return { buffer, dq };
}

template <typename T, size_t N>
inline FilledBufferReturnProxy buffer(const vk::Device &device, const vk::PhysicalDeviceMemoryProperties &properties, const std::array <T, N> &vec, const vk::BufferUsageFlags &flags, bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties, vec.size() * sizeof(T), flags, external).defer(dq);
	upload(device, buffer, vec);
	return { buffer, dq };
}

template <typename T>
inline FilledBufferReturnProxy buffer(const vk::Device &device, const vk::PhysicalDeviceMemoryProperties &properties, const T *const data, size_t size, const vk::BufferUsageFlags &flags, bool external = false)
{
	DeallocationQueue dq;
	Buffer buffer = littlevk::buffer(device, properties, size, flags, external).defer(dq);
	upload(device, buffer, data);
	return { buffer, dq };
}

// TODO: overload with size
inline void upload(const vk::Device &device, const Buffer &buffer, const void *data)
{
        void *mapped = device.mapMemory(buffer.memory, 0, buffer.requirements.size);
        std::memcpy(mapped, data, buffer.requirements.size);
        device.unmapMemory(buffer.memory);
}

template <typename T>
inline void upload(const vk::Device &device, const Buffer &buffer, const std::vector <T> &vec)
{
        size_t size = std::min(buffer.requirements.size, vec.size() * sizeof(T));
        void *mapped = device.mapMemory(buffer.memory, 0, size);
        std::memcpy(mapped, vec.data(), size);
        device.unmapMemory(buffer.memory);

        // Warn if fewer elements were transferred
        // TODO: or return some kind of error code?
        if (size < vec.size() * sizeof(T))
		microlog::warning("upload", "Fewer elements were transferred than may have been expected");
}

template <typename T, size_t N>
inline void upload(const vk::Device &device, const Buffer &buffer, const std::array <T, N> &arr)
{
	size_t size = std::min(buffer.requirements.size, arr.size() * sizeof(T));
	void *mapped = device.mapMemory(buffer.memory, 0, size);
	std::memcpy(mapped, arr.data(), size);
	device.unmapMemory(buffer.memory);

	// Warn if fewer elements were transferred
	if (size < N * sizeof(T))
		microlog::warning("upload", "Fewer elements were transferred than may have been expected");
}

inline void download(const vk::Device &device, const Buffer &buffer, void *data)
{
	void *mapped = device.mapMemory(buffer.memory, 0, buffer.requirements.size);
	std::memcpy(data, mapped, buffer.requirements.size);
	device.unmapMemory(buffer.memory);
}

template <typename T>
inline void download(const vk::Device &device, const Buffer &buffer, std::vector <T> &vec)
{
	size_t size = std::min(buffer.requirements.size, vec.size() * sizeof(T));
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

	vk::Image operator*() const {
		return image;
	}

	vk::DeviceSize device_size() const {
		return requirements.size;
	}
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
	bool external;

	constexpr ImageCreateInfo(uint32_t width_, uint32_t height_,
		vk::Format format_, vk::ImageUsageFlags usage_,
		vk::ImageAspectFlags aspect_, bool external_ = false)
			: width(width_), height(height_),
			format(format_), usage(usage_),
			aspect(aspect_), external(external_) {}

	constexpr ImageCreateInfo(vk::Extent2D extent,
		vk::Format format_, vk::ImageUsageFlags usage_,
		vk::ImageAspectFlags aspect_, bool external_ = false)
			: width(extent.width), height(extent.height),
			format(format_), usage(usage_),
			aspect(aspect_), external(external_) {}
};

inline ImageReturnProxy image(const vk::Device &device, const ImageCreateInfo &info, const vk::PhysicalDeviceMemoryProperties &properties)
{
        Image image;

        vk::ImageCreateInfo image_info {
                {}, vk::ImageType::e2D, info.format,
                vk::Extent3D { info.width, info.height, 1 },
                1, 1, vk::SampleCountFlagBits::e1,
                vk::ImageTiling::eOptimal,
                info.usage,
                vk::SharingMode::eExclusive, 0, nullptr,
                vk::ImageLayout::eUndefined
        };

	vk::ExternalMemoryImageCreateInfo external_info {};
	if (info.external) {
		external_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		image_info.pNext = &external_info;
	}

        image.image = device.createImage(image_info);
        image.requirements = device.getImageMemoryRequirements(image.image);

        vk::MemoryAllocateInfo alloc_info {
                image.requirements.size,
		find_memory_type
		(
                        properties,
			image.requirements.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eDeviceLocal
                )
        };

	vk::ExportMemoryAllocateInfo export_info {};
	if (info.external) {
		export_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
		alloc_info.pNext = &export_info;
	}

        image.memory = device.allocateMemory(alloc_info);
        device.bindImageMemory(image.image, image.memory, 0);

        vk::ImageViewCreateInfo view_info {
                {}, image.image, vk::ImageViewType::e2D, info.format,
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

        return image;
}

// TODO: pure template version that skips switch statements
template <typename ImageType>
inline void transition(const vk::CommandBuffer &cmd,
		const ImageType &image,
		const vk::ImageLayout old_layout,
		const vk::ImageLayout new_layout)
{
	static_assert(std::is_same_v <ImageType, Image> || std::is_same_v <ImageType, vk::Image>, "littlevk::transition: ImageType must be either littlevk::Image or vk::Image");

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
		microlog::error("transition layout", "Unsupported old layout %s", vk::to_string(old_layout).c_str());
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
		source_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
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
		microlog::error("transition layout", "Unsupported old layout %s", vk::to_string(old_layout).c_str());
		break;
	}

	// Destination stage
	vk::AccessFlags dst_access_mask = {};
	switch (new_layout) {
	case vk::ImageLayout::eColorAttachmentOptimal:
		dst_access_mask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		dst_access_mask = vk::AccessFlagBits::eDepthStencilAttachmentRead
			| vk::AccessFlagBits::eDepthStencilAttachmentWrite;
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
		microlog::error("transition layout", "Unsupported new layout %s", vk::to_string(new_layout).c_str());
		break;
	}

	// Destination stage
	vk::PipelineStageFlags destination_stage;
	switch (new_layout) {
	case vk::ImageLayout::eColorAttachmentOptimal:
		destination_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput; break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destination_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests; break;
	case vk::ImageLayout::eGeneral:
		destination_stage = vk::PipelineStageFlagBits::eHost; break;
	case vk::ImageLayout::ePresentSrcKHR:
		destination_stage = vk::PipelineStageFlagBits::eBottomOfPipe; break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destination_stage = vk::PipelineStageFlagBits::eFragmentShader; break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		destination_stage = vk::PipelineStageFlagBits::eTransfer; break;
	default:
		microlog::error("transition layout", "Unsupported new layout %s", vk::to_string(new_layout).c_str());
		break;
	}

	// Aspect mask
	vk::ImageAspectFlags aspect_mask;
	if (new_layout == vk::ImageLayout::eDepthStencilAttachmentOptimal) {
		aspect_mask = vk::ImageAspectFlagBits::eDepth;
		// if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint)
		// 	aspect_mask |= vk::ImageAspectFlagBits::eStencil;
	} else {
		aspect_mask = vk::ImageAspectFlagBits::eColor;
	}

	// Create the barrier
	vk::ImageSubresourceRange image_subresource_range {
		aspect_mask,
		0, 1, 0, 1
	};

	vk::Image target_image;
	if constexpr (std::is_same_v <ImageType, littlevk::Image>)
		target_image = *image;
	else
		target_image = image;

	vk::ImageMemoryBarrier barrier {
		src_access_mask, dst_access_mask,
		old_layout, new_layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		target_image,
		image_subresource_range
	};

	// Add the barrier
	return cmd.pipelineBarrier(source_stage, destination_stage, {}, {}, {}, barrier);
}

// Copying buffer to image
inline void copy_buffer_to_image
(
 	const vk::CommandBuffer &cmd,
	const vk::Image &image,
	const Buffer &buffer,
	const vk::Extent2D &extent,
	const vk::ImageLayout &layout
)
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

inline void copy_buffer_to_image
(
 	const vk::CommandBuffer &cmd,
	const Image &image,
	const Buffer &buffer,
	const vk::ImageLayout &layout
)
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
inline void copy_image_to_buffer
(
 	const vk::CommandBuffer &cmd,
	const vk::Image &image,
	const Buffer &buffer,
	const vk::Extent2D &extent,
	const vk::ImageLayout &layout
)
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

inline void copy_image_to_buffer
(
 	const vk::CommandBuffer &cmd,
	const Image &image,
	const Buffer &buffer,
	const vk::ImageLayout &layout
)
{
	vk::BufferImageCopy region {
		0, 0, 0,
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
inline void bind(const vk::Device &device, const vk::DescriptorSet &dset, const Image &img, const vk::Sampler &sampler)
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

	device.updateDescriptorSets({ write }, {});
}

inline void bind(const vk::Device &device, const vk::DescriptorSet &dset, const Buffer &buffer, uint32_t binding)
{
	vk::DescriptorBufferInfo buffer_info {
		*buffer, 0, vk::WholeSize
	};

	vk::WriteDescriptorSet write {
		dset, binding, 0, 1,
		vk::DescriptorType::eStorageBuffer,
		{}, &buffer_info
	};

	device.updateDescriptorSets({ write }, {});
}

// Construct framebuffer from image
inline FramebufferReturnProxy framebuffer(const vk::Device &device, const vk::RenderPass &rp, const littlevk::Image &image)
{
	std::array <vk::ImageView, 1> attachments = {
		image.view
	};

	vk::FramebufferCreateInfo framebuffer_info {
		{}, rp,
		(uint32_t) attachments.size(), attachments.data(),
		image.extent.width, image.extent.height, 1
	};

	return device.createFramebuffer(framebuffer_info);
}

// Single-time command buffer submission
inline void submit_now(const vk::Device &device, const vk::CommandPool &pool, const vk::Queue &queue,
		const std::function<void (const vk::CommandBuffer &)> &function)
{
	vk::CommandBuffer cmd = device.allocateCommandBuffers(
		vk::CommandBufferAllocateInfo {
			pool, vk::CommandBufferLevel::ePrimary, 1
		}
	).front();

	cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
	function(cmd);
	cmd.end();

	vk::SubmitInfo submit_info {
		0, nullptr, nullptr,
		1, &cmd,
		0, nullptr
	};

	(void) queue.submit(1, &submit_info, nullptr);
	queue.waitIdle();

	device.freeCommandBuffers(pool, 1, &cmd);
}

// Other companion functions with automatic memory management
static void destroy_command_pool(const vk::Device &device, const vk::CommandPool &pool)
{
	device.destroyCommandPool(pool);
}

using CommandPoolReturnProxy = DeviceReturnProxy <vk::CommandPool, destroy_command_pool>;

inline CommandPoolReturnProxy command_pool(const vk::Device &device, const vk::CommandPoolCreateInfo &info)
{
	vk::CommandPool pool;
	if (device.createCommandPool(&info, nullptr, &pool) != vk::Result::eSuccess)
		return  true;

	return std::move(pool);
}

static void destroy_descriptor_pool(const vk::Device &device, const vk::DescriptorPool &pool)
{
	device.destroyDescriptorPool(pool);
}

using DescriptorPoolReturnProxy = DeviceReturnProxy <vk::DescriptorPool, destroy_descriptor_pool>;

inline DescriptorPoolReturnProxy descriptor_pool(const vk::Device &device, const vk::DescriptorPoolCreateInfo &info)
{
	vk::DescriptorPool pool;
	if (device.createDescriptorPool(&info, nullptr, &pool) != vk::Result::eSuccess)
		return  true;

	return std::move(pool);
}

static void destroy_descriptor_set_layout(const vk::Device &device, const vk::DescriptorSetLayout &layout)
{
	device.destroyDescriptorSetLayout(layout);
}

using DescriptorSetLayoutReturnProxy = DeviceReturnProxy <vk::DescriptorSetLayout, destroy_descriptor_set_layout>;

inline DescriptorSetLayoutReturnProxy descriptor_set_layout(const vk::Device &device, const vk::DescriptorSetLayoutCreateInfo &info)
{
	vk::DescriptorSetLayout layout;
	if (device.createDescriptorSetLayout(&info, nullptr, &layout) != vk::Result::eSuccess)
		return  true;

	return std::move(layout);
}

static void destroy_pipeline_layout(const vk::Device &device, const vk::PipelineLayout &layout)
{
	device.destroyPipelineLayout(layout);
}

using PipelineLayoutReturnProxy = DeviceReturnProxy <vk::PipelineLayout, destroy_pipeline_layout>;

inline PipelineLayoutReturnProxy pipeline_layout(const vk::Device &device, const vk::PipelineLayoutCreateInfo &info)
{
	vk::PipelineLayout layout;
	if (device.createPipelineLayout(&info, nullptr, &layout) != vk::Result::eSuccess)
		return  true;

	return std::move(layout);
}

static void destroy_sampler(const vk::Device &device, const vk::Sampler &sampler)
{
	device.destroySampler(sampler);
}

using SamplerReturnProxy = DeviceReturnProxy <vk::Sampler, destroy_sampler>;

inline SamplerReturnProxy sampler(const vk::Device &device, const vk::SamplerCreateInfo &info)
{
	vk::Sampler sampler;
	if (device.createSampler(&info, nullptr, &sampler) != vk::Result::eSuccess)
		return  true;

	return std::move(sampler);
}

// Default sampler builder
struct SamplerAssembler {
	const vk::Device &device;
	Deallocator *const dal;

	vk::Filter mag = vk::Filter::eLinear;
	vk::Filter min = vk::Filter::eLinear;

	SamplerAssembler(const vk::Device &device, Deallocator *const dal)
			: device(device), dal(dal) {}

	operator vk::Sampler() const {
		vk::SamplerCreateInfo info {
			vk::SamplerCreateFlags {},
			mag, min,
			vk::SamplerMipmapMode::eLinear,
			vk::SamplerAddressMode::eRepeat,
			vk::SamplerAddressMode::eRepeat,
			vk::SamplerAddressMode::eRepeat,
			0.0f,
			vk::False,
			1.0f,
			vk::False,
			vk::CompareOp::eAlways,
			0.0f, 0.0f,
			vk::BorderColor::eIntOpaqueBlack,
			vk::False
		};

		return sampler(device, info).unwrap(dal);
	}
};

// Rebinder pattern to do all allocations at once, then unpack
template <typename ... Args>
struct linked_device_allocator : std::tuple <Args...> {
	const vk::Device &device;
	const vk::PhysicalDeviceMemoryProperties &properties;
	littlevk::Deallocator *dal = nullptr;

	constexpr linked_device_allocator(const vk::Device &device_,
		const vk::PhysicalDeviceMemoryProperties &properties_,
		littlevk::Deallocator *dal_,
		const Args & ... args)
			: std::tuple <Args...> (args...),
			device(device_),
			properties(properties_),
			dal(dal_) {}

	constexpr linked_device_allocator(const vk::Device &device_,
		const vk::PhysicalDeviceMemoryProperties &properties_,
		littlevk::Deallocator *dal_,
		const std::tuple <Args...> &args)
			: std::tuple <Args...> (args),
			device(device_),
			properties(properties_),
			dal(dal_) {}

	operator const auto &() const {
		if constexpr (sizeof...(Args) == 1)
			return std::get <0> (*this);
		else
			return *this;
	}

	template <typename ... InfoArgs>
	requires std::is_constructible_v <ImageCreateInfo, InfoArgs...>
	[[nodiscard]] linked_device_allocator <Args..., littlevk::Image>
	image(const InfoArgs & ... args) {
		ImageCreateInfo info(args...);
		auto image = littlevk::image(device, info, properties).unwrap(dal);
		auto new_values = std::tuple_cat((std::tuple <Args...>) *this, std::make_tuple(image));
		return { device, properties, dal, new_values };
	}
	
	template <typename ... BufferArgs>
	[[nodiscard]] linked_device_allocator <Args..., littlevk::Buffer>
	buffer(const BufferArgs & ... args) {
		auto buffer = littlevk::buffer(device, properties, args...).unwrap(dal);
		auto new_values = std::tuple_cat((std::tuple <Args...>) *this, std::make_tuple(buffer));
		return { device, properties, dal, new_values };
	}
};

// Starts with nothing
constexpr linked_device_allocator <> bind
(
	const vk::Device &device,
	const vk::PhysicalDeviceMemoryProperties &properties,
	littlevk::Deallocator *dal
)
{
	return { device, properties, dal };
}

struct linked_device_descriptor_pool {
	const vk::Device &device;
	const vk::DescriptorPool &pool;

	[[nodiscard]]
	std::vector <vk::DescriptorSet> allocateDescriptorSets(const vk::DescriptorSetLayout &dsl) const {
		return device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo { pool, dsl });
	}
};

constexpr linked_device_descriptor_pool bind(const vk::Device &device, const vk::DescriptorPool &pool)
{
	return { device, pool };
}

// Descriptor set update structures
struct DescriptorElementBase {
	uint32_t element;
};

struct DescriptorImageElementInfo : vk::DescriptorImageInfo, DescriptorElementBase {
	constexpr DescriptorImageElementInfo(const vk::Sampler &sampler, const vk::ImageView &view, const vk::ImageLayout &layout, uint32_t element = 0)
			: vk::DescriptorImageInfo(sampler, view, layout),
			DescriptorElementBase(element) {}
};

struct DescriptorBufferElementInfo : vk::DescriptorBufferInfo, DescriptorElementBase {
	constexpr DescriptorBufferElementInfo(const vk::Buffer &buffer, uint32_t offset, uint32_t range, uint32_t element = 0)
			: vk::DescriptorBufferInfo(buffer, offset, range),
			DescriptorElementBase(element) {}
};

using DescriptorTypeElementInfo = std::variant <DescriptorImageElementInfo, DescriptorBufferElementInfo>;

struct Visitor {
	vk::WriteDescriptorSet &ref;

	void operator()(const vk::DescriptorImageInfo &image_info) {
		ref.pImageInfo = &image_info;
	}

	void operator()(const vk::DescriptorBufferInfo &buffer_info) {
		ref.pBufferInfo = &buffer_info;
	}
};

template <size_t N>
void descriptor_set_update
(
	const vk::Device &device,
	const vk::DescriptorSet &dset,
	const std::array <vk::DescriptorSetLayoutBinding, N> &bindings,
	const std::array <DescriptorTypeElementInfo, N> &infos
)
{
	std::array <vk::WriteDescriptorSet, N> writes;
	for (size_t i = 0; i < N; i++) {
		const auto &binding = bindings[i];

		// TODO: pass elements
		writes[i] = vk::WriteDescriptorSet {
			dset, binding.binding, 0,
			binding.descriptorCount,
			binding.descriptorType,
			nullptr, nullptr, nullptr
		};

		std::visit(Visitor(writes[i]), infos[i]);
	}

	device.updateDescriptorSets(writes, nullptr);
}

namespace shader {

// TODO: detail here as well...
using Defines = std::map <std::string, std::string>;
using Includes = std::set <std::string>;

// Local structs
struct _compile_out {
	std::vector <unsigned int> 	spirv = {};
	std::string			log = "";
	std::string			source = "";
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

static _compile_out glsl_to_spriv
(
	const std::string &source,
	const std::set <std::string> &paths,
	const std::map <std::string, std::string> &defines,
	const vk::ShaderStageFlagBits &shader_type
)
{
	// Output
	_compile_out out;

	// Compile shader
	EShLanguage stage = translate_shader_stage(shader_type);

	const char *shaderStrings[1];
	shaderStrings[0] = source.data();

	glslang::TShader shader(stage);

	shader.setEnvTarget(glslang::EShTargetLanguage::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_6);
	shader.setStrings(shaderStrings, 1);

	// Enable SPIR-V and Vulkan rules when parsing GLSL
	EShMessages messages = (EShMessages) (EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);

	// Include directories
	standalone::DirectoryIncluder includer;
	for (const auto &path : paths)
		includer.include(path);

	// ShaderIncluder includer;
	if (!shader.parse(GetDefaultResources(), 450, false, messages, includer)) {
		out.log = shader.getInfoLog();
		out.source = source;
		return out;
	}

	// Link the program
	glslang::TProgram program;

	// program.s
	program.addShader(&shader);

	if (!program.link(messages)) {
		out.log = program.getInfoLog();
		return out;
	}

	glslang::GlslangToSpv(*program.getIntermediate(stage), out.spirv);
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
		out += std::to_string(line_num) + ": " + line + "\n";
		line_num++;
	}

	return out;
}

// Return proxy specialization for shader modules
static void destroy_shader_module(const vk::Device &device, const vk::ShaderModule &shader)
{
	device.destroyShaderModule(shader);
}

using ShaderModuleReturnProxy = DeviceReturnProxy <vk::ShaderModule, destroy_shader_module>;

// Compile shader
inline ShaderModuleReturnProxy compile
(
	const vk::Device &device,
	const std::string &source,
	const vk::ShaderStageFlagBits &shader_type,
	const Includes &includes = {},
	const Defines &defines = {}
)
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	_compile_out out = glsl_to_spriv(source, includes, defines, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		microlog::error("shader", "Shader compilation failed:\n%s\nSource:\n%s",
				out.log.c_str(), fmt_lines(out.source).c_str());
		return true;
	}

        vk::ShaderModuleCreateInfo create_info(
                vk::ShaderModuleCreateFlags(),
                out.spirv.size() * sizeof(uint32_t),
                out.spirv.data()
        );

        return device.createShaderModule(create_info);
}

inline ShaderModuleReturnProxy compile
(
	const vk::Device &device,
	const std::filesystem::path &path,
	const vk::ShaderStageFlagBits &shader_type,
	const Includes &includes = {},
	const Defines &defines = {}
)
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	std::string source = standalone::readfile(path);
	_compile_out out = glsl_to_spriv(source, includes, defines, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		microlog::error(__FUNCTION__, "Shader compilation failed:\n%s\nSource:\n%s",
				out.log.c_str(), fmt_lines(out.source).c_str());
		return true;
	}

        vk::ShaderModuleCreateInfo create_info(
                vk::ShaderModuleCreateFlags(),
                out.spirv.size() * sizeof(uint32_t),
                out.spirv.data()
        );

        return device.createShaderModule(create_info);
}

} // namespace shader

namespace pipeline {

static void destroy_pipeline(const vk::Device &device, const vk::Pipeline &pipeline)
{
	device.destroyPipeline(pipeline);
}

using PipelineReturnProxy = DeviceReturnProxy <vk::Pipeline, destroy_pipeline>;

struct GraphicsCreateInfo {
	std::optional <vk::VertexInputBindingDescription> vertex_binding = std::nullopt;
	std::optional <vk::ArrayProxy <vk::VertexInputAttributeDescription>> vertex_attributes = std::nullopt;

	// vk::ShaderModule vertex_shader;
	// vk::ShaderModule fragment_shader;
	std::vector <vk::PipelineShaderStageCreateInfo> shader_stages;

	vk::Extent2D extent;

	vk::PolygonMode fill_mode = vk::PolygonMode::eFill;
	vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eBack;

	bool dynamic_viewport = false;
	bool alpha_blend = false;

	vk::PipelineLayout pipeline_layout;
	vk::RenderPass render_pass;
	uint32_t subpass;
};

inline PipelineReturnProxy compile(const vk::Device &device, const GraphicsCreateInfo &info)
{
	if (!info.shader_stages.size())
		microlog::error("pipeline::compile", "Empty shader stages\n");

	vk::PipelineVertexInputStateCreateInfo vertex_input_info { {}, nullptr, nullptr };
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
		(float) info.extent.width, (float) info.extent.height,
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
			{}, (uint32_t) dynamic_states.size(), dynamic_states.data()
		};
	} else {
		viewport_state = vk::PipelineViewportStateCreateInfo {
			{}, 1, &viewport, 1, &scissor
		};
	}

	vk::PipelineRasterizationStateCreateInfo rasterizer {
		{}, false, false,
		info.fill_mode,
		info.cull_mode,
		vk::FrontFace::eClockwise,
		false, 0.0f, 0.0f, 0.0f, 1.0f
	};

	vk::PipelineMultisampleStateCreateInfo multisampling {
		{}, vk::SampleCountFlagBits::e1
	};

	vk::PipelineDepthStencilStateCreateInfo depth_stencil {
		{}, true, true,
		vk::CompareOp::eLess,
		false, false,
		{}, {}, 0.0f, 1.0f
	};

	vk::PipelineColorBlendAttachmentState color_blend_attachment = {};

	if (info.alpha_blend) {
		color_blend_attachment = {
			true,
			vk::BlendFactor::eSrcAlpha, vk::BlendFactor::eOneMinusSrcAlpha, vk::BlendOp::eAdd,
			vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
			vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
		};
	} else {
		color_blend_attachment = {
			false,
			vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
			vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
		};
	}

	vk::PipelineColorBlendStateCreateInfo color_blending {
		{}, false, vk::LogicOp::eCopy,
		1, &color_blend_attachment,
		{ 0.0f, 0.0f, 0.0f, 0.0f }
	};

	return device.createGraphicsPipeline(nullptr,
		vk::GraphicsPipelineCreateInfo {
			{},
			info.shader_stages,
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
		}
	).value;
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
template <typename T, typename ... Args>
constexpr size_t sizeof_all()
{
	if constexpr (sizeof...(Args)) {
		return sizeof(T) + sizeof_all <Args...> ();
	} else {
		return sizeof(T);
	}
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
		index, 0, type_translator <T> ::format, offset
	};
}

template <uint32_t index, uint32_t offset, typename T, typename ... Args>
constexpr std::array <vk::VertexInputAttributeDescription, 1 + sizeof...(Args)> attributes_for()
{
	if constexpr (sizeof...(Args)) {
		auto previous = attributes_for <index + 1, offset + sizeof(T), Args...> ();
		std::array <vk::VertexInputAttributeDescription, 1 + sizeof...(Args)> out;
		out[0] = attribute_for <index, offset, T> ();
		for (uint32_t i = 0; i < sizeof...(Args); i++)
			out[i + 1] = previous[i];
		return out;
	} else {	
		return { attribute_for <index, offset, T> () };
	}
}

template <typename ... Args>
struct VertexLayout {
	static constexpr size_t size = sizeof_all <Args...> ();

	static constexpr vk::VertexInputBindingDescription binding {
		0, size, vk::VertexInputRate::eVertex
	};

	static constexpr std::array <
		vk::VertexInputAttributeDescription,
		sizeof...(Args)
	> attributes {
		attributes_for <0, 0, Args...> ()
	};
};

// Group of shaders for a pipeline
struct ShaderStageBundle {
	vk::Device device;
	littlevk::Deallocator *dal;

	std::vector <vk::PipelineShaderStageCreateInfo> stages;

	ShaderStageBundle(const vk::Device &device, littlevk::Deallocator *dal)
			: device(device), dal(dal) {}

	ShaderStageBundle &attach(const std::string &glsl, vk::ShaderStageFlagBits flags) {
		vk::ShaderModule module = littlevk::shader::compile(device, glsl, flags).unwrap(dal);
		stages.push_back({{}, flags, module, "main"});
		return *this;
	}
};

// General purpose pipeline type
struct Pipeline {
	vk::Pipeline handle;
	vk::PipelineLayout layout;
	std::optional <vk::DescriptorSetLayout> dsl;
};

// General purpose pipeline compiler
struct PipelineAssembler {
	// Essential
	vk::Device device;
	littlevk::Window *window;
	littlevk::Deallocator *dal;

	// Render pass
	vk::RenderPass render_pass;
	uint32_t subpass;

	// Vertex information
	vk::VertexInputBindingDescription vertex_binding;
	std::vector <vk::VertexInputAttributeDescription> vertex_attributes;

	// Shader information
	ShaderStageBundle bundle;

	// Pipeline layout information
	std::vector <vk::DescriptorSetLayoutBinding> dsl_bindings;
	std::vector <vk::PushConstantRange> push_constants;
	
	PipelineAssembler(const vk::Device &device_, littlevk::Window *window_, littlevk::Deallocator *dal_)
			: device(device_), window(window_), dal(dal_), subpass(0), bundle(device_, dal_) {}

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
	
	PipelineAssembler &with_shader_bundle(const ShaderStageBundle &sb) {
		bundle = sb;
		return *this;
	}

	PipelineAssembler &with_dsl_binding(uint32_t binding, vk::DescriptorType type, uint32_t count, vk::ShaderStageFlagBits stage) {
		dsl_bindings.emplace_back(binding, type, count, stage);
		return *this;
	}

	template <size_t N>
	PipelineAssembler &with_dsl_bindings(const std::array <vk::DescriptorSetLayoutBinding, N> &bindings) {
		for (const auto &binding : bindings)
			dsl_bindings.push_back(binding);
		return *this;
	}

	template <typename T>
	PipelineAssembler &with_push_constant(vk::ShaderStageFlagBits stage) {
		push_constants.push_back(vk::PushConstantRange { stage, 0, sizeof(T) });
		return *this;
	}

	Pipeline compile() const {
		Pipeline pipeline;

		std::vector <vk::DescriptorSetLayout> dsls;
		if (dsl_bindings.size()) {
			vk::DescriptorSetLayout dsl = descriptor_set_layout(
				device, vk::DescriptorSetLayoutCreateInfo {
					{}, dsl_bindings
				}
			).unwrap(dal);

			dsls.push_back(dsl);
			pipeline.dsl = dsl;
		}
	
		pipeline.layout = littlevk::pipeline_layout
		(
			device,
			vk::PipelineLayoutCreateInfo{
				{}, dsls, push_constants
			}
		).unwrap(dal);

		littlevk::pipeline::GraphicsCreateInfo pipeline_info;

		pipeline_info.shader_stages = bundle.stages;
		pipeline_info.vertex_binding = vertex_binding;
		pipeline_info.vertex_attributes = vertex_attributes;
		pipeline_info.extent = window->extent;
		pipeline_info.pipeline_layout = pipeline.layout;
		pipeline_info.render_pass = render_pass;
		pipeline_info.subpass = subpass;
		pipeline_info.fill_mode = vk::PolygonMode::eFill;
		pipeline_info.cull_mode = vk::CullModeFlagBits::eNone;
		pipeline_info.dynamic_viewport = true;

		pipeline.handle = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);

		return pipeline;
	}

	operator Pipeline() const {
		return compile();
	}
};

}

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
inline VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDebugUtilsMessengerEXT
(
	VkInstance instance,
	const VkDebugUtilsMessengerCreateInfoEXT *create_info,
	const VkAllocationCallbacks *allocator,
	VkDebugUtilsMessengerEXT *debug_messenger
)
{
	microlog::assertion(Extensions::vkCreateDebugUtilsMessengerEXT(), "vkCreateDebugUtilsMessengerEXT", "Null function address\n");
	return Extensions::vkCreateDebugUtilsMessengerEXT()(instance, create_info, allocator, debug_messenger);
}

inline VKAPI_ATTR void VKAPI_CALL
vkDestroyDebugUtilsMessengerEXT
(
	VkInstance instance,
	VkDebugUtilsMessengerEXT debug_messenger,
	const VkAllocationCallbacks *allocator
)
{
	microlog::assertion(Extensions::vkDestroyDebugUtilsMessengerEXT(), "vkDestroyDebugUtilsMessengerEXT", "Null function address\n");
	return Extensions::vkDestroyDebugUtilsMessengerEXT()(instance, debug_messenger, allocator);
}


inline VKAPI_ATTR void VKAPI_CALL
vkCmdDrawMeshTasksEXT
(
	VkCommandBuffer commandBuffer,
	uint32_t groupCountX,
	uint32_t groupCountY,
	uint32_t groupCountZ
)
{
	microlog::assertion(Extensions::vkCmdDrawMeshTasksEXT(), "vkCmdDrawMeshTasksEXT", "Null function address\n");
	return Extensions::vkCmdDrawMeshTasksEXT()(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

inline VKAPI_ATTR void VKAPI_CALL
vkCmdDrawMeshTasksNV
(
	VkCommandBuffer commandBuffer,
	uint32_t taskCount,
	uint32_t firstTask
)
{
	microlog::assertion(Extensions::vkCmdDrawMeshTasksNV(), "vkCmdDrawMeshTasksNV", "Null function address\n");
	return Extensions::vkCmdDrawMeshTasksNV()(commandBuffer, taskCount, firstTask);
}

inline VKAPI_ATTR VkResult VKAPI_CALL
vkGetMemoryFdKHR
(
	VkDevice device,
	const VkMemoryGetFdInfoKHR *info,
	int *fd
)
{
	microlog::assertion(Extensions::vkGetMemoryFdKHR(), "vkGetMemoryFdKHR", "Null function address\n");
	return Extensions::vkGetMemoryFdKHR()(device, info, fd);
}
