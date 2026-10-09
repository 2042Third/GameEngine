#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/Renderer.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

namespace Strata
{

	// Part of one 2D subresource of a texture. A zero width or height extends the region to the subresource's edge.
	struct TextureReadbackRegion
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t MipLevel = 0;
		uint32_t ArraySlice = 0;
	};

	// Copies a region of a texture to the CPU without stalling the frame: Create submits the copy, IsReady reports
	// (without waiting) whether the GPU has finished it, and GetResult then takes the pixels. Poll once per frame; for
	// one-off reads that may block, use Renderer::ReadTexture. The copy sees everything submitted before Create.
	// Destroying a pending readback is safe. Main thread only.
	class TextureReadback
	{
	public:
		// Null (with an error) without a renderer, for missing textures, block-compressed, depth-stencil, multisampled or 3D
		// textures, and regions outside the subresource.
		static Scope<TextureReadback> Create(nvrhi::ITexture* texture, const TextureReadbackRegion& region = {}, std::string* outError = nullptr);

		TextureReadback(const TextureReadback&) = delete;
		TextureReadback& operator=(const TextureReadback&) = delete;

		bool IsReady() const;
		// Blocks until the copy has finished.
		void Wait();
		// The pixels (tightly packed rows, top row first); false while the copy is pending or when the staging memory
		// cannot be mapped.
		bool GetResult(ReadbackImage& outImage) const;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		nvrhi::Format GetFormat() const { return m_Format; }
	private:
		TextureReadback() = default;
	private:
		nvrhi::StagingTextureHandle m_Staging;
		nvrhi::EventQueryHandle m_Query;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_BytesPerPixel = 0;
		nvrhi::Format m_Format = nvrhi::Format::UNKNOWN;
	};

}
