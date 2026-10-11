#include "Renderer/TextureStreamingTestUtils.h"

#include <doctest/doctest.h>

#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BC7Encoder.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/Material.h"
#include "TestHelpers.h"

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

	std::filesystem::path WriteGradientStreamingPack()
	{
		MaterialProperties properties = UnlitColor(glm::vec4(1.0f));
		properties.BaseColorMap = c_GradientTextureHandle;
		const std::string material = JsonUtils::Dump(Material::Create(properties)->Serialize());

		std::unordered_map<AssetHandle, std::vector<uint8_t>> data;
		data[c_GradientTextureHandle] = CookGradientTexture();
		data[c_GradientMaterialHandle] = std::vector<uint8_t>(material.begin(), material.end());
		std::vector<AssetMetadata> metadata(2);
		metadata[0].Handle = c_GradientTextureHandle;
		metadata[0].Type = AssetType::Texture;
		metadata[0].Path = "Textures/Gradient.png";
		metadata[0].Name = "Gradient";
		metadata[1].Handle = c_GradientMaterialHandle;
		metadata[1].Type = AssetType::Material;
		metadata[1].Path = "Materials/Gradient.stmat";
		metadata[1].Name = "GradientMaterial";

		const std::filesystem::path pack = CreateTemporaryDirectory("TextureStreaming") / "Streaming.stpak";
		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(pack, metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(asset.Handle);
			return true;
		}, &error), error);
		return pack;
	}

}
