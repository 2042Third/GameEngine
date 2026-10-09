#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <string>

namespace Strata
{

	class Window;

	struct GraphicsDeviceSpecification
	{
		std::string ApplicationName = "Strata";
		// Enables the Khronos validation layer (when installed, e.g. with the Vulkan SDK) and NVRHI's own
		// validation layer. Intended for Debug builds and tests.
		bool EnableValidation = false;
		// No presentation support: offscreen rendering only (tests, servers, headless automation).
		bool Headless = false;
		uint32_t MaxFramesInFlight = 2;
		int32_t AdapterIndex = -1; // -1 selects the best adapter (discrete GPUs preferred)
	};

	struct GraphicsDeviceInfo
	{
		std::string API = "Vulkan";
		std::string APIVersion;
		std::string AdapterName;
		std::string DriverVersion;
		uint64_t DedicatedVideoMemory = 0;
		bool IsDiscreteGPU = false;
		bool SupportsBCCompression = false;
		bool SupportsWireframe = false;
		bool ValidationEnabled = false;
		uint32_t MaxTextureDimension2D = 4096; // Largest width or height of a 2D texture or render target
	};

	struct GraphicsMemoryBudget
	{
		uint64_t Budget = 0; // Bytes the process can use without hurting performance (device-local heaps)
		uint64_t Usage = 0;  // Bytes currently allocated by the process
	};

	// Owns the GPU device (through NVRHI), the swapchain of the main window, and frame pacing.
	class GraphicsDevice
	{
	public:
		virtual ~GraphicsDevice() = default;

		// Creates the device for the platform's graphics API. Returns nullptr (after logging the reason) when
		// no suitable GPU or driver is available.
		static Scope<GraphicsDevice> Create(const GraphicsDeviceSpecification& specification);

		virtual nvrhi::IDevice* GetDevice() const = 0;
		virtual const GraphicsDeviceInfo& GetInfo() const = 0;
		virtual GraphicsMemoryBudget GetMemoryBudget() const = 0;

		// Presentation. Not available on headless devices. A minimized window gets its swapchain once it has a size.
		virtual bool CreateSwapchain(Window& window, bool vsync) = 0;
		virtual void DestroySwapchain() = 0;
		virtual bool HasSwapchain() const = 0; // True while presenting to a window

		virtual void SetVSync(bool vsync) = 0;
		virtual bool IsVSync() const = 0;
		virtual nvrhi::ITexture* GetBackBuffer() const = 0;
		virtual nvrhi::IFramebuffer* GetBackBufferFramebuffer() const = 0;
		virtual nvrhi::Format GetBackBufferFormat() const = 0;
		virtual glm::uvec2 GetBackBufferSize() const = 0;

		// BeginFrame waits for a free frame slot and, with a swapchain, acquires the next back buffer (recreating
		// the swapchain after resizes). It returns false when nothing should be rendered this frame (e.g. the
		// window is minimized, or the device was lost); EndFrame must then not be called. EndFrame presents and
		// advances the frame index.
		virtual bool BeginFrame() = 0;
		virtual void EndFrame() = 0;
		virtual uint64_t GetFrameIndex() const = 0;
		virtual uint32_t GetMaxFramesInFlight() const = 0;
		virtual void WaitForIdle() = 0;

		// Errors reported by the validation layers or NVRHI so far. Tests require this to stay zero.
		virtual uint32_t GetErrorCount() const = 0;
		// True after an unrecoverable GPU failure (driver reset, GPU removed); nothing renders afterwards.
		virtual bool IsDeviceLost() const = 0;
	};

}
