#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/StagingTexturePool.h"

#include <nvrhi/nvrhi.h>

#include <vector>

namespace Strata
{

	struct ReadbackImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		nvrhi::Format Format = nvrhi::Format::UNKNOWN;
		uint32_t BytesPerPixel = 0;
		std::vector<uint8_t> Pixels; // Tightly packed rows, top row first
	};

	// Process-wide rendering services shared by every renderer: the graphics device, shader library, common
	// samplers and fallback textures. Main thread only.
	class Renderer
	{
	public:
		static bool Init(GraphicsDevice& device);
		static void Shutdown();
		static bool IsInitialized();

		static GraphicsDevice& GetGraphicsDevice();
		static nvrhi::IDevice* GetDevice();
		static ShaderLibrary& GetShaderLibrary();
		static BindlessTextureTable& GetBindlessTextures();
		// Staging textures for uploads (textures upload through them).
		static StagingTexturePool& GetStagingTextures();

		// Called by the application after the graphics device began a frame: recycles bindless slots and staging textures
		// and clears the back buffer (when there is one) to black, so layers render onto a defined image.
		static void BeginFrame();

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
