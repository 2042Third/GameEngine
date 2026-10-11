#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Renderer/Texture.h"

#include <atomic>
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
		NormalMap, // Linear; mips are renormalized, X and Y are stored (Z is reconstructed when shading)
		Data,      // Linear: roughness, metallic, occlusion, masks
		HDR        // Floating point (RGBA16F): environment maps
	};

	// Whether the importer block-compresses the texture (the format follows from the usage, see
	// TextureImporter::ChooseFormat) or stores it uncompressed.
	enum class TextureCompression : uint8_t
	{
		Auto = 0,
		Uncompressed
	};

	// How hard the block compressor works on color. High: color is BC7 searched over all of its modes (BC7Encoder). Fast:
	// opaque color is BC1, and translucent color's BC7 tries modes 6 and 5 only. Normal maps, data and HDR are stored the
	// same either way: data's BC7 is always searched in full, as Fast's modes would merge its independent channels.
	enum class TextureQuality : uint8_t
	{
		High = 0,
		Fast
	};

	const char* TextureUsageToString(TextureUsage usage);
	std::optional<TextureUsage> TextureUsageFromString(std::string_view text);
	const char* TextureCompressionToString(TextureCompression compression);
	std::optional<TextureCompression> TextureCompressionFromString(std::string_view text);
	const char* TextureQualityToString(TextureQuality quality);
	std::optional<TextureQuality> TextureQualityFromString(std::string_view text);

	struct TextureImportSettings
	{
		TextureUsage Usage = TextureUsage::Color;
		TextureCompression Compression = TextureCompression::Auto;
		TextureQuality Quality = TextureQuality::High;
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
		uint32_t GetVersion() const override { return 4; }
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		// Usage, MaxSize, Filter, Wrap, GenerateMips, FlipVertically (TextureImportSettings).
		std::vector<PropertyInfo> GetSettingsProperties() const override;
		// Compression has no effect on HDR textures (they stay RGBA16F), Quality none but on color that is compressed.
		std::string GetInactiveReason(std::string_view setting, const nlohmann::json& settings) const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;

		// Decodes an encoded image and cooks it into a texture: resized to MaxSize, each mip filtered from the previous level
		// (a Mitchell filter; sRGB color in linear space with alpha weighting, normal maps renormalized per level) and every
		// level stored in the format ChooseFormat picks (block-compressed on worker threads). Thread-safe; used by model
		// importers for embedded images too. Fails once `cancel` is set (ImportContext::Cancel), checked after decoding,
		// between mip levels and between blocks.
		static Ref<Texture> Decode(std::span<const uint8_t> encoded, const TextureImportSettings& settings, const std::string& debugName,
			std::string* outError = nullptr, const std::atomic<bool>* cancel = nullptr);

		// The stored format of an image with `channels` channels (1 to 4; 2 is gray and alpha) whose alpha is below 255
		// somewhere (`hasAlpha`):
		//   Color      BC7SRGB; at Fast quality BC1SRGB when opaque (uncompressed: RGBA8SRGB)
		//   NormalMap  BC5 (uncompressed: RGBA8)
		//   Data       BC4 for one channel (grayscale), else BC7 (uncompressed: R8 for one channel, else RGBA8)
		//   HDR        RGBA16F
		static TextureFormat ChooseFormat(const TextureImportSettings& settings, uint32_t channels, bool hasAlpha);
	};

}
