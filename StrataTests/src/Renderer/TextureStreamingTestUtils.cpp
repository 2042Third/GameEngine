#include "Renderer/TextureStreamingTestUtils.h"

#include <doctest/doctest.h>

#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BC7Encoder.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Texture.h"
#include "TestHelpers.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <unordered_map>

namespace Strata::Tests
{

	namespace
	{

		// The color of a texel at (u, v) in [0, 1].
		std::array<uint8_t, 4> GetGradient(float u, float v)
		{
			return { static_cast<uint8_t>(40.0f + 150.0f * u), static_cast<uint8_t>(40.0f + 150.0f * v), static_cast<uint8_t>(60.0f + 60.0f * (u + v)), 255 };
		}

	}

	std::vector<uint8_t> CookGradientTexture()
	{
		std::map<uint32_t, std::array<uint8_t, 16>> encoded; // Per color
		std::vector<std::vector<uint8_t>> levels(c_GradientTextureMips);
		std::vector<std::span<const uint8_t>> spans;
		for (uint32_t level = 0; level < c_GradientTextureMips; level++)
		{
			const uint32_t size = std::max(1u, c_GradientTextureSize >> level);
			const uint32_t blocks = (size + 3) / 4;
			std::vector<uint8_t>& data = levels[level];
			data.resize(static_cast<size_t>(blocks) * blocks * 16);
			for (uint32_t y = 0; y < blocks; y++)
			{
				for (uint32_t x = 0; x < blocks; x++)
				{
					const std::array<uint8_t, 4> color = GetGradient((static_cast<float>(x) + 0.5f) / static_cast<float>(blocks), (static_cast<float>(y) + 0.5f) / static_cast<float>(blocks));
					const uint32_t key = static_cast<uint32_t>(color[0]) | (static_cast<uint32_t>(color[1]) << 8) | (static_cast<uint32_t>(color[2]) << 16);
					auto it = encoded.find(key);
					if (it == encoded.end())
					{
						std::array<uint8_t, 64> texels {};
						for (uint32_t texel = 0; texel < 16; texel++)
							std::memcpy(texels.data() + texel * 4, color.data(), 4);
						std::array<uint8_t, 16> block {};
						BC7Encoder::EncodeBlock(texels.data(), block.data());
						it = encoded.emplace(key, block).first;
					}
					std::memcpy(data.data() + (static_cast<size_t>(y) * blocks + x) * 16, it->second.data(), 16);
				}
			}
			spans.push_back(data);
		}
		CookedTextureDescription description;
		description.Format = TextureFormat::BC7SRGB;
		description.Width = c_GradientTextureSize;
		description.Height = c_GradientTextureSize;
		description.MipCount = c_GradientTextureMips;
		description.DebugName = "StreamedGradient";
		std::vector<uint8_t> cooked = WriteCookedTexture(description, spans);
		REQUIRE_FALSE(cooked.empty());
		return cooked;
	}

	std::vector<uint8_t> CookSolidTexture(uint32_t size, const std::array<uint8_t, 4>& color)
	{
		std::array<uint8_t, 64> texels {};
		for (uint32_t texel = 0; texel < 16; texel++)
			std::memcpy(texels.data() + texel * 4, color.data(), 4);
		std::array<uint8_t, 16> block {};
		BC7Encoder::EncodeBlock(texels.data(), block.data());
		const uint32_t mips = TextureUtils::CalculateMipCount(size, size);
		std::vector<std::vector<uint8_t>> levels(mips);
		std::vector<std::span<const uint8_t>> spans;
		for (uint32_t level = 0; level < mips; level++)
		{
			const uint32_t blocks = (std::max(1u, size >> level) + 3) / 4;
			std::vector<uint8_t>& data = levels[level];
			data.resize(static_cast<size_t>(blocks) * blocks * 16);
			for (size_t offset = 0; offset < data.size(); offset += 16)
				std::memcpy(data.data() + offset, block.data(), 16);
			spans.push_back(data);
		}
		CookedTextureDescription description;
		description.Format = TextureFormat::BC7SRGB;
		description.Width = size;
		description.Height = size;
		description.MipCount = mips;
		description.DebugName = "Solid";
		std::vector<uint8_t> cooked = WriteCookedTexture(description, spans);
		REQUIRE_FALSE(cooked.empty());
		return cooked;
	}

	std::filesystem::path WriteGradientStreamingPack()
	{
		MaterialProperties properties = UnlitColor(glm::vec4(1.0f));
		properties.BaseColorMap = c_GradientTextureHandle;
		return WriteStreamingPack({ StreamingPackAsset { c_GradientTextureHandle, AssetType::Texture, "Textures/Gradient.png", CookGradientTexture() },
			StreamingPackAsset { c_GradientMaterialHandle, AssetType::Material, "Materials/Gradient.stmat", SerializeMaterial(properties) } });
	}

	std::vector<uint8_t> CookCheckerTexture()
	{
		constexpr uint32_t c_Mips = 12;
		std::vector<std::vector<uint8_t>> levels(c_Mips);
		std::vector<std::span<const uint8_t>> spans;
		for (uint32_t level = 0; level < c_Mips; level++)
		{
			const uint32_t size = std::max(1u, c_CheckerTextureSize >> level);
			const uint32_t square = c_CheckerSquareTexels >> level; // 0 once the squares are finer than a texel: gray
			std::vector<uint8_t>& data = levels[level];
			data.resize(static_cast<size_t>(size) * size * 4);
			for (uint32_t y = 0; y < size; y++)
			{
				for (uint32_t x = 0; x < size; x++)
				{
					const uint8_t value = square == 0 ? 128 : (((x / square) + (y / square)) % 2 == 0 ? 236 : 20);
					uint8_t* texel = data.data() + (static_cast<size_t>(y) * size + x) * 4;
					texel[0] = value;
					texel[1] = value;
					texel[2] = value;
					texel[3] = 255;
				}
			}
			spans.push_back(data);
		}
		CookedTextureDescription description;
		description.Format = TextureFormat::RGBA8;
		description.Width = c_CheckerTextureSize;
		description.Height = c_CheckerTextureSize;
		description.MipCount = c_Mips;
		description.DebugName = "StreamedChecker";
		std::vector<uint8_t> cooked = WriteCookedTexture(description, spans);
		REQUIRE_FALSE(cooked.empty());
		return cooked;
	}

	std::vector<uint8_t> CookEnvironmentMap(uint32_t width, uint32_t height)
	{
		const uint32_t mips = TextureUtils::CalculateMipCount(width, height);
		std::vector<std::vector<uint8_t>> levels(mips);
		std::vector<std::span<const uint8_t>> spans;
		for (uint32_t level = 0; level < mips; level++)
		{
			const uint32_t levelWidth = std::max(1u, width >> level);
			const uint32_t levelHeight = std::max(1u, height >> level);
			std::vector<uint8_t>& data = levels[level];
			data.resize(static_cast<size_t>(levelWidth) * levelHeight * 8);
			for (uint32_t y = 0; y < levelHeight; y++)
			{
				// Bright toward the zenith (the top rows), dim toward the ground.
				const float up = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(levelHeight);
				const std::array<uint16_t, 4> texel = { glm::packHalf1x16(0.2f + 0.8f * up), glm::packHalf1x16(0.3f + 0.9f * up), glm::packHalf1x16(0.5f + 1.5f * up),
					glm::packHalf1x16(1.0f) };
				for (uint32_t x = 0; x < levelWidth; x++)
					std::memcpy(data.data() + (static_cast<size_t>(y) * levelWidth + x) * 8, texel.data(), 8);
			}
			spans.push_back(data);
		}
		CookedTextureDescription description;
		description.Format = TextureFormat::RGBA16F;
		description.Width = width;
		description.Height = height;
		description.MipCount = mips;
		description.DebugName = "StreamedSky";
		std::vector<uint8_t> cooked = WriteCookedTexture(description, spans);
		REQUIRE_FALSE(cooked.empty());
		return cooked;
	}

	std::vector<uint8_t> SerializeMaterial(const MaterialProperties& properties)
	{
		const std::string material = JsonUtils::Dump(Material::Create(properties)->Serialize());
		return std::vector<uint8_t>(material.begin(), material.end());
	}

	std::filesystem::path WriteStreamingPack(const std::vector<StreamingPackAsset>& assets)
	{
		std::unordered_map<AssetHandle, const std::vector<uint8_t>*> data;
		std::vector<AssetMetadata> metadata;
		for (const StreamingPackAsset& asset : assets)
		{
			AssetMetadata entry;
			entry.Handle = asset.Handle;
			entry.Type = asset.Type;
			entry.Path = asset.Path;
			// The file name without its extension.
			const size_t slash = asset.Path.find_last_of('/');
			const std::string file = slash == std::string::npos ? asset.Path : asset.Path.substr(slash + 1);
			entry.Name = file.substr(0, file.find_last_of('.'));
			metadata.push_back(entry);
			data[asset.Handle] = &asset.Data;
		}

		const std::filesystem::path pack = CreateTemporaryDirectory("TextureStreaming") / "Streaming.stpak";
		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(pack, metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = *data.at(asset.Handle);
			return true;
		}, &error), error);
		return pack;
	}

}
