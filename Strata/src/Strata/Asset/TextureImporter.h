#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Renderer/Texture.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// How a texture's pixels are interpreted: decides the stored format, color space and mip filtering.
	enum class TextureUsage : uint8_t
	{
		Color = 0, // sRGB color: albedo, emissive, UI
		NormalMap, // Linear; mips are renormalized
		Data,      // Linear: roughness, metallic, occlusion, masks
		HDR        // Floating point (RGBA16F): environment maps
	};

	const char* TextureUsageToString(TextureUsage usage);
	std::optional<TextureUsage> TextureUsageFromString(std::string_view text);

	struct TextureImportSettings
	{
		TextureUsage Usage = TextureUsage::Color;
		bool GenerateMips = true;
		TextureFilter Filter = TextureFilter::Linear;
		TextureWrap Wrap = TextureWrap::Repeat;
		uint32_t MaxSize = 0; // Longest side; larger images are downscaled. 0 = engine limit only
		bool FlipVertically = false;

		nlohmann::json ToJson() const;
		// Missing or invalid values keep their defaults (each invalid value adds a warning).
		static TextureImportSettings FromJson(const nlohmann::json& json, std::vector<std::string>* outWarnings = nullptr);
		// Defaults guessed from the file name: ".hdr" files are HDR, "*_normal"/"*_n" normal maps, roughness/metallic/
		// occlusion/mask textures linear data, everything else sRGB color.
		static TextureImportSettings GetDefaults(const std::filesystem::path& path);
	};

	// Imports PNG, JPEG, TGA, BMP, PSD, GIF (first frame) and Radiance HDR images.
	class TextureImporter final : public AssetImporter
	{
	public:
		static constexpr uint32_t c_MaxImageDimension = 16384;

		AssetType GetType() const override { return AssetType::Texture; }
		std::vector<std::string> GetExtensions() const override;
		uint32_t GetVersion() const override { return 1; }
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;

		// Decodes an encoded image into a texture (mip chain included). Thread-safe; used by model importers for
		// embedded images too.
		static Ref<Texture> Decode(std::span<const uint8_t> encoded, const TextureImportSettings& settings, const std::string& debugName,
			std::string* outError = nullptr);
	};

}
