#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
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
		// Storage images of the extended formats (r16f, r8...; shaderStorageImageExtendedFormats), enabled when present.
		bool SupportsExtendedStorageFormats = false;
		bool ValidationEnabled = false;
		uint32_t MaxTextureDimension2D = 4096; // Largest width or height of a 2D texture or render target
		// Slots of the renderer's bindless texture table: GraphicsDevice::c_MaxBindlessTextures, or fewer on a device whose
		// update-after-bind limits allow fewer sampled images (bindless tables are update-after-bind descriptor sets) or
		// whose descriptor set support query rejects larger tables (halved down to 1,024: ChooseBindlessTableSize).
		uint32_t MaxBindlessTextures = 0;
	};

	struct GraphicsMemoryBudget
	{
		uint64_t Budget = 0; // Bytes the process can use without hurting performance (device-local heaps)
		uint64_t Usage = 0;  // Bytes currently allocated by the process
	};

	// A resource that copies on the transfer queue wrote and the graphics queue reads next: mip levels of a texture (left
	// in the copy destination layout by the copies, in the shader read layout after the transfer), or a whole buffer
	// (GraphicsDevice::RecordQueueTransfer).
	struct GPUQueueTransfer
	{
		nvrhi::ITexture* Texture = nullptr;
		uint32_t FirstMip = 0;
		uint32_t MipCount = 0;
		nvrhi::IBuffer* Buffer = nullptr; // Instead of a texture
	};

	// A copy of rows of one mip level of a color texture from a buffer (GraphicsDevice::RecordBufferToTextureCopies): the
	// rows are tightly packed in the buffer from SourceOffset on, as whole blocks for block-compressed formats.
	struct GPUBufferTextureCopy
	{
		nvrhi::IBuffer* Source = nullptr;
		uint64_t SourceOffset = 0; // A multiple of 16 bytes (every format's texel block size divides it)
		nvrhi::ITexture* Destination = nullptr;
		uint32_t MipLevel = 0;
		uint32_t FirstRow = 0;     // Texel rows; for block-compressed formats the first row of a block
		uint32_t Width = 0;        // The level's width in texels
		uint32_t Rows = 0;         // Texel rows; for block-compressed formats whole blocks but at the level's end
	};

	// Owns the GPU device (through NVRHI), the swapchain of the main window, and frame pacing.
	class GraphicsDevice
	{
	public:
		virtual ~GraphicsDevice() = default;

		// The bindless texture table's size where the device allows it: every texture resident at once takes a slot.
		static constexpr uint32_t c_MaxBindlessTextures = 32768;

		// Creates the device for the platform's graphics API. Returns nullptr (after logging the reason) when
		// no suitable GPU or driver is available.
		static Scope<GraphicsDevice> Create(const GraphicsDeviceSpecification& specification);
		// What to install when Create finds no graphics API or GPU on this platform (one sentence; its errors end with it).
		static std::string GetDriverInstallAdvice();

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
		// The GPU waits for the display to release the acquired back buffer only from here on: call it before the first
		// command list of the frame that uses the back buffer (Renderer::PrepareBackBuffer does), so that the frame's earlier
		// work - asset uploads, offscreen rendering - never queues behind the display. Does nothing after the first call of a
		// frame; EndFrame calls it when nothing did. False when the frame has no back buffer (headless, or the wait could not
		// be submitted: the back buffer is gone for the frame).
		virtual bool PrepareBackBuffer() = 0;
		virtual uint64_t GetFrameIndex() const = 0;
		virtual uint32_t GetMaxFramesInFlight() const = 0;
		virtual void WaitForIdle() = 0;

		// Whether the device has a dedicated transfer queue (nvrhi::CommandQueue::Copy), which copies can run on beside the
		// graphics queue's work.
		virtual bool HasTransferQueue() const = 0;
		// Records half of the queue family ownership transfer of a resource the transfer queue wrote to the graphics queue:
		// the release (acquire false) into a command list of the transfer queue, after the copies; the acquire into a command
		// list of the graphics queue that is executed once the release completed (observed on the CPU), before anything uses
		// the resource there. Both halves must be recorded with the same transfer. Pending barriers of the command list are
		// committed first. Does nothing without a transfer queue.
		virtual void RecordQueueTransfer(nvrhi::ICommandList* commandList, const GPUQueueTransfer& transfer, bool acquire) = 0;
		// Records copies from buffers into textures, which NVRHI's command lists only make from staging textures (one per
		// shape of upload, so they could hardly be reused). The destinations' levels are made copy destinations through the
		// command list's state tracking first; the sources must be copy sources already (staging buffers stay ones). The
		// command list does not keep the resources alive: the caller does until the GPU has run the copies.
		virtual void RecordBufferToTextureCopies(nvrhi::ICommandList* commandList, std::span<const GPUBufferTextureCopy> copies) = 0;

		// Errors reported by the validation layers or NVRHI so far. Tests require this to stay zero.
		virtual uint32_t GetErrorCount() const = 0;
		// True after an unrecoverable GPU failure (driver reset, GPU removed); nothing renders afterwards.
		virtual bool IsDeviceLost() const = 0;
	};

}
