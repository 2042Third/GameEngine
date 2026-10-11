#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>

namespace Strata
{

	// Images of display colors the editor's UI draws as they are (thumbnails): RGBA8 textures, sampled as shader resources
	// (ImGuiTextureDisplay::Direct).
	namespace DisplayImage
	{

		// A texture of `width` x `height` RGBA8 pixels (tightly packed rows, top row first), filled through the command list,
		// which must be open. Null without a renderer, for an empty image or too few pixels, and when the device cannot make it.
		nvrhi::TextureHandle Upload(nvrhi::ICommandList* commandList, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, const char* debugName);

	}

}
