#include "stpch.h"
#include "Strata/Asset/TextureImporter.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"

#include <glm/gtc/packing.hpp>
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

		void FlipRows(std::vector<uint8_t>& pixels, size_t rowBytes, uint32_t height)
		{
			for (uint32_t row = 0; row < height / 2; row++)
			{
				uint8_t* top = pixels.data() + row * rowBytes;
				uint8_t* bottom = pixels.data() + (height - 1 - row) * rowBytes;
				std::swap_ranges(top, top + rowBytes, bottom);
			}
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

	nlohmann::json TextureImportSettings::ToJson() const
	{
		nlohmann::json json = nlohmann::json::object();
		json["Usage"] = TextureUsageToString(Usage);
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
		int channels = 0;
		if (!stbi_info_from_memory(data, length, &width, &height, &channels))
			return fail(fmt::format("unsupported or corrupt image ({})", GetDecodeFailureReason()));
		if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > c_MaxDecodedPixels)
			return fail(fmt::format("image size {}x{} is not supported", width, height));

		const uint32_t sourceWidth = static_cast<uint32_t>(width);
		const uint32_t sourceHeight = static_cast<uint32_t>(height);
		const glm::uvec2 targetSize = CalculateTargetSize(sourceWidth, sourceHeight, settings.MaxSize);
		const bool resize = targetSize.x != sourceWidth || targetSize.y != sourceHeight;

		TextureSpecification specification;
		specification.Filter = settings.Filter;
		specification.Wrap = settings.Wrap;
		specification.DebugName = debugName;

		std::vector<TextureMip> mips(1);
		mips[0].Width = targetSize.x;
		mips[0].Height = targetSize.y;
		const size_t targetPixels = static_cast<size_t>(targetSize.x) * targetSize.y;

		// Peak memory is the decoded image plus the final level 0: stb's buffer is consumed directly by the resize or
		// the format conversion, never copied in between.
		const int outputWidth = static_cast<int>(targetSize.x);
		const int outputHeight = static_cast<int>(targetSize.y);
		int decodedWidth = 0;
		int decodedHeight = 0;
		if (settings.Usage == TextureUsage::HDR)
		{
			// LDR sources are converted to linear values with the sRGB-like gamma stb_image applies (2.2).
			std::unique_ptr<float, StbImageDeleter> decoded(stbi_loadf_from_memory(data, length, &decodedWidth, &decodedHeight, &channels, 4));
			if (!decoded)
				return fail(fmt::format("decoding failed ({})", GetDecodeFailureReason()));
			if (decodedWidth != width || decodedHeight != height)
				return fail("decoded size differs from the image header");

			std::vector<float> resized;
			const float* pixels = decoded.get();
			if (resize)
			{
				resized.resize(targetPixels * 4);
				if (!stbir_resize_float_linear(decoded.get(), width, height, 0, resized.data(), outputWidth, outputHeight, 0, STBIR_4CHANNEL))
					return fail("resizing failed");
				decoded.reset();
				pixels = resized.data();
			}

			specification.Format = TextureFormat::RGBA16F;
			mips[0].Data.resize(targetPixels * 8);
			for (size_t index = 0; index < targetPixels * 4; index++)
			{
				// Clamp to the half-float range (and drop NaN/negative values some HDR encoders produce).
				const float value = std::isfinite(pixels[index]) ? std::clamp(pixels[index], 0.0f, 65504.0f) : 0.0f;
				const uint16_t half = glm::packHalf1x16(value);
				std::memcpy(mips[0].Data.data() + index * sizeof(half), &half, sizeof(half));
			}
		}
		else
		{
			std::unique_ptr<stbi_uc, StbImageDeleter> decoded(stbi_load_from_memory(data, length, &decodedWidth, &decodedHeight, &channels, 4));
			if (!decoded)
				return fail(fmt::format("decoding failed ({})", GetDecodeFailureReason()));
			if (decodedWidth != width || decodedHeight != height)
				return fail("decoded size differs from the image header");

			const bool srgb = settings.Usage == TextureUsage::Color;
			specification.Format = srgb ? TextureFormat::RGBA8SRGB : TextureFormat::RGBA8;
			if (resize)
			{
				mips[0].Data.resize(targetPixels * 4);
				// Color is filtered in linear space with alpha weighting; data and normals channel by channel.
				const bool success = srgb
					? stbir_resize_uint8_srgb(decoded.get(), width, height, 0, mips[0].Data.data(), outputWidth, outputHeight, 0, STBIR_RGBA) != nullptr
					: stbir_resize_uint8_linear(decoded.get(), width, height, 0, mips[0].Data.data(), outputWidth, outputHeight, 0, STBIR_4CHANNEL) != nullptr;
				if (!success)
					return fail("resizing failed");
			}
			else
			{
				mips[0].Data.assign(decoded.get(), decoded.get() + targetPixels * 4);
			}
		}

		if (settings.FlipVertically)
			FlipRows(mips[0].Data, static_cast<size_t>(targetSize.x) * GetTextureFormatBytesPerPixel(specification.Format), targetSize.y);

		if (settings.GenerateMips && !TextureUtils::GenerateMips(mips, specification.Format, settings.Usage == TextureUsage::NormalMap))
			return fail("mip generation failed");

		std::string error;
		Ref<Texture> texture = Texture::Create(specification, std::move(mips), &error);
		if (!texture)
			return fail(error);
		return texture;
	}

}
