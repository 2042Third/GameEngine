#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <cstdint>
#include <filesystem>
#include <vector>

// Helpers of the tests that stream a texture's mip levels on a device (GPU.TextureStreaming, GPU.Editor.StreamingView).
namespace Strata::Tests
{

	// A 2048x2048 BC7 texture of a smooth gradient (the same function at every level, so each level is the texture seen from
	// further away and the levels next to each other differ little) and an unlit material showing it.
	constexpr uint32_t c_GradientTextureSize = 2048;
	constexpr uint32_t c_GradientTextureMips = 12;
	constexpr AssetHandle c_GradientTextureHandle = AssetHandle(0xA1000);
	constexpr AssetHandle c_GradientMaterialHandle = AssetHandle(0xA1001);

	// The texture's cooked form: every level of the gradient, one color per 4x4 block (solid blocks encode exactly).
	std::vector<uint8_t> CookGradientTexture();
	// An asset pack with the texture and the material, in a new temporary directory.
	std::filesystem::path WriteGradientStreamingPack();

}
