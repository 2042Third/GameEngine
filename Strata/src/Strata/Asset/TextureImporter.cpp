#include "stpch.h"
#include "Strata/Asset/TextureImporter.h"

#include "Strata/Asset/TextureCooker.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <cctype>
#include <climits>

namespace Strata
{

	namespace
	{

		// Decoding larger images would need more memory than any texture the GPU accepts is worth.
		constexpr uint64_t c_MaxDecodedPixels = 1ull << 28;

		const char* TextureFilterToString(TextureFilter filter)
		{
			return filter == TextureFilter::Nearest ? "Nearest" : "Linear";
		}

		std::optional<TextureFilter> TextureFilterFromString(std::string_view text)
		{
			if (text == "Linear")
				return TextureFilter::Linear;
			if (text == "Nearest")
				return TextureFilter::Nearest;
			return std::nullopt;
		}

		const char* TextureWrapToString(TextureWrap wrap)
		{
			switch (wrap)
			{
				case TextureWrap::Repeat: return "Repeat";
				case TextureWrap::Clamp:  return "Clamp";
				case TextureWrap::Mirror: return "Mirror";
			}
			return "Repeat";
		}

		std::optional<TextureWrap> TextureWrapFromString(std::string_view text)
		{
			if (text == "Repeat")
				return TextureWrap::Repeat;
			if (text == "Clamp")
				return TextureWrap::Clamp;
			if (text == "Mirror")
				return TextureWrap::Mirror;
			return std::nullopt;
		}

		const char* GetDecodeFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason ? reason : "unknown error";
		}

		struct StbImageDeleter
		{
			void operator()(void* pixels) const { stbi_image_free(pixels); }
		};

		template<typename T>
		void FlipRows(std::vector<T>& pixels, size_t rowElements, uint32_t height)
		{
			for (uint32_t row = 0; row < height / 2; row++)
			{
				T* top = pixels.data() + row * rowElements;
				T* bottom = pixels.data() + (height - 1 - row) * rowElements;
				std::swap_ranges(top, top + rowElements, bottom);
			}
		}

		// The channels to decode an 8-bit image with `sourceChannels` into for a usage (see TextureImporter::Decode).
		uint32_t GetDecodedChannels(TextureUsage usage, uint32_t sourceChannels)
		{
			const bool alpha = sourceChannels == 2 || sourceChannels == 4;
			switch (usage)
			{
				case TextureUsage::Color:     return alpha ? 4 : 3;
				case TextureUsage::NormalMap: return 3;
				case TextureUsage::Data:      return sourceChannels == 1 ? 1 : (alpha ? 4 : 3);
				case TextureUsage::HDR:       return 4;
			}
			return 4;
		}

		// Whether an RGBA image has a texel that is not fully opaque.
		bool HasTranslucentTexels(const TextureCooker::Image& image)
		{
			for (size_t index = 3; index < image.Pixels.size(); index += 4)
			{
				if (image.Pixels[index] != 255)
					return true;
			}
			return false;
		}

		// Size after applying the size limit, preserving the aspect ratio.
		glm::uvec2 CalculateTargetSize(uint32_t width, uint32_t height, uint32_t maxSize)
		{
			const uint32_t limit = maxSize == 0 ? TextureImporter::c_MaxImageDimension : std::min(maxSize, TextureImporter::c_MaxImageDimension);
			const uint32_t longest = std::max(width, height);
			if (longest <= limit)
				return glm::uvec2(width, height);

			const double scale = static_cast<double>(limit) / static_cast<double>(longest);
			return glm::uvec2(std::max(1u, static_cast<uint32_t>(std::lround(width * scale))), std::max(1u, static_cast<uint32_t>(std::lround(height * scale))));
		}

	}

	const char* TextureUsageToString(TextureUsage usage)
	{
		switch (usage)
		{
			case TextureUsage::Color:     return "Color";
			case TextureUsage::NormalMap: return "NormalMap";
			case TextureUsage::Data:      return "Data";
			case TextureUsage::HDR:       return "HDR";
		}
		return "Color";
	}

	std::optional<TextureUsage> TextureUsageFromString(std::string_view text)
	{
		for (TextureUsage usage : { TextureUsage::Color, TextureUsage::NormalMap, TextureUsage::Data, TextureUsage::HDR })
		{
			if (text == TextureUsageToString(usage))
				return usage;
		}
		return std::nullopt;
	}

	const char* TextureCompressionToString(TextureCompression compression)
	{
		return compression == TextureCompression::Uncompressed ? "Uncompressed" : "Auto";
	}

	std::optional<TextureCompression> TextureCompressionFromString(std::string_view text)
	{
		if (text == "Auto")
			return TextureCompression::Auto;
		if (text == "Uncompressed")
			return TextureCompression::Uncompressed;
		return std::nullopt;
	}

	const char* TextureQualityToString(TextureQuality quality)
	{
		return quality == TextureQuality::Fast ? "Fast" : "High";
	}

	std::optional<TextureQuality> TextureQualityFromString(std::string_view text)
	{
		if (text == "High")
			return TextureQuality::High;
		if (text == "Fast")
			return TextureQuality::Fast;
		return std::nullopt;
	}

	nlohmann::json TextureImportSettings::ToJson() const
	{
		nlohmann::json json = nlohmann::json::object();
		json["Usage"] = TextureUsageToString(Usage);
		json["Compression"] = TextureCompressionToString(Compression);
		json["Quality"] = TextureQualityToString(Quality);
		json["GenerateMips"] = GenerateMips;
		json["Filter"] = TextureFilterToString(Filter);
		json["Wrap"] = TextureWrapToString(Wrap);
		json["MaxSize"] = MaxSize;
		json["FlipVertically"] = FlipVertically;
		return json;
	}

	TextureImportSettings TextureImportSettings::FromJson(const nlohmann::json& json, std::vector<std::string>* outWarnings)
	{
		TextureImportSettings settings;
		if (!json.is_object())
			return settings;

		auto warn = [outWarnings](const std::string& message)
		{
			if (outWarnings)
				outWarnings->push_back(message);
		};

		if (const nlohmann::json* usage = JsonUtils::Find(json, "Usage"))
		{
			std::optional<TextureUsage> value = usage->is_string() ? TextureUsageFromString(usage->get<std::string>()) : std::nullopt;
			if (value)
				settings.Usage = *value;
			else
				warn("Unknown texture usage; expected Color, NormalMap, Data or HDR");
		}
		if (const nlohmann::json* compression = JsonUtils::Find(json, "Compression"))
		{
			std::optional<TextureCompression> value = compression->is_string() ? TextureCompressionFromString(compression->get<std::string>()) : std::nullopt;
			if (value)
				settings.Compression = *value;
			else
				warn("Unknown texture compression; expected Auto or Uncompressed");
		}
		if (const nlohmann::json* quality = JsonUtils::Find(json, "Quality"))
		{
			std::optional<TextureQuality> value = quality->is_string() ? TextureQualityFromString(quality->get<std::string>()) : std::nullopt;
			if (value)
				settings.Quality = *value;
			else
				warn("Unknown texture compression quality; expected High or Fast");
		}
		if (const nlohmann::json* filter = JsonUtils::Find(json, "Filter"))
		{
			std::optional<TextureFilter> value = filter->is_string() ? TextureFilterFromString(filter->get<std::string>()) : std::nullopt;
			if (value)
				settings.Filter = *value;
			else
				warn("Unknown texture filter; expected Linear or Nearest");
		}
		if (const nlohmann::json* wrap = JsonUtils::Find(json, "Wrap"))
		{
			std::optional<TextureWrap> value = wrap->is_string() ? TextureWrapFromString(wrap->get<std::string>()) : std::nullopt;
			if (value)
				settings.Wrap = *value;
			else
				warn("Unknown texture wrap mode; expected Repeat, Clamp or Mirror");
		}

		settings.GenerateMips = JsonUtils::GetBool(json, "GenerateMips", settings.GenerateMips);
		settings.FlipVertically = JsonUtils::GetBool(json, "FlipVertically", settings.FlipVertically);
		const uint64_t maxSize = JsonUtils::GetUInt(json, "MaxSize", settings.MaxSize);
		if (maxSize > TextureImporter::c_MaxImageDimension)
			warn(fmt::format("MaxSize {} exceeds the engine limit of {}", maxSize, TextureImporter::c_MaxImageDimension));
		settings.MaxSize = static_cast<uint32_t>(std::min<uint64_t>(maxSize, TextureImporter::c_MaxImageDimension));
		return settings;
	}

	TextureImportSettings TextureImportSettings::GetDefaults(const std::filesystem::path& path)
	{
		TextureImportSettings settings;
		const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(path.extension()));
		if (extension == ".hdr")
		{
			settings.Usage = TextureUsage::HDR;
			settings.Wrap = TextureWrap::Clamp;
			return settings;
		}

		const std::string stem = StringUtils::ToLower(FileSystem::ToUTF8(path.stem()));
		for (std::string_view suffix : { "_n", "_nrm", "_nor", "_norm", "_normal", "-normal", "_normalgl", "_normalmap" })
		{
			if (StringUtils::EndsWith(stem, suffix))
			{
				settings.Usage = TextureUsage::NormalMap;
				return settings;
			}
		}
		if (stem.find("normal") != std::string::npos)
		{
			settings.Usage = TextureUsage::NormalMap;
			return settings;
		}

		for (std::string_view token : { "rough", "metal", "occlusion", "_ao", "_orm", "_arm", "_rma", "mask", "height", "displacement", "_disp", "_spec" })
		{
			if (stem.find(token) != std::string::npos)
			{
				settings.Usage = TextureUsage::Data;
				return settings;
			}
		}
		return settings;
	}

	std::vector<std::string> TextureImporter::GetExtensions() const
	{
		return { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr" };
	}

	nlohmann::json TextureImporter::GetDefaultSettings(const std::filesystem::path& sourcePath) const
	{
		return TextureImportSettings::GetDefaults(sourcePath).ToJson();
	}

	std::string TextureImporter::GetInactiveReason(std::string_view setting, const nlohmann::json& settings) const
	{
		const TextureImportSettings values = TextureImportSettings::FromJson(settings);
		if (setting == "Compression" && values.Usage == TextureUsage::HDR)
			return "HDR textures stay 16-bit float: they are not block-compressed yet";
		if (setting == "Quality")
		{
			if (values.Usage == TextureUsage::HDR)
				return "HDR textures stay 16-bit float: they are not block-compressed yet";
			if (values.Compression == TextureCompression::Uncompressed)
				return "Quality only chooses how block compression encodes; with Compression Uncompressed every texel is kept exact";
			if (values.Usage != TextureUsage::Color)
				return "Normal maps and data are stored the same at either quality";
		}
		return {};
	}

	std::vector<PropertyInfo> TextureImporter::GetSettingsProperties() const
	{
		const TextureImportSettings defaults;
		std::vector<PropertyInfo> properties;
		ImportSettingsBuilder(properties)
			.Enum("Usage", { "Color", "NormalMap", "Data", "HDR" }, TextureUsageToString(defaults.Usage))
				.Tooltip("How the pixels are read: Color (sRGB albedo, emissive, UI), NormalMap (linear, mips renormalized), Data (linear roughness, "
					"metallic, occlusion, masks) or HDR (16-bit float, environment maps)")
			.Enum("Compression", { "Auto", "Uncompressed" }, TextureCompressionToString(defaults.Compression))
				.Tooltip("Auto stores the texture block-compressed in the format its usage needs (BC7 for color and for data of several "
					"channels, BC4 for one channel, BC5 for normal maps; HDR stays 16-bit float); Uncompressed keeps every texel exact "
					"(pixel art, textures compared texel for texel)")
			.Enum("Quality", { "High", "Fast" }, TextureQualityToString(defaults.Quality))
				.Tooltip("How hard compression works on color: High searches every BC7 mode; Fast stores opaque color as BC1 (half the "
					"bytes, a little less exact) and imports large textures several times faster. Normal maps and data are stored the same "
					"either way")
			.UInt("MaxSize", defaults.MaxSize, c_MaxImageDimension).PowerOfTwoLimit()
				.Tooltip("Longest side in pixels: larger images are scaled down, keeping their aspect ratio. No limit (0): only the engine's limit")
			.Enum("Filter", { "Linear", "Nearest" }, TextureFilterToString(defaults.Filter))
				.Tooltip("Linear blends neighboring texels; Nearest keeps them sharp (pixel art)")
			.Enum("Wrap", { "Repeat", "Clamp", "Mirror" }, TextureWrapToString(defaults.Wrap))
				.Tooltip("What texture coordinates outside 0..1 show: the texture repeated, its edge, or the texture mirrored")
			.Bool("GenerateMips", defaults.GenerateMips)
				.Tooltip("Store the smaller versions of the image that keep distant surfaces from shimmering")
			.Bool("FlipVertically", defaults.FlipVertically)
				.Tooltip("Store the image upside down (for images whose rows start at the bottom)");
		return properties;
	}

	bool TextureImporter::Import(const ImportContext& context, ImportResult& result, std::string* outError) const
	{
		std::optional<std::vector<uint8_t>> encoded = FileSystem::ReadBytes(context.SourcePath);
		if (!encoded)
		{
			if (outError)
				*outError = fmt::format("Could not read '{}'", FileSystem::ToUTF8(context.SourcePath));
			return false;
		}

		const TextureImportSettings settings = TextureImportSettings::FromJson(context.Settings, &result.Warnings);
		Ref<Texture> texture = Decode(*encoded, settings, FileSystem::ToUTF8(context.SourcePath.filename()), outError);
		if (!texture)
			return false;

		result.Data = texture->Serialize();
		return true;
	}

	Ref<Texture> TextureImporter::Decode(std::span<const uint8_t> encoded, const TextureImportSettings& settings, const std::string& debugName, std::string* outError)
	{
		auto fail = [outError, &debugName](const std::string& message) -> Ref<Texture>
		{
			if (outError)
				*outError = fmt::format("{}: {}", debugName, message);
			return nullptr;
		};

		if (encoded.empty() || encoded.size() > static_cast<size_t>(INT_MAX))
			return fail("image data is empty or too large");

		const stbi_uc* data = encoded.data();
		const int length = static_cast<int>(encoded.size());
		int width = 0;
		int height = 0;
		int sourceChannels = 0;
		if (!stbi_info_from_memory(data, length, &width, &height, &sourceChannels))
			return fail(fmt::format("unsupported or corrupt image ({})", GetDecodeFailureReason()));
		if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > c_MaxDecodedPixels)
			return fail(fmt::format("image size {}x{} is not supported", width, height));

		const uint32_t sourceWidth = static_cast<uint32_t>(width);
		const uint32_t sourceHeight = static_cast<uint32_t>(height);
		const glm::uvec2 targetSize = CalculateTargetSize(sourceWidth, sourceHeight, settings.MaxSize);
		const bool resize = targetSize.x != sourceWidth || targetSize.y != sourceHeight;
		const int outputWidth = static_cast<int>(targetSize.x);
		const int outputHeight = static_cast<int>(targetSize.y);
		const size_t targetPixels = static_cast<size_t>(targetSize.x) * targetSize.y;

		TextureSpecification specification;
		specification.Filter = settings.Filter;
		specification.Wrap = settings.Wrap;
		specification.DebugName = debugName;
		std::vector<TextureMip> mips;
		std::string error;

		// Peak memory is the decoded image plus the final level 0 and the levels being cooked: stb's buffer is consumed
		// directly by the resize, never copied in between.
		int decodedWidth = 0;
		int decodedHeight = 0;
		int ignoredChannels = 0;
		if (settings.Usage == TextureUsage::HDR)
		{
			// LDR sources are converted to linear values with the sRGB-like gamma stb_image applies (2.2).
			std::unique_ptr<float, StbImageDeleter> decoded(stbi_loadf_from_memory(data, length, &decodedWidth, &decodedHeight, &ignoredChannels, 4));
			if (!decoded)
				return fail(fmt::format("decoding failed ({})", GetDecodeFailureReason()));
			if (decodedWidth != width || decodedHeight != height)
				return fail("decoded size differs from the image header");

			TextureCooker::FloatImage level0;
			level0.Width = targetSize.x;
			level0.Height = targetSize.y;
			if (resize)
			{
				level0.Pixels.resize(targetPixels * 4);
				if (!stbir_resize_float_linear(decoded.get(), width, height, 0, level0.Pixels.data(), outputWidth, outputHeight, 0, STBIR_4CHANNEL))
					return fail("resizing failed");
			}
			else
			{
				level0.Pixels.assign(decoded.get(), decoded.get() + targetPixels * 4);
			}
			decoded.reset();
			if (settings.FlipVertically)
				FlipRows(level0.Pixels, static_cast<size_t>(targetSize.x) * 4, targetSize.y);

			specification.Format = TextureFormat::RGBA16F;
			if (!TextureCooker::CookHDRLevels(std::move(level0), settings.GenerateMips, settings.Wrap, mips, &error))
				return fail(error);
		}
		else
		{
			// Only the channels the stored format needs are decoded: color keeps RGB or RGBA (gray becomes RGB, as sRGB
			// formats are color), normal maps XYZ, data its own channels (gray and alpha become RGBA).
			const uint32_t channels = GetDecodedChannels(settings.Usage, static_cast<uint32_t>(sourceChannels));
			std::unique_ptr<stbi_uc, StbImageDeleter> decoded(stbi_load_from_memory(data, length, &decodedWidth, &decodedHeight, &ignoredChannels,
				static_cast<int>(channels)));
			if (!decoded)
				return fail(fmt::format("decoding failed ({})", GetDecodeFailureReason()));
			if (decodedWidth != width || decodedHeight != height)
				return fail("decoded size differs from the image header");

			const bool color = settings.Usage == TextureUsage::Color;
			TextureCooker::Image level0;
			level0.Width = targetSize.x;
			level0.Height = targetSize.y;
			level0.Channels = channels;
			if (resize)
			{
				level0.Pixels.resize(targetPixels * channels);
				// Color is filtered in linear space with alpha weighting; data and normals channel by channel.
				const stbir_pixel_layout layout = channels == 1 ? STBIR_1CHANNEL : channels == 3 ? STBIR_RGB : (color ? STBIR_RGBA : STBIR_4CHANNEL);
				const bool success = color
					? stbir_resize_uint8_srgb(decoded.get(), width, height, 0, level0.Pixels.data(), outputWidth, outputHeight, 0, layout) != nullptr
					: stbir_resize_uint8_linear(decoded.get(), width, height, 0, level0.Pixels.data(), outputWidth, outputHeight, 0, layout) != nullptr;
				if (!success)
					return fail("resizing failed");
			}
			else
			{
				level0.Pixels.assign(decoded.get(), decoded.get() + targetPixels * channels);
			}
			decoded.reset();
			if (settings.FlipVertically)
				FlipRows(level0.Pixels, static_cast<size_t>(targetSize.x) * channels, targetSize.y);

			const bool hasAlpha = channels == 4 && HasTranslucentTexels(level0);
			specification.Format = ChooseFormat(settings, channels, hasAlpha);
			specification.Grayscale = GetTextureFormatInfo(specification.Format).Channels == 1;

			TextureCooker::CookSettings cook;
			cook.Format = specification.Format;
			cook.GenerateMips = settings.GenerateMips;
			cook.SRGB = color;
			cook.AlphaWeighted = color && hasAlpha;
			cook.NormalMap = settings.Usage == TextureUsage::NormalMap;
			cook.Wrap = settings.Wrap;
			// Fast's BC7 tries only the single-subset modes (6, and 5 when alpha varies), which keep a block's colors on one
			// line through RGB space as BC1 does. Color mostly follows such lines; the channels of data vary independently,
			// and a metallic edge would shift the roughness beside it (see ChooseFormat), so data is searched in full at
			// either quality.
			cook.HighQuality = settings.Quality == TextureQuality::High || settings.Usage == TextureUsage::Data;
			if (!TextureCooker::CookLevels(std::move(level0), cook, mips, &error))
				return fail(error);
		}

		Ref<Texture> texture = Texture::Create(specification, std::move(mips), &error);
		if (!texture)
			return fail(error);
		return texture;
	}

	TextureFormat TextureImporter::ChooseFormat(const TextureImportSettings& settings, uint32_t channels, bool hasAlpha)
	{
		const bool compress = settings.Compression == TextureCompression::Auto;
		switch (settings.Usage)
		{
			case TextureUsage::Color:
				if (!compress)
					return TextureFormat::RGBA8SRGB;
				// Fast trades quality for time and size where it can: opaque color as BC1 (half of BC7's bytes, colors on one
				// line per block). Alpha needs BC7 (BC3's color is BC1's).
				return settings.Quality == TextureQuality::Fast && !hasAlpha ? TextureFormat::BC1SRGB : TextureFormat::BC7SRGB;
			case TextureUsage::NormalMap:
				return compress ? TextureFormat::BC5 : TextureFormat::RGBA8;
			case TextureUsage::Data:
				// The channels of data vary independently (occlusion, roughness and metallic in one texture), which BC1 and
				// BC3 cannot store (the colors of one of their blocks lie on a line through RGB space, so a metallic edge
				// inside a block would shift the roughness next to it by up to a sixth); BC7 can, with partitions and a
				// channel stored apart. One channel is BC4.
				if (channels == 1)
					return compress ? TextureFormat::BC4 : TextureFormat::R8;
				return compress ? TextureFormat::BC7 : TextureFormat::RGBA8;
			case TextureUsage::HDR:
				return TextureFormat::RGBA16F;
		}
		return TextureFormat::RGBA8;
	}

}
