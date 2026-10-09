#include "stpch.h"
#include "Strata/Renderer/GraphicsDevice.h"

#include "Platform/Vulkan/VulkanLoader.h"
#include "Strata/Core/Window.h"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// NVRHI is linked as a static library, which leaves defining vulkan.hpp's dynamic dispatcher to the application.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <atomic>
#include <queue>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MinimumVulkanVersion = VK_API_VERSION_1_2;
		constexpr const char* c_ValidationLayerName = "VK_LAYER_KHRONOS_validation";

		std::string FormatVulkanVersion(uint32_t version)
		{
			return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
		}

		std::string FormatDriverVersion(uint32_t vendorId, uint32_t version)
		{
			if (vendorId == 0x10DE) // NVIDIA
				return fmt::format("{}.{}.{}", (version >> 22) & 0x3FF, (version >> 14) & 0xFF, (version >> 6) & 0xFF);
#if defined(ST_PLATFORM_WINDOWS)
			if (vendorId == 0x8086) // Intel on Windows
				return fmt::format("{}.{}", version >> 14, version & 0x3FFF);
#endif
			return FormatVulkanVersion(version);
		}

		bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
		{
			for (const VkExtensionProperties& extension : extensions)
			{
				if (std::strcmp(extension.extensionName, name) == 0)
					return true;
			}
			return false;
		}

		class NvrhiMessageCallback final : public nvrhi::IMessageCallback
		{
		public:
			void message(nvrhi::MessageSeverity severity, const char* messageText) override
			{
				switch (severity)
				{
					case nvrhi::MessageSeverity::Info:
						ST_CORE_TRACE("NVRHI: {}", messageText);
						break;
					case nvrhi::MessageSeverity::Warning:
						ST_CORE_WARN("NVRHI: {}", messageText);
						break;
					case nvrhi::MessageSeverity::Error:
					case nvrhi::MessageSeverity::Fatal:
						ST_CORE_ERROR("NVRHI: {}", messageText);
						ErrorCount.fetch_add(1);
						break;
				}
			}

			std::atomic<uint32_t> ErrorCount = 0;
		};

		VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
		{
			const char* message = callbackData && callbackData->pMessage ? callbackData->pMessage : "(no message)";
			// The loader reports its search (e.g. missing registry keys for layer manifests) as warnings; keep them out
			// of the way unless tracing.
			const bool loaderMessage = callbackData && callbackData->pMessageIdName && std::strcmp(callbackData->pMessageIdName, "Loader Message") == 0;
			if (loaderMessage && !(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
			{
				ST_CORE_TRACE("Vulkan loader: {}", message);
				return VK_FALSE;
			}

			if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			{
				ST_CORE_ERROR("Vulkan: {}", message);
				static_cast<std::atomic<uint32_t>*>(userData)->fetch_add(1);
			}
			else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
			{
				ST_CORE_WARN("Vulkan: {}", message);
			}
			return VK_FALSE;
		}

		nvrhi::Format ConvertSwapchainFormat(VkFormat format)
		{
			switch (format)
			{
				case VK_FORMAT_B8G8R8A8_UNORM: return nvrhi::Format::BGRA8_UNORM;
				case VK_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
				case VK_FORMAT_B8G8R8A8_SRGB:  return nvrhi::Format::SBGRA8_UNORM;
				case VK_FORMAT_R8G8B8A8_SRGB:  return nvrhi::Format::SRGBA8_UNORM;
				default:                       return nvrhi::Format::UNKNOWN;
			}
		}

	}

	class VulkanGraphicsDevice final : public GraphicsDevice
	{
	public:
		explicit VulkanGraphicsDevice(const GraphicsDeviceSpecification& specification)
			: m_Specification(specification)
		{
			m_Specification.MaxFramesInFlight = std::clamp(m_Specification.MaxFramesInFlight, 1u, 4u);
		}

		~VulkanGraphicsDevice() override
		{
			Shutdown();
		}

		bool Initialize()
		{
			if (!LoadVulkanLibrary() || !CreateInstance() || !SelectPhysicalDevice() || !CreateLogicalDevice())
				return false;

			m_BarrierCommandList = m_NvrhiDevice->createCommandList();
			ST_CORE_INFO("Graphics device: {} (Vulkan {}, driver {}){}", m_Info.AdapterName, m_Info.APIVersion, m_Info.DriverVersion,
				m_Info.ValidationEnabled ? ", validation enabled" : "");
			return true;
		}

		nvrhi::IDevice* GetDevice() const override { return m_NvrhiDevice.Get(); }
		const GraphicsDeviceInfo& GetInfo() const override { return m_Info; }

		GraphicsMemoryBudget GetMemoryBudget() const override
		{
			GraphicsMemoryBudget result;
			const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
			if (!m_HasMemoryBudget || !dispatch.vkGetPhysicalDeviceMemoryProperties2)
			{
				result.Budget = m_Info.DedicatedVideoMemory;
				return result;
			}

			VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
			VkPhysicalDeviceMemoryProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
			properties.pNext = &budget;
			dispatch.vkGetPhysicalDeviceMemoryProperties2(m_PhysicalDevice, &properties);
			for (uint32_t heap = 0; heap < properties.memoryProperties.memoryHeapCount; heap++)
			{
				if (properties.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
				{
					result.Budget += budget.heapBudget[heap];
					result.Usage += budget.heapUsage[heap];
				}
			}
			return result;
		}

		bool CreateSwapchain(Window& window, bool vsync) override;
		void DestroySwapchain() override;
		bool HasSwapchain() const override { return m_Window != nullptr; }

		void SetVSync(bool vsync) override
		{
			if (vsync != m_VSync)
			{
				m_VSync = vsync;
				m_SwapchainDirty = true;
			}
		}

		bool IsVSync() const override { return m_VSync; }

		nvrhi::ITexture* GetBackBuffer() const override
		{
			return m_ImageAcquired ? m_SwapchainImages[m_CurrentImageIndex].Texture.Get() : nullptr;
		}

		nvrhi::IFramebuffer* GetBackBufferFramebuffer() const override
		{
			return m_ImageAcquired ? m_SwapchainImages[m_CurrentImageIndex].Framebuffer.Get() : nullptr;
		}

		nvrhi::Format GetBackBufferFormat() const override { return ConvertSwapchainFormat(m_SwapchainFormat); }
		glm::uvec2 GetBackBufferSize() const override { return glm::uvec2(m_SwapchainExtent.width, m_SwapchainExtent.height); }

		bool BeginFrame() override;
		void EndFrame() override;
		uint64_t GetFrameIndex() const override { return m_FrameIndex; }
		uint32_t GetMaxFramesInFlight() const override { return m_Specification.MaxFramesInFlight; }

		void WaitForIdle() override
		{
			if (m_NvrhiDevice)
				m_NvrhiDevice->waitForIdle();
		}

		uint32_t GetErrorCount() const override
		{
			return m_MessageCallback.ErrorCount.load() + m_VulkanErrorCount.load();
		}

		bool IsDeviceLost() const override { return m_DeviceLost; }
	private:
		bool LoadVulkanLibrary();
		bool CreateInstance();
		bool SelectPhysicalDevice();
		bool CreateLogicalDevice();
		bool CreateSurface();
		void DestroySurface();
		bool CreateSwapchainResources();
		void DestroySwapchainResources();
		bool RecreateSwapchainResources();
		void InitializeAcquiredImage();
		void OnDeviceLost(const char* operation);
		void Shutdown();
	private:
		GraphicsDeviceSpecification m_Specification;
		GraphicsDeviceInfo m_Info;
		NvrhiMessageCallback m_MessageCallback;
		std::atomic<uint32_t> m_VulkanErrorCount = 0;

		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		uint32_t m_InstanceApiVersion = 0; // Highest version the loader and the engine both support
		uint32_t m_ApiVersion = 0;         // Version used with the selected device
		int32_t m_GraphicsFamily = -1;
		int32_t m_ComputeFamily = -1;
		int32_t m_TransferFamily = -1;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		VkQueue m_ComputeQueue = VK_NULL_HANDLE;
		VkQueue m_TransferQueue = VK_NULL_HANDLE;
		std::vector<std::string> m_InstanceExtensions;
		std::vector<std::string> m_DeviceExtensions;
		bool m_HasDebugUtils = false;
		bool m_HasMemoryBudget = false;

		nvrhi::vulkan::DeviceHandle m_NvrhiVulkanDevice;
		nvrhi::DeviceHandle m_NvrhiDevice; // Validation-wrapped when validation is enabled
		nvrhi::CommandListHandle m_BarrierCommandList;

		struct SwapchainImage
		{
			VkImage Image = VK_NULL_HANDLE;
			nvrhi::TextureHandle Texture;
			nvrhi::FramebufferHandle Framebuffer;
			bool Initialized = false;
		};

		Window* m_Window = nullptr;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		VkFormat m_SwapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D m_SwapchainExtent = {};
		bool m_VSync = true;
		bool m_SwapchainDirty = false;
		std::vector<SwapchainImage> m_SwapchainImages;
		std::vector<VkSemaphore> m_AcquireSemaphores;
		std::vector<VkSemaphore> m_PresentSemaphores;
		uint32_t m_AcquireSemaphoreIndex = 0;
		uint32_t m_CurrentImageIndex = 0;
		bool m_ImageAcquired = false;

		std::queue<nvrhi::EventQueryHandle> m_FramesInFlight;
		std::vector<nvrhi::EventQueryHandle> m_EventQueryPool;
		uint64_t m_FrameIndex = 0;
		bool m_DeviceLost = false;
	};

	Scope<GraphicsDevice> GraphicsDevice::Create(const GraphicsDeviceSpecification& specification)
	{
		auto device = CreateScope<VulkanGraphicsDevice>(specification);
		if (!device->Initialize())
			return nullptr;
		return device;
	}

	bool VulkanGraphicsDevice::LoadVulkanLibrary()
	{
		const PFN_vkGetInstanceProcAddr getInstanceProcAddr = VulkanLoader::GetInstanceProcAddr();
		if (!getInstanceProcAddr)
		{
			ST_CORE_ERROR("Vulkan is not available: {}. Install or update your GPU driver.", VulkanLoader::GetLoadError());
			return false;
		}

		VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
		return true;
	}

	bool VulkanGraphicsDevice::CreateInstance()
	{
		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;

		uint32_t instanceVersion = VK_API_VERSION_1_0;
		if (dispatch.vkEnumerateInstanceVersion)
			dispatch.vkEnumerateInstanceVersion(&instanceVersion);
		if (instanceVersion < c_MinimumVulkanVersion)
		{
			ST_CORE_ERROR("Vulkan {} is required, but the installed loader only supports {}", FormatVulkanVersion(c_MinimumVulkanVersion), FormatVulkanVersion(instanceVersion));
			return false;
		}
		m_InstanceApiVersion = std::min(instanceVersion, static_cast<uint32_t>(VK_API_VERSION_1_3));

		uint32_t extensionCount = 0;
		dispatch.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> availableExtensions(extensionCount);
		dispatch.vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());

		if (!m_Specification.Headless)
		{
			uint32_t glfwExtensionCount = 0;
			const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
			if (!glfwExtensions)
			{
				ST_CORE_ERROR("GLFW reports no Vulkan presentation support on this system");
				return false;
			}
			for (uint32_t index = 0; index < glfwExtensionCount; index++)
				m_InstanceExtensions.emplace_back(glfwExtensions[index]);
		}

		VkInstanceCreateFlags createFlags = 0;
		if (HasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		{
			m_InstanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			m_HasDebugUtils = true;
		}
		if (HasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
		{
			// Required to see MoltenVK (macOS) devices, harmless elsewhere.
			m_InstanceExtensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			createFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}

		std::vector<const char*> layers;
		if (m_Specification.EnableValidation)
		{
			uint32_t layerCount = 0;
			dispatch.vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> availableLayers(layerCount);
			dispatch.vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
			const bool hasValidation = std::any_of(availableLayers.begin(), availableLayers.end(), [](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, c_ValidationLayerName) == 0; });
			if (hasValidation)
				layers.push_back(c_ValidationLayerName);
			else
				ST_CORE_WARN("Vulkan validation requested but {} is not installed (install the Vulkan SDK); using NVRHI validation only", c_ValidationLayerName);
		}

		std::vector<const char*> extensionNames;
		for (const std::string& extension : m_InstanceExtensions)
			extensionNames.push_back(extension.c_str());

		VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		applicationInfo.pApplicationName = m_Specification.ApplicationName.c_str();
		applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
		applicationInfo.pEngineName = "Strata";
		applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
		applicationInfo.apiVersion = m_InstanceApiVersion;

		VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
		messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerInfo.pfnUserCallback = DebugUtilsCallback;
		messengerInfo.pUserData = &m_VulkanErrorCount;

		VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		instanceInfo.flags = createFlags;
		instanceInfo.pApplicationInfo = &applicationInfo;
		instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		instanceInfo.ppEnabledLayerNames = layers.data();
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		instanceInfo.ppEnabledExtensionNames = extensionNames.data();
		// Also report problems during instance creation and destruction.
		if (m_HasDebugUtils && !layers.empty())
			instanceInfo.pNext = &messengerInfo;

		const VkResult result = dispatch.vkCreateInstance(&instanceInfo, nullptr, &m_Instance);
		if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("vkCreateInstance failed: {}", nvrhi::vulkan::resultToString(result));
			return false;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(m_Instance));

		if (m_HasDebugUtils && dispatch.vkCreateDebugUtilsMessengerEXT)
			dispatch.vkCreateDebugUtilsMessengerEXT(m_Instance, &messengerInfo, nullptr, &m_DebugMessenger);

		m_Info.ValidationEnabled = m_Specification.EnableValidation;
		return true;
	}

	bool VulkanGraphicsDevice::SelectPhysicalDevice()
	{
		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;

		uint32_t deviceCount = 0;
		dispatch.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
		std::vector<VkPhysicalDevice> devices(deviceCount);
		dispatch.vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());
		if (devices.empty())
		{
			ST_CORE_ERROR("No Vulkan-capable GPU found");
			return false;
		}

		int64_t bestScore = -1;
		for (uint32_t deviceIndex = 0; deviceIndex < deviceCount; deviceIndex++)
		{
			const VkPhysicalDevice physicalDevice = devices[deviceIndex];
			if (m_Specification.AdapterIndex >= 0 && static_cast<uint32_t>(m_Specification.AdapterIndex) != deviceIndex)
				continue;

			VkPhysicalDeviceProperties properties;
			dispatch.vkGetPhysicalDeviceProperties(physicalDevice, &properties);
			auto reject = [&](const std::string& reason)
			{
				ST_CORE_INFO("GPU '{}' skipped: {}", properties.deviceName, reason);
			};

			if (properties.apiVersion < c_MinimumVulkanVersion)
			{
				reject(fmt::format("supports Vulkan {} (1.2 required)", FormatVulkanVersion(properties.apiVersion)));
				continue;
			}

			uint32_t extensionCount = 0;
			dispatch.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> extensions(extensionCount);
			dispatch.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data());

			if (!m_Specification.Headless && !HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			{
				reject("no swapchain support");
				continue;
			}

			// Required features: dynamic rendering (NVRHI render passes), synchronization2 (NVRHI barriers), timeline
			// semaphores (NVRHI queues) and descriptor indexing (bindless textures). Vulkan 1.2 devices provide the first
			// two through extensions.
			const uint32_t deviceApiVersion = std::min(m_InstanceApiVersion, properties.apiVersion);
			const bool vulkan13 = deviceApiVersion >= VK_API_VERSION_1_3;
			VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
			VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
			VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR };
			VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
			VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
			features.pNext = &features12;
			if (vulkan13)
			{
				features12.pNext = &features13;
			}
			else
			{
				features12.pNext = &dynamicRendering;
				dynamicRendering.pNext = &synchronization2;
			}
			dispatch.vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

			const bool hasDynamicRendering = vulkan13 ? features13.dynamicRendering == VK_TRUE
				: (HasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) && dynamicRendering.dynamicRendering == VK_TRUE);
			if (!hasDynamicRendering)
			{
				reject("no dynamic rendering support");
				continue;
			}
			const bool hasSynchronization2 = vulkan13 ? features13.synchronization2 == VK_TRUE
				: (HasExtension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) && synchronization2.synchronization2 == VK_TRUE);
			if (!hasSynchronization2)
			{
				reject("no synchronization2 support");
				continue;
			}
			if (!features12.timelineSemaphore || !features12.descriptorIndexing || !features12.runtimeDescriptorArray
				|| !features12.descriptorBindingPartiallyBound || !features12.shaderSampledImageArrayNonUniformIndexing
				|| !features12.descriptorBindingSampledImageUpdateAfterBind || !features12.descriptorBindingUpdateUnusedWhilePending)
			{
				reject("missing timeline semaphores or descriptor indexing features");
				continue;
			}
			if (!features.features.samplerAnisotropy)
			{
				reject("no anisotropic filtering");
				continue;
			}

			uint32_t familyCount = 0;
			dispatch.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			dispatch.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

			int32_t graphicsFamily = -1;
			for (uint32_t family = 0; family < familyCount; family++)
			{
				if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
					continue;
				if (!m_Specification.Headless && !glfwGetPhysicalDevicePresentationSupport(m_Instance, physicalDevice, family))
					continue;
				graphicsFamily = static_cast<int32_t>(family);
				break;
			}
			if (graphicsFamily < 0)
			{
				reject("no graphics queue with presentation support");
				continue;
			}

			VkPhysicalDeviceMemoryProperties memoryProperties;
			dispatch.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
			uint64_t dedicatedMemory = 0;
			for (uint32_t heap = 0; heap < memoryProperties.memoryHeapCount; heap++)
			{
				if (memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
					dedicatedMemory += memoryProperties.memoryHeaps[heap].size;
			}

			int64_t score = static_cast<int64_t>(dedicatedMemory >> 20);
			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
				score += 1'000'000;
			else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
				score += 100'000;

			if (score <= bestScore)
				continue;

			bestScore = score;
			m_PhysicalDevice = physicalDevice;
			m_GraphicsFamily = graphicsFamily;
			m_Info.AdapterName = properties.deviceName;
			m_Info.DriverVersion = FormatDriverVersion(properties.vendorID, properties.driverVersion);
			m_Info.DedicatedVideoMemory = dedicatedMemory;
			m_Info.IsDiscreteGPU = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
			m_Info.SupportsBCCompression = features.features.textureCompressionBC == VK_TRUE;
			m_Info.SupportsWireframe = features.features.fillModeNonSolid == VK_TRUE;
			m_ApiVersion = deviceApiVersion;

			// Dedicated async compute and transfer families, when the GPU has them.
			m_ComputeFamily = -1;
			m_TransferFamily = -1;
			for (uint32_t family = 0; family < familyCount; family++)
			{
				const VkQueueFlags flags = families[family].queueFlags;
				if (m_ComputeFamily < 0 && (flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT))
					m_ComputeFamily = static_cast<int32_t>(family);
				if (m_TransferFamily < 0 && (flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)))
					m_TransferFamily = static_cast<int32_t>(family);
			}
		}

		if (m_PhysicalDevice == VK_NULL_HANDLE)
		{
			ST_CORE_ERROR("No GPU meets Strata's requirements (Vulkan 1.2 with dynamic rendering, synchronization2 and descriptor indexing)");
			return false;
		}

		m_Info.APIVersion = FormatVulkanVersion(m_ApiVersion);
		return true;
	}

	bool VulkanGraphicsDevice::CreateLogicalDevice()
	{
		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;

		uint32_t extensionCount = 0;
		dispatch.vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> availableExtensions(extensionCount);
		dispatch.vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, availableExtensions.data());

		const bool vulkan13 = m_ApiVersion >= VK_API_VERSION_1_3;
		if (!m_Specification.Headless)
			m_DeviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (!vulkan13)
		{
			m_DeviceExtensions.emplace_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
			m_DeviceExtensions.emplace_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
		}
		if (HasExtension(availableExtensions, "VK_KHR_portability_subset"))
			m_DeviceExtensions.emplace_back("VK_KHR_portability_subset"); // Required whenever the device exposes it (MoltenVK)
		if (HasExtension(availableExtensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
		{
			m_DeviceExtensions.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
			m_HasMemoryBudget = true;
		}

		// Query what is supported, then enable the required set plus every useful optional feature that exists.
		VkPhysicalDeviceVulkan13Features supported13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan12Features supported12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceFeatures2 supported = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		supported.pNext = &supported12;
		if (vulkan13)
			supported12.pNext = &supported13;
		dispatch.vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &supported);

		VkPhysicalDeviceFeatures2 enabled = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		const VkPhysicalDeviceFeatures& core = supported.features;
		enabled.features.samplerAnisotropy = VK_TRUE;
		enabled.features.fillModeNonSolid = core.fillModeNonSolid;
		enabled.features.depthClamp = core.depthClamp;
		enabled.features.depthBiasClamp = core.depthBiasClamp;
		enabled.features.independentBlend = core.independentBlend;
		enabled.features.imageCubeArray = core.imageCubeArray;
		enabled.features.textureCompressionBC = core.textureCompressionBC;
		enabled.features.multiDrawIndirect = core.multiDrawIndirect;
		enabled.features.drawIndirectFirstInstance = core.drawIndirectFirstInstance;
		enabled.features.shaderImageGatherExtended = core.shaderImageGatherExtended;
		enabled.features.fragmentStoresAndAtomics = core.fragmentStoresAndAtomics;
		enabled.features.shaderStorageImageReadWithoutFormat = core.shaderStorageImageReadWithoutFormat;
		enabled.features.shaderStorageImageWriteWithoutFormat = core.shaderStorageImageWriteWithoutFormat;
		enabled.features.shaderInt16 = core.shaderInt16;

		VkPhysicalDeviceVulkan12Features enabled12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		enabled12.timelineSemaphore = VK_TRUE;
		enabled12.descriptorIndexing = VK_TRUE;
		enabled12.runtimeDescriptorArray = VK_TRUE;
		enabled12.descriptorBindingPartiallyBound = VK_TRUE;
		enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		enabled12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
		enabled12.descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount;
		// Bindless layouts may only contain descriptor types whose update-after-bind feature is enabled here. Sampled
		// images are required; the others are enabled wherever the device supports them.
		enabled12.descriptorBindingStorageBufferUpdateAfterBind = supported12.descriptorBindingStorageBufferUpdateAfterBind;
		enabled12.descriptorBindingStorageImageUpdateAfterBind = supported12.descriptorBindingStorageImageUpdateAfterBind;
		enabled12.descriptorBindingUniformTexelBufferUpdateAfterBind = supported12.descriptorBindingUniformTexelBufferUpdateAfterBind;
		enabled12.descriptorBindingStorageTexelBufferUpdateAfterBind = supported12.descriptorBindingStorageTexelBufferUpdateAfterBind;
		enabled12.descriptorBindingUniformBufferUpdateAfterBind = supported12.descriptorBindingUniformBufferUpdateAfterBind;
		enabled12.shaderStorageBufferArrayNonUniformIndexing = supported12.shaderStorageBufferArrayNonUniformIndexing;
		enabled12.scalarBlockLayout = supported12.scalarBlockLayout;
		enabled12.samplerFilterMinmax = supported12.samplerFilterMinmax;
		enabled12.hostQueryReset = supported12.hostQueryReset;
		enabled12.shaderFloat16 = supported12.shaderFloat16;
		enabled12.drawIndirectCount = supported12.drawIndirectCount;
		enabled12.separateDepthStencilLayouts = supported12.separateDepthStencilLayouts;
		enabled.pNext = &enabled12;

		VkPhysicalDeviceVulkan13Features enabled13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
		VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR };
		if (vulkan13)
		{
			enabled13.dynamicRendering = VK_TRUE;
			enabled13.synchronization2 = VK_TRUE;
			enabled13.maintenance4 = supported13.maintenance4;
			enabled12.pNext = &enabled13;
		}
		else
		{
			dynamicRendering.dynamicRendering = VK_TRUE;
			synchronization2.synchronization2 = VK_TRUE;
			dynamicRendering.pNext = &synchronization2;
			enabled12.pNext = &dynamicRendering;
		}

		const float queuePriority = 1.0f;
		std::vector<VkDeviceQueueCreateInfo> queueInfos;
		for (const int32_t family : { m_GraphicsFamily, m_ComputeFamily, m_TransferFamily })
		{
			if (family < 0)
				continue;
			VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
			queueInfo.queueFamilyIndex = static_cast<uint32_t>(family);
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &queuePriority;
			queueInfos.push_back(queueInfo);
		}

		std::vector<const char*> extensionNames;
		for (const std::string& extension : m_DeviceExtensions)
			extensionNames.push_back(extension.c_str());

		VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInfo.pNext = &enabled;
		deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
		deviceInfo.pQueueCreateInfos = queueInfos.data();
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		deviceInfo.ppEnabledExtensionNames = extensionNames.data();

		const VkResult result = dispatch.vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device);
		if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("vkCreateDevice failed: {}", nvrhi::vulkan::resultToString(result));
			return false;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(m_Device));

		dispatch.vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_GraphicsFamily), 0, &m_GraphicsQueue);
		if (m_ComputeFamily >= 0)
			dispatch.vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_ComputeFamily), 0, &m_ComputeQueue);
		if (m_TransferFamily >= 0)
			dispatch.vkGetDeviceQueue(m_Device, static_cast<uint32_t>(m_TransferFamily), 0, &m_TransferQueue);

		std::vector<const char*> instanceExtensionNames;
		for (const std::string& extension : m_InstanceExtensions)
			instanceExtensionNames.push_back(extension.c_str());

		nvrhi::vulkan::DeviceDesc nvrhiDesc;
		nvrhiDesc.errorCB = &m_MessageCallback;
		nvrhiDesc.instance = m_Instance;
		nvrhiDesc.physicalDevice = m_PhysicalDevice;
		nvrhiDesc.device = m_Device;
		nvrhiDesc.graphicsQueue = m_GraphicsQueue;
		nvrhiDesc.graphicsQueueIndex = m_GraphicsFamily;
		nvrhiDesc.computeQueue = m_ComputeQueue;
		nvrhiDesc.computeQueueIndex = m_ComputeFamily;
		nvrhiDesc.transferQueue = m_TransferQueue;
		nvrhiDesc.transferQueueIndex = m_TransferFamily;
		nvrhiDesc.instanceExtensions = instanceExtensionNames.data();
		nvrhiDesc.numInstanceExtensions = instanceExtensionNames.size();
		nvrhiDesc.deviceExtensions = extensionNames.data();
		nvrhiDesc.numDeviceExtensions = extensionNames.size();
		nvrhiDesc.descriptorBindingUniformBufferUpdateAfterBind = enabled12.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;

		// NVRHI uses vulkan.hpp with exceptions enabled; failures surface as vk::SystemError.
		try
		{
			m_NvrhiVulkanDevice = nvrhi::vulkan::createDevice(nvrhiDesc);
		}
		catch (const vk::SystemError& error)
		{
			ST_CORE_ERROR("Failed to create the NVRHI Vulkan device: {}", error.what());
			return false;
		}
		if (!m_NvrhiVulkanDevice)
		{
			ST_CORE_ERROR("Failed to create the NVRHI Vulkan device");
			return false;
		}

		m_NvrhiDevice = m_Specification.EnableValidation ? nvrhi::validation::createValidationLayer(m_NvrhiVulkanDevice) : nvrhi::DeviceHandle(m_NvrhiVulkanDevice);
		return true;
	}

	bool VulkanGraphicsDevice::CreateSwapchain(Window& window, bool vsync)
	{
		if (m_Specification.Headless)
		{
			ST_CORE_ERROR("Cannot create a swapchain on a headless graphics device");
			return false;
		}
		DestroySwapchain();

		m_Window = &window;
		m_VSync = vsync;
		if (!CreateSurface() || !CreateSwapchainResources())
		{
			DestroySwapchain();
			return false;
		}
		// A minimized window has no swapchain yet; BeginFrame creates it once the window has a size.
		return true;
	}

	void VulkanGraphicsDevice::DestroySwapchain()
	{
		DestroySwapchainResources();
		DestroySurface();
		m_Window = nullptr;
	}

	bool VulkanGraphicsDevice::CreateSurface()
	{
		const VkResult result = glfwCreateWindowSurface(m_Instance, static_cast<GLFWwindow*>(m_Window->GetNativeWindow()), nullptr, &m_Surface);
		if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("Failed to create the window surface: {}", nvrhi::vulkan::resultToString(result));
			m_Surface = VK_NULL_HANDLE;
			return false;
		}

		VkBool32 supported = VK_FALSE;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, static_cast<uint32_t>(m_GraphicsFamily), m_Surface, &supported);
		if (!supported)
		{
			ST_CORE_ERROR("The selected GPU cannot present to this window");
			DestroySurface();
			return false;
		}
		return true;
	}

	void VulkanGraphicsDevice::DestroySurface()
	{
		if (m_Surface != VK_NULL_HANDLE)
		{
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
			m_Surface = VK_NULL_HANDLE;
		}
	}

	bool VulkanGraphicsDevice::CreateSwapchainResources()
	{
		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;

		VkSurfaceCapabilitiesKHR capabilities = {};
		VkResult result = dispatch.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &capabilities);
		if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: {}", nvrhi::vulkan::resultToString(result));
			return false;
		}

		// Copies to and from the back buffer are used for clears and captures.
		constexpr VkImageUsageFlags requiredUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if ((capabilities.supportedUsageFlags & requiredUsage) != requiredUsage)
		{
			ST_CORE_ERROR("The window surface does not support color attachment and transfer usage of swapchain images");
			return false;
		}

		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == UINT32_MAX)
		{
			const glm::uvec2 size = m_Window->GetFramebufferSize();
			extent.width = std::clamp(size.x, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			extent.height = std::clamp(size.y, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
		}
		if (extent.width == 0 || extent.height == 0)
			return true; // Minimized: no swapchain for now, BeginFrame creates it once the window has a size

		uint32_t formatCount = 0;
		dispatch.vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		dispatch.vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, formats.data());

		// The engine writes display-encoded (sRGB) values itself, so it wants a UNORM back buffer.
		VkSurfaceFormatKHR surfaceFormat = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
		for (const VkFormat preferred : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
		{
			for (const VkSurfaceFormatKHR& format : formats)
			{
				if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				{
					surfaceFormat = format;
					break;
				}
			}
			if (surfaceFormat.format != VK_FORMAT_UNDEFINED)
				break;
		}
		if (surfaceFormat.format == VK_FORMAT_UNDEFINED)
		{
			ST_CORE_ERROR("The window surface supports no 8-bit UNORM sRGB format");
			return false;
		}

		uint32_t presentModeCount = 0;
		dispatch.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentModeCount, nullptr);
		std::vector<VkPresentModeKHR> presentModes(presentModeCount);
		dispatch.vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentModeCount, presentModes.data());
		auto supportsMode = [&](VkPresentModeKHR mode) { return std::find(presentModes.begin(), presentModes.end(), mode) != presentModes.end(); };

		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Always supported
		if (!m_VSync)
		{
			if (supportsMode(VK_PRESENT_MODE_MAILBOX_KHR))
				presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
			else if (supportsMode(VK_PRESENT_MODE_IMMEDIATE_KHR))
				presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
		}

		uint32_t imageCount = std::max(capabilities.minImageCount + 1, m_Specification.MaxFramesInFlight + 1);
		if (capabilities.maxImageCount > 0)
			imageCount = std::min(imageCount, capabilities.maxImageCount);

		VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		for (const VkCompositeAlphaFlagBitsKHR candidate : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR })
		{
			if (capabilities.supportedCompositeAlpha & candidate)
			{
				compositeAlpha = candidate;
				break;
			}
		}

		VkSwapchainCreateInfoKHR swapchainInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		swapchainInfo.surface = m_Surface;
		swapchainInfo.minImageCount = imageCount;
		swapchainInfo.imageFormat = surfaceFormat.format;
		swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
		swapchainInfo.imageExtent = extent;
		swapchainInfo.imageArrayLayers = 1;
		swapchainInfo.imageUsage = requiredUsage;
		swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		swapchainInfo.preTransform = capabilities.currentTransform;
		swapchainInfo.compositeAlpha = compositeAlpha;
		swapchainInfo.presentMode = presentMode;
		swapchainInfo.clipped = VK_TRUE;

		result = dispatch.vkCreateSwapchainKHR(m_Device, &swapchainInfo, nullptr, &m_Swapchain);
		if (result != VK_SUCCESS)
		{
			ST_CORE_ERROR("vkCreateSwapchainKHR failed: {}", nvrhi::vulkan::resultToString(result));
			m_Swapchain = VK_NULL_HANDLE;
			return false;
		}

		m_SwapchainFormat = surfaceFormat.format;
		m_SwapchainExtent = extent;

		uint32_t swapchainImageCount = 0;
		dispatch.vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, nullptr);
		std::vector<VkImage> images(swapchainImageCount);
		dispatch.vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &swapchainImageCount, images.data());

		nvrhi::TextureDesc textureDesc;
		textureDesc.width = extent.width;
		textureDesc.height = extent.height;
		textureDesc.format = ConvertSwapchainFormat(surfaceFormat.format);
		textureDesc.debugName = "SwapchainImage";
		textureDesc.isRenderTarget = true;
		textureDesc.initialState = nvrhi::ResourceStates::Present;
		textureDesc.keepInitialState = true;

		m_SwapchainImages.clear();
		for (VkImage image : images)
		{
			SwapchainImage& swapchainImage = m_SwapchainImages.emplace_back();
			swapchainImage.Image = image;
			swapchainImage.Texture = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), textureDesc);
			swapchainImage.Framebuffer = m_NvrhiDevice->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(swapchainImage.Texture));
		}

		// Present semaphores are indexed by swapchain image. Acquire semaphores rotate per frame: an acquire semaphore
		// is reused only after the frame that waited on it has completed, which frame pacing guarantees once there are
		// more semaphores than frames in flight.
		VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		const uint32_t acquireSemaphoreCount = std::max(swapchainImageCount, m_Specification.MaxFramesInFlight) + 1;
		m_AcquireSemaphores.assign(acquireSemaphoreCount, VK_NULL_HANDLE);
		m_PresentSemaphores.assign(swapchainImageCount, VK_NULL_HANDLE);
		for (VkSemaphore& semaphore : m_AcquireSemaphores)
		{
			if (dispatch.vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
			{
				ST_CORE_ERROR("Failed to create swapchain semaphores");
				return false;
			}
		}
		for (VkSemaphore& semaphore : m_PresentSemaphores)
		{
			if (dispatch.vkCreateSemaphore(m_Device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
			{
				ST_CORE_ERROR("Failed to create swapchain semaphores");
				return false;
			}
		}
		m_AcquireSemaphoreIndex = 0;
		m_SwapchainDirty = false;

		ST_CORE_INFO("Swapchain: {} x {}, {} images, {}", extent.width, extent.height, swapchainImageCount,
			presentMode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate"));
		return true;
	}

	void VulkanGraphicsDevice::DestroySwapchainResources()
	{
		if (m_Device == VK_NULL_HANDLE)
			return;

		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
		if (m_NvrhiDevice)
			m_NvrhiDevice->waitForIdle();

		m_ImageAcquired = false;
		m_SwapchainImages.clear();
		if (m_NvrhiDevice)
			m_NvrhiDevice->runGarbageCollection();

		for (VkSemaphore semaphore : m_AcquireSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				dispatch.vkDestroySemaphore(m_Device, semaphore, nullptr);
		}
		for (VkSemaphore semaphore : m_PresentSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				dispatch.vkDestroySemaphore(m_Device, semaphore, nullptr);
		}
		m_AcquireSemaphores.clear();
		m_PresentSemaphores.clear();

		if (m_Swapchain != VK_NULL_HANDLE)
		{
			dispatch.vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
			m_Swapchain = VK_NULL_HANDLE;
		}
	}

	bool VulkanGraphicsDevice::RecreateSwapchainResources()
	{
		DestroySwapchainResources();
		return CreateSwapchainResources();
	}

	void VulkanGraphicsDevice::InitializeAcquiredImage()
	{
		// NVRHI tracks swapchain images as being in the Present state between frames, but an image that has
		// never been presented is still in VK_IMAGE_LAYOUT_UNDEFINED. Move it to PRESENT_SRC once, on first acquire.
		SwapchainImage& image = m_SwapchainImages[m_CurrentImageIndex];
		if (image.Initialized)
			return;

		m_BarrierCommandList->open();
		VkCommandBuffer commandBuffer = static_cast<VkCommandBuffer>(m_BarrierCommandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
		VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = 0;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image.Image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		// ALL_COMMANDS on both sides chains this transition after the acquire semaphore wait submitted by BeginFrame.
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		m_BarrierCommandList->close();
		m_NvrhiDevice->executeCommandList(m_BarrierCommandList);
		image.Initialized = true;
	}

	void VulkanGraphicsDevice::OnDeviceLost(const char* operation)
	{
		if (!m_DeviceLost)
			ST_CORE_CRITICAL("The graphics device was lost ({}). Rendering stops; restart the application.", operation);
		m_DeviceLost = true;
		m_ImageAcquired = false;
	}

	bool VulkanGraphicsDevice::BeginFrame()
	{
		m_ImageAcquired = false;
		if (m_DeviceLost)
			return false;

		// Frame pacing: wait until the GPU has finished the frame that last used this slot.
		try
		{
			while (m_FramesInFlight.size() >= m_Specification.MaxFramesInFlight)
			{
				nvrhi::EventQueryHandle query = m_FramesInFlight.front();
				m_FramesInFlight.pop();
				m_NvrhiDevice->waitEventQuery(query);
				m_EventQueryPool.push_back(query);
			}
		}
		catch (const vk::SystemError& error)
		{
			OnDeviceLost(error.what());
			return false;
		}

		if (!m_Window)
			return true; // Headless or no window: offscreen rendering only
		if (m_Surface == VK_NULL_HANDLE && !CreateSurface())
			return false; // Retried every frame

		const glm::uvec2 framebufferSize = m_Window->GetFramebufferSize();
		if (framebufferSize.x == 0 || framebufferSize.y == 0)
			return false; // Minimized

		if (m_Swapchain == VK_NULL_HANDLE || m_SwapchainDirty || framebufferSize != GetBackBufferSize())
		{
			if (!RecreateSwapchainResources() || m_Swapchain == VK_NULL_HANDLE)
				return false;
		}

		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			VkSemaphore acquireSemaphore = m_AcquireSemaphores[m_AcquireSemaphoreIndex];
			VkResult result = dispatch.vkAcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX, acquireSemaphore, VK_NULL_HANDLE, &m_CurrentImageIndex);
			if (result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				if (!RecreateSwapchainResources() || m_Swapchain == VK_NULL_HANDLE)
					return false;
				continue;
			}
			if (result == VK_ERROR_DEVICE_LOST)
			{
				OnDeviceLost("vkAcquireNextImageKHR");
				return false;
			}
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			{
				ST_CORE_ERROR("vkAcquireNextImageKHR failed: {}", nvrhi::vulkan::resultToString(result));
				// Rebuild the surface and swapchain next frame (e.g. after VK_ERROR_SURFACE_LOST_KHR).
				DestroySwapchainResources();
				DestroySurface();
				return false;
			}

			if (result == VK_SUBOPTIMAL_KHR)
				m_SwapchainDirty = true; // Present this frame, recreate before the next one

			m_AcquireSemaphoreIndex = (m_AcquireSemaphoreIndex + 1) % static_cast<uint32_t>(m_AcquireSemaphores.size());

			// Every later submission on the graphics queue waits for the image. NVRHI's own semaphore waits use the
			// TOP_OF_PIPE stage, which blocks nothing, so the wait is submitted here with ALL_COMMANDS: a semaphore wait
			// also orders all work submitted after it on the same queue.
			const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &acquireSemaphore;
			submitInfo.pWaitDstStageMask = &waitStage;
			result = dispatch.vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
			if (result != VK_SUCCESS)
			{
				if (result == VK_ERROR_DEVICE_LOST)
				{
					OnDeviceLost("vkQueueSubmit");
				}
				else
				{
					// The acquired image and its signaled semaphore cannot be used; start over with a new swapchain.
					ST_CORE_ERROR("vkQueueSubmit failed: {}", nvrhi::vulkan::resultToString(result));
					DestroySwapchainResources();
				}
				return false;
			}

			m_ImageAcquired = true;
			InitializeAcquiredImage();
			return true;
		}
		return false;
	}

	void VulkanGraphicsDevice::EndFrame()
	{
		if (m_ImageAcquired)
		{
			VkSemaphore presentSemaphore = m_PresentSemaphores[m_CurrentImageIndex];
			m_NvrhiVulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, presentSemaphore, 0);

			// NVRHI attaches queued semaphore signals to the next submission; submit an empty command list to flush.
			m_BarrierCommandList->open();
			m_BarrierCommandList->close();
			m_NvrhiDevice->executeCommandList(m_BarrierCommandList);

			VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &presentSemaphore;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &m_Swapchain;
			presentInfo.pImageIndices = &m_CurrentImageIndex;
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
				m_SwapchainDirty = true;
			else if (result == VK_ERROR_DEVICE_LOST)
				OnDeviceLost("vkQueuePresentKHR");
			else if (result != VK_SUCCESS)
				ST_CORE_ERROR("vkQueuePresentKHR failed: {}", nvrhi::vulkan::resultToString(result));

			m_ImageAcquired = false;
		}

		nvrhi::EventQueryHandle query;
		if (!m_EventQueryPool.empty())
		{
			query = m_EventQueryPool.back();
			m_EventQueryPool.pop_back();
		}
		else
		{
			query = m_NvrhiDevice->createEventQuery();
		}
		m_NvrhiDevice->resetEventQuery(query);
		m_NvrhiDevice->setEventQuery(query, nvrhi::CommandQueue::Graphics);
		m_FramesInFlight.push(query);

		m_NvrhiDevice->runGarbageCollection();
		m_FrameIndex++;
	}

	void VulkanGraphicsDevice::Shutdown()
	{
		if (m_NvrhiDevice)
			m_NvrhiDevice->waitForIdle();

		DestroySwapchain();
		while (!m_FramesInFlight.empty())
			m_FramesInFlight.pop();
		m_EventQueryPool.clear();
		m_BarrierCommandList = nullptr;

		if (m_NvrhiDevice)
			m_NvrhiDevice->runGarbageCollection();
		m_NvrhiDevice = nullptr;
		m_NvrhiVulkanDevice = nullptr;

		const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
		if (m_Device != VK_NULL_HANDLE)
		{
			dispatch.vkDestroyDevice(m_Device, nullptr);
			m_Device = VK_NULL_HANDLE;
		}
		if (m_DebugMessenger != VK_NULL_HANDLE)
		{
			dispatch.vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
			m_DebugMessenger = VK_NULL_HANDLE;
		}
		if (m_Instance != VK_NULL_HANDLE)
		{
			dispatch.vkDestroyInstance(m_Instance, nullptr);
			m_Instance = VK_NULL_HANDLE;
		}
	}

}
