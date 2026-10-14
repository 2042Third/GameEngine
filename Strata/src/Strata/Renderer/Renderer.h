#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/DeferredReleaseQueue.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/RendererUploadContext.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/StagingPool.h"

#include <nvrhi/nvrhi.h>

#include <vector>

namespace Strata
{

	class RenderSceneGPUCache;

	struct ReadbackImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		nvrhi::Format Format = nvrhi::Format::UNKNOWN;
		uint32_t BytesPerPixel = 0;
		std::vector<uint8_t> Pixels; // Tightly packed rows, top row first
	};

	struct RendererSpecification
	{
		// Asset uploads the managers may wait for run on the device's dedicated transfer queue, where it has one
		// (GPUUploadQueue::SetTransferQueue). Off: on the reference machine it did not load the exported stress game faster
		// (every texture resident from frame 10-11 with it, 10 without, in 5 runs each) and held more memory meanwhile (peak
		// private memory 595-622 MB with it, 582-595 MB without): each upload is handed to the graphics queue a frame after
		// its last copy, so its data stays that much longer.
		bool TransferQueueUploads = false;
	};

	// Process-wide rendering services shared by every renderer: the graphics device, shader library, common
	// samplers and fallback textures. Main thread only.
	class Renderer
	{
	public:
		static bool Init(GraphicsDevice& device, const RendererSpecification& specification = {});
		static void Shutdown();
		static bool IsInitialized();

		static GraphicsDevice& GetGraphicsDevice();
		static nvrhi::IDevice* GetDevice();
		static ShaderLibrary& GetShaderLibrary();
		static BindlessTextureTable& GetBindlessTextures();
		// Staging memory for uploads (textures and meshes upload through it).
		static StagingPool& GetStagingPool();
		// Where the asset managers' finalizations record their uploads (the IAssetGraphics engine service hands it to them).
		static RendererUploadContext& GetUploadContext();
		// The budgeted queue every asset upload goes through (the upload context's batches are its batches).
		static GPUUploadQueue& GetUploadQueue();
		// Every mesh's GPU geometry (null without a renderer). Meshes keep a weak reference: the pool goes with the renderer.
		static Ref<GeometryPool> GetGeometryPool();
		// GPU time per stage of the frame (timestamps; a no-op without timestamp support): passes are timed with GPUPassTimer.
		static GPUProfiler& GetGPUProfiler();
		// The profiler, or null without a renderer (any time, also while the renderer initializes).
		static GPUProfiler* FindGPUProfiler();
		// The render scenes' GPU copies, shared by each scene's renderers (RenderSceneGPU).
		static RenderSceneGPUCache& GetRenderSceneGPUs();
		// Resources dropped while the GPU may still use them (see ReleaseDeferred).
		static DeferredReleaseQueue& GetDeferredReleases();
		// Drops the references once the GPU has finished every command list executed so far, without waiting for it. Use
		// it for render targets and whatever references them when they are recreated or freed: NVRHI does not keep the
		// textures of clears alive for the GPU (DeferredReleaseQueue). Ignored without a renderer (nothing renders).
		static void ReleaseDeferred(std::vector<nvrhi::ResourceHandle> resources);

		// Called by the application after the graphics device began a frame: hands the GPU times of completed frames to the
		// frame profiler (GPUProfiler::BeginFrame), returns the geometry pool's freed ranges and grows or compacts it
		// (GeometryPool::BeginFrame), releases the GPU copies of render scenes that went (RenderSceneGPUCache::BeginFrame),
		// recycles bindless slots and staging memory, rolls the upload queue's frame and drops deferred releases the GPU is
		// done with.
		static void BeginFrame();
		// Before the first command list of a frame that renders into the back buffer: the GPU waits for the display to hand
		// the image over only from here on (GraphicsDevice::PrepareBackBuffer), so the frame's earlier work - asset uploads,
		// offscreen rendering - never queues behind the display; the first call of a frame also clears the back buffer to
		// black, so layers render onto a defined image. Returns the back buffer (null without one, e.g. headless).
		static nvrhi::ITexture* PrepareBackBuffer();

		static nvrhi::ISampler* GetLinearClampSampler();
		static nvrhi::ISampler* GetLinearWrapSampler();
		static nvrhi::ISampler* GetPointClampSampler();
		static nvrhi::ISampler* GetAnisotropicWrapSampler();

		// 1x1 fallback textures used while real textures stream in.
		static nvrhi::ITexture* GetWhiteTexture();
		static nvrhi::ITexture* GetBlackTexture();
		static nvrhi::ITexture* GetFlatNormalTexture();
		static nvrhi::ITexture* GetBlackCubeTexture();

		// Copies one subresource of a 2D, 2D-array or cube texture (a mip level of an array slice or cube face) to the
		// CPU. Blocks until the GPU is done; meant for screenshots, picking fallbacks and tests, not per-frame use.
		// Returns false for missing textures, block-compressed formats and out-of-range subresources.
		static bool ReadTexture(nvrhi::ITexture* texture, ReadbackImage& outImage, uint32_t mipLevel = 0, uint32_t arraySlice = 0);
	};

}
