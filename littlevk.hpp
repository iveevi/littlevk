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
#include <regex>
#include <set>
#include <variant>

// Miscellaneous standard library
#include <stdarg.h>

// Vulkan and GLFW
// TODO: suppor other window apis later
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>
#include <GLFW/glfw3.h>

// Glslang and SPIRV-Tools
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>

namespace littlevk {

namespace detail {

// Configuration parameters (free to user modification)
struct Config {
	std::vector <const char *> instance_extensions {};
	bool enable_validation_layers = true;
	bool abort_on_validation_error = false;
	bool enable_logging = true;
};

} // namespace detail

// Singleton config
inline detail::Config *config()
{
	static detail::Config config;
	return &config;
}

namespace log {

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
	// Always skip if logging is disabled
	if (!config()->enable_logging)
		return;

	printf("%s[littlevk::error]%s (%s) ",
		colors::error, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void warning(const char *header, const char *format, ...)
{
	// Always skip if logging is disabled
	if (!config()->enable_logging)
		return;

	printf("%s[littlevk::warning]%s (%s) ",
		colors::warning, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

inline void info(const char *header, const char *format, ...)
{
	// Always skip if logging is disabled
	if (!config()->enable_logging)
		return;

	printf("%s[littlevk::info]%s (%s) ",
		colors::info, colors::reset, header);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

} // namespace logging

namespace validation {

// Create debug messenger
static bool check_validation_layer_support(const std::vector <const char *> &validation_layers)
{
	// TODO: remove this initial part?
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

static VkResult make_debug_messenger(const VkInstance &instance,
		const VkDebugUtilsMessengerCreateInfoEXT *create_info,
		const VkAllocationCallbacks *allocator,
		VkDebugUtilsMessengerEXT *debug_messenger)
{
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");

	if (func != nullptr) {
		return func(instance,
			create_info,
			allocator,
			debug_messenger
		);
	}

	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void destroy_debug_messenger(const VkInstance &instance,
		const VkDebugUtilsMessengerEXT &debug_messenger,
		const VkAllocationCallbacks *allocator)
{
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

	if (func != nullptr) {
		func(instance,
			debug_messenger,
			allocator
		);
	}
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_logger
		(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageType,
		const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
		void *pUserData)
{
	// Errors
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		log::error("validation", "%s\n", pCallbackData->pMessage);
		if (config()->abort_on_validation_error)
			abort();
	} else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		log::warning("validation", "%s\n", pCallbackData->pMessage);
	} else {
		log::info("validation", "%s\n", pCallbackData->pMessage);
	}

	return VK_FALSE;
}

} // namespace validation

namespace detail {

// Get (or create) the singleton context
inline const vk::raii::Context &get_vulkan_context()
{
	// Global context
	static vk::raii::Context context;
	return context;
}

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
		// extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
		// extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
		// extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

		if (config()->enable_validation_layers) {
			// Add validation layers
			extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
	}

	return extensions;
}


// Get (or create) the singleton instance
inline const vk::raii::Instance &get_vulkan_instance()
{
	static bool initialized = false;
	static vk::raii::Instance instance = nullptr;

	// TODO: from config...
	static vk::ApplicationInfo app_info {
		"Kobra",
		VK_MAKE_VERSION(1, 0, 0),
		"Kobra",
		VK_MAKE_VERSION(1, 0, 0),
		VK_API_VERSION_1_3
	};

	// Skip if already initialized
	if (initialized)
		return instance;

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
			std::cerr << "Validation layers are not available" << std::endl;
			config()->enable_validation_layers = false;
		}

		if (config()->enable_validation_layers) {
			instance_info.enabledLayerCount = (uint32_t) validation_layers.size();
			instance_info.ppEnabledLayerNames = validation_layers.data();
		}
	}

	instance = vk::raii::Instance {
		get_vulkan_context(),
		instance_info
	};

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

		static vk::raii::DebugUtilsMessengerEXT debug_messenger { instance, debug_messenger_info };
	}

	initialized = true;
	return instance;
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
        GLFWwindow *handle = glfwCreateWindow(
                extent.width,
                extent.height,
                title.c_str(),
                nullptr,
                nullptr
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

inline vk::raii::SurfaceKHR make_surface(const Window &window)
{
	// Create the surface
	VkSurfaceKHR surface;
	VkResult result = glfwCreateWindowSurface(
		*detail::get_vulkan_instance(),
		window.handle,
		nullptr,
		&surface
	);

	// KOBRA_ASSERT(result == VK_SUCCESS, "Failed to create surface");
	return vk::raii::SurfaceKHR { detail::get_vulkan_instance(), surface };
}

// Coupling graphics and present queue families
struct QueueFamilyIndices {
	uint32_t graphics;
	uint32_t present;
};

// Find graphics queue family
inline uint32_t find_graphics_queue_family(const vk::raii::PhysicalDevice &phdev)
{
	// Get the queue families
	std::vector <vk::QueueFamilyProperties> queue_families =
			phdev.getQueueFamilyProperties();

	// Find the first one that supports graphics
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics)
			return i;
	}

	// If none found, throw an error
	// KOBRA_LOG_FUNC(Log::ERROR) << "No graphics queue family found\n";
	throw std::runtime_error("[Vulkan] No graphics queue family found");
}

// Find present queue family
inline uint32_t find_present_queue_family(const vk::raii::PhysicalDevice &phdev,
		const vk::raii::SurfaceKHR &surface)
{
	// Get the queue families
	std::vector <vk::QueueFamilyProperties> queue_families =
			phdev.getQueueFamilyProperties();

	// Find the first one that supports presentation
	for (uint32_t i = 0; i < queue_families.size(); i++) {
		if (phdev.getSurfaceSupportKHR(i, *surface))
			return i;
	}

	// If none found, throw an error
	// KOBRA_LOG_FUNC(Log::ERROR) << "No presentation queue family found\n";
	throw std::runtime_error("[Vulkan] No presentation queue family found");
}

// Get both graphics and present queue families
inline QueueFamilyIndices find_queue_families(const vk::raii::PhysicalDevice &phdev,
		const vk::raii::SurfaceKHR &surface)
{
	return {
		find_graphics_queue_family(phdev),
		find_present_queue_family(phdev, surface)
	};
}

// Swapchain structure
struct Swapchain {
	vk::Format				format;
	vk::raii::SwapchainKHR			swapchain = nullptr; // TODO: remove raii
	std::vector <vk::Image>			images;
	std::vector <vk::raii::ImageView>	image_views;

        Swapchain(const vk::raii::PhysicalDevice &,
                const vk::raii::Device &,
                const vk::raii::SurfaceKHR &,
                const vk::Extent2D &,
                const QueueFamilyIndices &,
                const vk::raii::SwapchainKHR * = nullptr);

	// Null constructor
	Swapchain(std::nullptr_t) {}
};

// Pick a surface format
inline vk::SurfaceFormatKHR pick_surface_format(const vk::raii::PhysicalDevice &phdev,
		const vk::raii::SurfaceKHR &surface)
{
	// Constant formats
	static const std::vector <vk::SurfaceFormatKHR> target_formats = {
		{ vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear },
	};

	// Get the surface formats
	std::vector <vk::SurfaceFormatKHR> formats =
			phdev.getSurfaceFormatsKHR(*surface);

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
inline vk::PresentModeKHR pick_present_mode(const vk::raii::PhysicalDevice &phdev,
		const vk::raii::SurfaceKHR &surface)
{
	// Constant modes
	static const std::vector <vk::PresentModeKHR> target_modes = {
		vk::PresentModeKHR::eMailbox,
		vk::PresentModeKHR::eImmediate,
		vk::PresentModeKHR::eFifo
	};

	// Get the present modes
	std::vector <vk::PresentModeKHR> modes =
			phdev.getSurfacePresentModesKHR(*surface);

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
	// KOBRA_LOG_FUNC(Log::ERROR) << "No supported present mode found\n";
	throw std::runtime_error("[Vulkan] No supported present mode found");
}

inline Swapchain::Swapchain(const vk::raii::PhysicalDevice &phdev,
                const vk::raii::Device &device,
                const vk::raii::SurfaceKHR &surface,
                const vk::Extent2D &extent,
                const QueueFamilyIndices &indices,
                const vk::raii::SwapchainKHR *old_swapchain)
{
        // Pick a surface format
        auto surface_format = pick_surface_format(phdev, surface);
        format = surface_format.format;

        // Surface capabilities and extent
        vk::SurfaceCapabilitiesKHR capabilities =
                        phdev.getSurfaceCapabilitiesKHR(*surface);

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
        vk::PresentModeKHR present_mode = pick_present_mode(phdev, surface);

        // Creation info
        vk::SwapchainCreateInfoKHR create_info {
                {},
                *surface,
                capabilities.minImageCount,
                format,
                surface_format.colorSpace,
                swapchain_extent,
                1,
                vk::ImageUsageFlagBits::eColorAttachment
                        | vk::ImageUsageFlagBits::eTransferSrc,
                vk::SharingMode::eExclusive,
                {},
                transform,
                composite_alpha,
                present_mode,
                true,
                (old_swapchain ? **old_swapchain : nullptr)
        };

        // In case graphics and present queues are different
        if (indices.graphics != indices.present) {
                create_info.imageSharingMode = vk::SharingMode::eConcurrent;
                create_info.queueFamilyIndexCount = 2;
                create_info.pQueueFamilyIndices = &indices.graphics;
        }

        // Create the swapchain
        swapchain = vk::raii::SwapchainKHR(device, create_info);

        // Get the swapchain images
        images = swapchain.getImages();

        // Create image views
        vk::ImageViewCreateInfo create_view_info {
                {}, {},
                vk::ImageViewType::e2D,
                format,
                {},
                vk::ImageSubresourceRange(
                        vk::ImageAspectFlagBits::eColor,
                        0, 1, 0, 1
                )
        };

        for (size_t i = 0; i < images.size(); i++) {
                create_view_info.image = images[i];
                image_views.emplace_back(device, create_view_info);
        }
}

// Generate framebuffer from swapchain, render pass and optional depth buffer
struct FramebufferSetInfo {
	Swapchain *swapchain;
	vk::RenderPass render_pass;
	vk::Extent2D extent;
	// TODO: depth buffer as well...
	// vk::raii::ImageView *depth_buffer_view = nullptr;
};

inline std::vector <vk::Framebuffer> make_framebuffers(const vk::Device &device, const FramebufferSetInfo &info)
{
	std::vector <vk::Framebuffer> framebuffers;

	for (const vk::raii::ImageView &view : info.swapchain->image_views) {
		vk::ImageView views[] = { *view };

		vk::FramebufferCreateInfo fb_info {
			{}, info.render_pass,
			1, views,
			info.extent.width, info.extent.height, 1
		};

		framebuffers.emplace_back(device.createFramebuffer(fb_info));
	}

	return framebuffers;
}

struct PresentSyncronization {
	// TODO: get rid of raii things...
        std::vector <vk::raii::Semaphore> image_available;
        std::vector <vk::raii::Semaphore> render_finished;
        std::vector <vk::raii::Fence> in_flight;

        PresentSyncronization(const vk::raii::Device &device, uint32_t frames_in_flight) {
                // Default semaphores
                vk::SemaphoreCreateInfo semaphore_info {};

                // Signaled fences
                vk::FenceCreateInfo fence_info {};
                fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

                for (uint32_t i = 0; i < frames_in_flight; i++) {
                        image_available.push_back(device.createSemaphore(semaphore_info));
                        render_finished.push_back(device.createSemaphore(semaphore_info));
                        in_flight.push_back(device.createFence(fence_info));
                }
        }
};

struct SurfaceOperation {
        enum {
                eOk,
                eResize,
                eFailed
        } status;

        uint32_t index;
};

inline SurfaceOperation acquire_image(const vk::raii::Device &device,
                const vk::raii::SwapchainKHR &swapchain,
                const PresentSyncronization &sync,
                uint32_t frame)
{
        // Wait for previous frame to finish
        device.waitForFences(*sync.in_flight[frame], VK_TRUE, UINT64_MAX);

        // Acquire image
        auto [result, image_index] = swapchain.acquireNextImage(UINT64_MAX, *sync.image_available[frame], nullptr);
        if (result == vk::Result::eErrorOutOfDateKHR) {
                std::cerr << "Swapchain out of date" << std::endl;
                return { SurfaceOperation::eResize, 0 };
        } else if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
                std::cerr << "Failed to acquire swapchain image" << std::endl;
                return { SurfaceOperation::eFailed, 0 };
        }

        // Reset fence to prepare for next frame
        device.resetFences(*sync.in_flight[frame]);

        return { SurfaceOperation::eOk, image_index };
}

inline SurfaceOperation present_image(const vk::raii::Queue &queue,
                const vk::raii::SwapchainKHR &swapchain,
                const PresentSyncronization &sync,
                uint32_t index)
{
        vk::PresentInfoKHR present_info {
                *sync.render_finished[index],
                *swapchain,
                index
        };

        try {
		// TODO: check return value here
                queue.presentKHR(present_info);
        } catch (vk::OutOfDateKHRError &e) {
                std::cerr << "Swapchain out of date" << std::endl;
                return { SurfaceOperation::eResize, 0 };
        }

        return { SurfaceOperation::eOk, 0 };
}

// Get all available physical devices
// TODO: unnecessary?
inline vk::raii::PhysicalDevices get_physical_devices()
{
	return vk::raii::PhysicalDevices { detail::get_vulkan_instance() };
}

// Check if a physical device supports a set of extensions
inline bool physical_device_able(const vk::raii::PhysicalDevice &phdev,
		const std::vector <const char *> &extensions)
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
			// KOBRA_LOG_FUNC(Log::WARN) << "Extension \"" << extension
					// << "\" is not supported\n";
			return false;
		}
	}

	return true;
}

// Pick physical device according to some criteria
inline vk::raii::PhysicalDevice pick_physical_device
	(const std::function <bool (const vk::raii::PhysicalDevice &)> &predicate)
{
	// Get all the physical devices
	vk::raii::PhysicalDevices devices = get_physical_devices();

	// Find the first one that satisfies the predicate
	for (const vk::raii::PhysicalDevice &device : devices) {
		if (predicate(device))
			return device;
	}

	// If none found, throw an error
	// KOBRA_LOG_FUNC(Log::ERROR) << "No physical device found\n";
	throw std::runtime_error("[Vulkan] No physical device found");
}

struct ApplicationSkeleton {
        vk::raii::Device device = nullptr;
        vk::raii::PhysicalDevice phdev = nullptr;
        vk::raii::SurfaceKHR surface = nullptr;

        vk::raii::Queue graphics_queue = nullptr;
        vk::raii::Queue present_queue = nullptr;

        Swapchain swapchain = nullptr;
        Window *window = nullptr;
};

// Create logical device on an arbitrary queue
inline vk::raii::Device make_device(const vk::raii::PhysicalDevice &phdev,
		const uint32_t queue_family,
		const uint32_t queue_count,
		const std::vector <const char *> &extensions)
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

	// Create the device
	vk::DeviceCreateInfo device_info {
		vk::DeviceCreateFlags(), queue_info,
		{}, extensions, &device_features, nullptr
	};

	return vk::raii::Device {
		phdev, device_info
	};
}

// Create a logical device
inline vk::raii::Device make_device(const vk::raii::PhysicalDevice &phdev,
		const QueueFamilyIndices &indices,
		const std::vector <const char *> &extensions)
{
	auto families = phdev.getQueueFamilyProperties();
	uint32_t count = families[indices.graphics].queueCount;
	return make_device(phdev, indices.graphics, count, extensions);
}

inline void make_application(ApplicationSkeleton *app,
                const vk::raii::PhysicalDevice &phdev,
                const vk::Extent2D &extent,
                const std::string &title)
{
        // Extensions for the application
        static const std::vector <const char *> device_extensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };

        app->phdev = phdev;
        app->window = make_window(extent, title);
        app->surface = make_surface(*app->window);

        QueueFamilyIndices queue_family = find_queue_families(phdev, app->surface);
        app->device = make_device(phdev, queue_family, device_extensions);
	app->swapchain = Swapchain {
                phdev, app->device, app->surface,
                app->window->extent, queue_family
        };

        app->graphics_queue = vk::raii::Queue { app->device, queue_family.graphics, 0 };
        app->present_queue = vk::raii::Queue { app->device, queue_family.present, 0 };
}

inline void destroy_application(ApplicationSkeleton *app)
{
        destroy_window(app->window);
}

// Vulkan buffer wrapper
struct Buffer {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        vk::MemoryRequirements requirements;
};

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
		// KOBRA_LOG_FUNC(Log::ERROR) << "No memory type found\n";
		throw std::runtime_error("[Vulkan] No memory type found");
	}

	return type_index;
}

// TODO: move to source file
inline Buffer make_buffer(const vk::Device &device, size_t size, const vk::PhysicalDeviceMemoryProperties &properties)
{
	// TODO: usage flags as well...
        Buffer buffer;

        vk::BufferCreateInfo buffer_info {
                {}, size,
                vk::BufferUsageFlagBits::eTransferSrc
			| vk::BufferUsageFlagBits::eVertexBuffer,
                vk::SharingMode::eExclusive, 0, nullptr
        };

        buffer.buffer = device.createBuffer(buffer_info);

        buffer.requirements = device.getBufferMemoryRequirements(buffer.buffer);
        vk::MemoryAllocateInfo buffer_alloc_info {
                buffer.requirements.size, find_memory_type(
                        properties, buffer.requirements.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eHostVisible
                                | vk::MemoryPropertyFlagBits::eHostCoherent
                )
        };

        buffer.memory = device.allocateMemory(buffer_alloc_info);
        device.bindBufferMemory(buffer.buffer, buffer.memory, 0);

        return buffer;
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
        std::cout << "MAPPED TO ADDRESS: " << mapped << "\n";
        std::cout << "Size of vec: " << vec.size() << ", transfer size: " << size << "\n";
        std::memcpy(mapped, vec.data(), size);
        device.unmapMemory(buffer.memory);

        // Warn if fewer elements were transferred
        // TODO: or return some kind of error code?
        if (size < vec.size() * sizeof(T)) {
		std::cout << "Fewer elements were transferred than"
			<< " may have been expected: "
			<< size << "/" << vec.size() * sizeof(T)
			<< " bytes were transferred\n";
        }
}

inline void destroy_buffer(const vk::Device &device, const Buffer &buffer)
{
        device.destroyBuffer(buffer.buffer);
        device.freeMemory(buffer.memory);
}

// Vulkan image wrapper
struct Image {
        vk::Image image;
        vk::ImageView view;
        vk::DeviceMemory memory;
        vk::MemoryRequirements requirements;
};

struct ImageCreateInfo {
        uint32_t width;
        uint32_t height;
        vk::Format format;
        vk::ImageUsageFlags usage;
};

inline Image make_image(const vk::Device &device, const ImageCreateInfo &info, const vk::PhysicalDeviceMemoryProperties &properties)
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

        image.image = device.createImage(image_info);
        image.requirements = device.getImageMemoryRequirements(image.image);

        vk::MemoryAllocateInfo alloc_info {
                image.requirements.size, find_memory_type(
                        properties, image.requirements.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eDeviceLocal
                )
        };

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
                        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1
                }
        };

        image.view = device.createImageView(view_info);

        return image;
}

inline void transition_image_layout(const vk::CommandBuffer &cmd,
		const Image &image,
		const vk::ImageLayout old_layout,
		const vk::ImageLayout new_layout)
{
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
		// KOBRA_ASSERT(false, "Unsupported old layout " + vk::to_string(old_layout));
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
		// KOBRA_ASSERT(false, "Unsupported old layout " + vk::to_string(old_layout));
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
		// KOBRA_ASSERT(false, "Unsupported new layout " + vk::to_string(new_layout));
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
		// KOBRA_ASSERT(false, "Unsupported new layout " + vk::to_string(new_layout));
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

	vk::ImageMemoryBarrier barrier {
		src_access_mask, dst_access_mask,
		old_layout, new_layout,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		image.image, image_subresource_range
	};

	// Add the barrier
	return cmd.pipelineBarrier(source_stage, destination_stage, {}, {}, {}, barrier);
}

inline void destroy_image(const vk::Device &device, const Image &image)
{
        device.destroyImageView(image.view);
        device.destroyImage(image.image);
        device.freeMemory(image.memory);
}

namespace shader {

// TODO: detail here as well...
using Defines = std::map <std::string, std::string>;
using Includes = std::set <std::string>;

inline const std::string read_file(const std::filesystem::path &path)
{
	std::ifstream f(path);
	if (!f.good()) {
		printf("Could not open file: %s\n", path.c_str());
		return "";
	}

	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

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
	case vk::ShaderStageFlagBits::eTaskNV:
		return EShLangTaskNV;
	case vk::ShaderStageFlagBits::eMeshNV:
		return EShLangMeshNV;
	default:
		break;
	}

	// KOBRA_LOG_FUNC(Log::ERROR) << "Unknown shader stage: "
	// 	<< vk::to_string(stage) << std::endl;

	return EShLangVertex;
}

static std::string preprocess(const std::string &source,
		const std::map <std::string, std::string> &defines,
                const std::set <std::string> &paths)
{
	// Defines contains string values to be relpaced
	// e.g. if {"VERSION", "450"} is in defines, then
	// "${VERSION}" will be replaced with "450"

	std::string out = "";
	std::string line;

	std::istringstream stream(source);
	while (std::getline(stream, line)) {
		// Check if line is an include but not commented out
		if (line.find("#include") != std::string::npos &&
				line.find("//") == std::string::npos) {
			// Get the include path
			std::regex regex("#include \"(.*)\"");
			std::smatch match;
			std::regex_search(line, match, regex);

			// Check that the regex matched
			if (match.size() != 2) {
				// KOBRA_LOG_FUNC(Log::ERROR)
				// 	<< "Failed to match regex for include: "
				// 	<< line << std::endl;
				continue;
			}

			// Read the file
                        std::string source = "";

                        for (const std::string &dir : paths) {
                                std::filesystem::path path = dir;
                                path /= match[1].str();

                                source = read_file(path);
                                if (source != "") {
                                        break;
                                }
                        }

                        if (source == "") {
                                // KOBRA_LOG_FUNC(Log::ERROR)
                                //         << "Failed to locate included file: "
                                //         << match[1].str() << std::endl;
                                continue;
                        }

			// std::string path = KOBRA_SHADER_INCLUDE_DIR + match[1].str();
			// std::string source = common::read_file(path);

                        // TODO: add self's directory to paths

			// Replace the include with the file contents
			out += preprocess(source, defines, paths);

			// TODO: allow simoultaneous features
			// e.g. add the includes file lines into the stream...
		} else if (line.find("${") != std::string::npos) {
			// Replace the define
			for (auto &define : defines) {
				std::string key = "${" + define.first + "}";
				std::string value = define.second;

				// Replace all instances of the key with the value
				size_t pos = 0;
				while ((pos = line.find(key, pos)) != std::string::npos) {
					line.replace(pos, key.length(), value);
					pos += value.length();
				}
			}

			out += line + "\n";
		} else {
			out += line + "\n";
		}
	}

	return out;
}

static _compile_out glsl_to_spriv(const std::string &source,
		const std::map <std::string, std::string> &defines,
                const std::set <std::string> &paths,
		const vk::ShaderStageFlagBits &shader_type)
{
        // Get possible include paths
        std::set <std::string> include_paths = paths;

        // Get the directory of the source file
	std::string source_copy = preprocess(source, defines, include_paths);

	// Output
	_compile_out out;

	// Compile shader
	EShLanguage stage = translate_shader_stage(shader_type);

	const char *shaderStrings[1];
	shaderStrings[0] = source_copy.data();

	glslang::TShader shader(stage);
	shader.setStrings(shaderStrings, 1);

	// Enable SPIR-V and Vulkan rules when parsing GLSL
	EShMessages messages = (EShMessages) (EShMsgSpvRules | EShMsgVulkanRules);

	// ShaderIncluder includer;
	if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
		out.log = shader.getInfoLog();
		out.source = source_copy;
		return out;
	}

	// Link the program
	glslang::TProgram program;
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

// Compile shader
inline std::optional <vk::ShaderModule> compile(const vk::Device &device,
		const std::string &source,
		const vk::ShaderStageFlagBits &shader_type,
		const Defines &defines = {},
		const Includes &includes = {})
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	_compile_out out = glsl_to_spriv(source, defines, includes, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		std::cerr << "Shader compilation failed:\n" << out.log
			<< "\nSource:\n" << fmt_lines(out.source) << "\n";

		return std::nullopt;
	}

        vk::ShaderModuleCreateInfo create_info(
                vk::ShaderModuleCreateFlags(),
                out.spirv.size() * sizeof(uint32_t),
                out.spirv.data()
        );

        return device.createShaderModule(create_info);
}

inline std::optional <vk::ShaderModule> compile(const vk::Device &device,
		const std::filesystem::path &path,
		const vk::ShaderStageFlagBits &shader_type,
		const Defines &defines = {},
		const Includes &includes = {})
{
	// Check that file exists
	glslang::InitializeProcess();

	// Compile shader
	std::string source = read_file(path);
	_compile_out out = glsl_to_spriv(source, defines, includes, shader_type);
	if (!out.log.empty()) {
		// TODO: show the errornous line(s)
		std::cerr << "Shader compilation failed:\n" << out.log
			<< "\nSource:\n" << fmt_lines(out.source) << "\n";

		return std::nullopt;
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

struct GraphicsCreateInfo {
	vk::VertexInputBindingDescription vertex_binding;
	vk::ArrayProxy <vk::VertexInputAttributeDescription> vertex_attributes;

	vk::ShaderModule vertex_shader;
	vk::ShaderModule fragment_shader;

	bool dynamic_viewport = false;
	vk::Extent2D extent;

	vk::PipelineLayout pipeline_layout;
	vk::RenderPass render_pass;
};

// TODO: remove as much raii as possible
inline std::optional <vk::Pipeline> create(const vk::Device &device, const GraphicsCreateInfo &info)
{
	vk::PipelineShaderStageCreateInfo shader_stages[] = {
		vk::PipelineShaderStageCreateInfo {
			{}, vk::ShaderStageFlagBits::eVertex, info.vertex_shader, "main"
		},
		vk::PipelineShaderStageCreateInfo {
			{}, vk::ShaderStageFlagBits::eFragment, info.fragment_shader, "main"
		}
	};

	vk::PipelineVertexInputStateCreateInfo vertex_input_info {
		{}, info.vertex_binding, info.vertex_attributes
	};

	vk::PipelineInputAssemblyStateCreateInfo input_assembly {
		{}, vk::PrimitiveTopology::eTriangleList
	};

	// TODO: dynamic state options
	assert(!info.dynamic_viewport); // NOTE: temporary
	vk::Viewport viewport {
		0.0f, 0.0f,
		(float) info.extent.width, (float) info.extent.height,
		0.0f, 1.0f
	};

	vk::Rect2D scissor { {}, info.extent };
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

	return device.createGraphicsPipeline(nullptr,
		vk::GraphicsPipelineCreateInfo {
			{}, shader_stages,
			&vertex_input_info, &input_assembly,
			nullptr, &viewport_state, &rasterizer,
			&multisampling, nullptr, &color_blending,
			nullptr, info.pipeline_layout, info.render_pass,
			0, nullptr, -1
		}
	).value;
}

struct ComputeCreateInfo {};

} // namespace pipeline

}
