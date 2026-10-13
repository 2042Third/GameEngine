#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Renderer/Material.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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
	// A `size` x `size` BC7 sRGB texture (a power of two, at least 4) of one color, with every level.
	std::vector<uint8_t> CookSolidTexture(uint32_t size, const std::array<uint8_t, 4>& color);
	// An asset pack with the texture and the material, in a new temporary directory.
	std::filesystem::path WriteGradientStreamingPack();

	// A 2048x2048 RGBA8 checkerboard of squares of c_CheckerSquareTexels texels and its exact mip chain (each level the box
	// filter of the one before: checkerboards down to squares of a texel, then gray): a coarser level than the screen needs
	// shows at once, as blurred edges.
	constexpr uint32_t c_CheckerTextureSize = 2048;
	constexpr uint32_t c_CheckerSquareTexels = 8;
	std::vector<uint8_t> CookCheckerTexture();
	// An equirectangular RGBA16F environment map of width x height texels with its mip chain: a sky gradient.
	std::vector<uint8_t> CookEnvironmentMap(uint32_t width, uint32_t height);
	// The stored form of a material document.
	std::vector<uint8_t> SerializeMaterial(const MaterialProperties& properties);

	// An asset of a pack the tests write (WriteStreamingPack).
	struct StreamingPackAsset
	{
		AssetHandle Handle;
		AssetType Type = AssetType::None;
		std::string Path;
		std::vector<uint8_t> Data;
	};
	// An asset pack with the assets, in a new temporary directory.
	std::filesystem::path WriteStreamingPack(const std::vector<StreamingPackAsset>& assets);

}
