#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Renderer/Texture.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
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
		// Inverts green, for normal maps of DirectX's convention (+Y down): only with Usage NormalMap.
		bool FlipGreen = false;
		// Fills the sun's disk of an equirectangular HDR sky with the sky around it (HDRSun::Remove), for a scene whose
		// directional light gives the sun's light instead (lighting.extractSun): only with Usage HDR.
		bool RemoveSun = false;

		nlohmann::json ToJson() const;
		// Missing or invalid values keep their defaults (each invalid value adds a warning).
		static TextureImportSettings FromJson(const nlohmann::json& json, std::vector<std::string>* outWarnings = nullptr);
		// Defaults guessed from the file name: ".hdr" files are HDR; otherwise the stem's tokens (split at '_', '-', '.',
		// spaces and lower-to-upper case changes, lowercased, resolution and convention tokens such as "2k", "1024",
		// "2048x2048", "gl" and "dx" dropped) name normal maps ("nor", "nrm", "normal", ..., "n" as the last token) and data
		// ("rough", "metal", "ao", "orm", "mask", "height", ...), anywhere in the name; everything else is sRGB color. A
		// "dx", "directx" or "normaldx" token turns FlipGreen on.
		static TextureImportSettings GetDefaults(const std::filesystem::path& path);
		// The tokens GetDefaults matches, in order (exposed for tests and messages).
		static std::vector<std::string> GetNameTokens(std::string_view stem, bool* outDirectX = nullptr);
	};

	// Imports PNG, JPEG, TGA, BMP, PSD, GIF (first frame) and Radiance HDR images.
	class TextureImporter final : public AssetImporter
	{
	public:
		static constexpr uint32_t c_MaxImageDimension = 16384;

		AssetType GetType() const override { return AssetType::Texture; }
		std::vector<std::string> GetExtensions() const override;
		// 5: FlipGreen.
		uint32_t GetVersion() const override { return 5; }
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		// Usage, MaxSize, Filter, Wrap, GenerateMips, FlipVertically, FlipGreen, RemoveSun (TextureImportSettings).
		std::vector<PropertyInfo> GetSettingsProperties() const override;
		// Compression has no effect on HDR textures (they stay RGBA16F), Quality none but on color that is compressed, FlipGreen
		// none but on normal maps and RemoveSun none but on HDR textures.
		std::string GetInactiveReason(std::string_view setting, const nlohmann::json& settings) const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;
		// From the image's header: its pixels decoded (4 bytes each) and their working copies (16 bytes each: the float mip
		// chain and the compressor's blocks). See EstimateImageMemory.
		uint64_t EstimateImportMemory(const ImportEstimateSource& source, const nlohmann::json& settings) const override;
		// What an import of an image takes by EstimateImportMemory's count: the `size` bytes at `offset` of `path` (the source
		// or a file it names, read through ReadEstimateBytes), of which `head` holds the first ones already read (or none).
		// A JPEG's frame header follows its metadata segments (EXIF thumbnails, XMP, ICC profiles: up to 64 KiB each), so
		// prefixes eight times larger are read until the header shows, up to the whole image. One whose header never shows
		// (it does not decode either) counts c_UnknownImageFactor times its size: what a compressed image of that size
		// decodes to at a ratio of 10:1, with the working copies.
		static uint64_t EstimateImageMemory(const ImportEstimateSource& source, const std::filesystem::path& path, uint64_t offset, uint64_t size,
			std::span<const uint8_t> head = {});
		static constexpr uint64_t c_UnknownImageFactor = 50;

		// Decodes an encoded image and cooks it into a texture: resized to MaxSize, each mip filtered from the previous level
		// (a Mitchell filter; sRGB color in linear space with alpha weighting, normal maps renormalized per level) and every
		// level stored in the format ChooseFormat picks (block-compressed on worker threads). Thread-safe; used by model
		// importers for embedded images too. Fails once `cancel` is set (ImportContext::Cancel), checked after decoding,
		// between mip levels and between blocks. RemoveSun on an HDR image without a sun adds a warning to `outWarnings`.
		static Ref<Texture> Decode(std::span<const uint8_t> encoded, const TextureImportSettings& settings, const std::string& debugName,
			std::string* outError = nullptr, const std::atomic<bool>* cancel = nullptr, std::vector<std::string>* outWarnings = nullptr);

		// The stored format of an image with `channels` channels (1 to 4; 2 is gray and alpha) whose alpha is below 255
		// somewhere (`hasAlpha`):
		//   Color      BC7SRGB; at Fast quality BC1SRGB when opaque (uncompressed: RGBA8SRGB)
		//   NormalMap  BC5 (uncompressed: RGBA8)
		//   Data       BC4 for one channel (grayscale), else BC7 (uncompressed: R8 for one channel, else RGBA8)
		//   HDR        RGBA16F
		static TextureFormat ChooseFormat(const TextureImportSettings& settings, uint32_t channels, bool hasAlpha);
	};

}
