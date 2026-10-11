#include "stpch.h"
#include "Strata/Asset/TextureCooker.h"

#include "Strata/Asset/BC7Encoder.h"
#include "Strata/Core/JobSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <stb_dxt.h>
#include <stb_image_resize2.h>

namespace Strata::TextureCooker
{

	namespace
	{

		// Blocks (or, for uncompressed formats, texels) a job encodes at a time: enough to outweigh scheduling. BC7 blocks
		// take far longer than the others (a search over modes and partitions), so their batches are smaller.
		constexpr uint32_t c_BlocksPerBatch = 4096;
		constexpr uint32_t c_BC7BlocksPerBatch = 256;
		constexpr uint32_t c_TexelsPerBatch = 1u << 16;

		stbir_edge ToEdgeMode(TextureWrap wrap)
		{
			switch (wrap)
			{
				case TextureWrap::Repeat: return STBIR_EDGE_WRAP;
				case TextureWrap::Clamp:  return STBIR_EDGE_CLAMP;
				case TextureWrap::Mirror: return STBIR_EDGE_REFLECT;
			}
			return STBIR_EDGE_CLAMP;
		}

		stbir_pixel_layout GetPixelLayout(uint32_t channels, bool alphaWeighted)
		{
			switch (channels)
			{
				case 1:  return STBIR_1CHANNEL;
				case 2:  return alphaWeighted ? STBIR_RA : STBIR_2CHANNEL;
				case 3:  return STBIR_RGB;
				default: return alphaWeighted ? STBIR_RGBA : STBIR_4CHANNEL;
			}
		}

		bool IsValidImage(const Image& image)
		{
			return image.Width > 0 && image.Height > 0 && image.Channels >= 1 && image.Channels <= 4
				&& image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * image.Channels;
		}

		// The texel at (x, y), clamped into the image, as RGBA: gray fills red, green and blue, a missing alpha is opaque.
		void LoadRGBA(const Image& image, uint32_t x, uint32_t y, uint8_t* rgba)
		{
			x = std::min(x, image.Width - 1);
			y = std::min(y, image.Height - 1);
			const uint8_t* texel = image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * image.Channels;
			switch (image.Channels)
			{
				case 1:
					rgba[0] = rgba[1] = rgba[2] = texel[0];
					rgba[3] = 255;
					break;
				case 2:
					rgba[0] = rgba[1] = rgba[2] = texel[0];
					rgba[3] = texel[1];
					break;
				case 3:
					rgba[0] = texel[0];
					rgba[1] = texel[1];
					rgba[2] = texel[2];
					rgba[3] = 255;
					break;
				default:
					std::memcpy(rgba, texel, 4);
					break;
			}
		}

		// The next smaller level of a chain, filtered from `source`.
		bool Halve(const Image& source, const CookSettings& settings, Image& outImage)
		{
			outImage.Width = std::max(1u, source.Width / 2);
			outImage.Height = std::max(1u, source.Height / 2);
			outImage.Channels = source.Channels;
			outImage.Pixels.resize(static_cast<size_t>(outImage.Width) * outImage.Height * outImage.Channels);

			STBIR_RESIZE resize;
			stbir_resize_init(&resize, source.Pixels.data(), static_cast<int>(source.Width), static_cast<int>(source.Height), 0, outImage.Pixels.data(),
				static_cast<int>(outImage.Width), static_cast<int>(outImage.Height), 0, GetPixelLayout(source.Channels, settings.AlphaWeighted),
				settings.SRGB ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8);
			const stbir_edge edge = ToEdgeMode(settings.Wrap);
			stbir_set_edgemodes(&resize, edge, edge);
			stbir_set_filters(&resize, STBIR_FILTER_MITCHELL, STBIR_FILTER_MITCHELL);
			return stbir_resize_extended(&resize) != 0;
		}

		bool HalveHDR(const FloatImage& source, TextureWrap wrap, FloatImage& outImage)
		{
			outImage.Width = std::max(1u, source.Width / 2);
			outImage.Height = std::max(1u, source.Height / 2);
			outImage.Pixels.resize(static_cast<size_t>(outImage.Width) * outImage.Height * 4);

			STBIR_RESIZE resize;
			stbir_resize_init(&resize, source.Pixels.data(), static_cast<int>(source.Width), static_cast<int>(source.Height), 0, outImage.Pixels.data(),
				static_cast<int>(outImage.Width), static_cast<int>(outImage.Height), 0, STBIR_4CHANNEL, STBIR_TYPE_FLOAT);
			const stbir_edge edge = ToEdgeMode(wrap);
			stbir_set_edgemodes(&resize, edge, edge);
			stbir_set_filters(&resize, STBIR_FILTER_BOX, STBIR_FILTER_BOX);
			return stbir_resize_extended(&resize) != 0;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// How EncodeLevel hands a level to JobSystem::ParallelFor: rows of blocks, or texels for uncompressed formats.
		struct EncodeWork
		{
			uint32_t Count = 0;
			uint32_t MinBatchSize = 1;
		};

		EncodeWork GetEncodeWork(TextureFormat format, uint32_t width, uint32_t height)
		{
			const TextureFormatInfo info = GetTextureFormatInfo(format);
			if (!info.IsBlockCompressed)
				return EncodeWork { width * height, c_TexelsPerBatch };
			const uint32_t blocksX = (width + info.BlockWidth - 1) / info.BlockWidth;
			const uint32_t blocksY = (height + info.BlockHeight - 1) / info.BlockHeight;
			const bool bc7 = format == TextureFormat::BC7 || format == TextureFormat::BC7SRGB;
			return EncodeWork { blocksY, std::max(1u, (bc7 ? c_BC7BlocksPerBatch : c_BlocksPerBatch) / blocksX) };
		}

	}

	uint32_t GetEncodeBatchCount(TextureFormat format, uint32_t width, uint32_t height)
	{
		if (!CanEncode(format) || width == 0 || height == 0)
			return 0;
		const EncodeWork work = GetEncodeWork(format, width, height);
		return (work.Count + work.MinBatchSize - 1) / work.MinBatchSize;
	}

	bool CanEncode(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8:
			case TextureFormat::RGBA8SRGB:
			case TextureFormat::R8:
			case TextureFormat::RG8:
			case TextureFormat::BC1:
			case TextureFormat::BC1SRGB:
			case TextureFormat::BC3:
			case TextureFormat::BC3SRGB:
			case TextureFormat::BC4:
			case TextureFormat::BC5:
			case TextureFormat::BC7:
			case TextureFormat::BC7SRGB:
				return true;
			default:
				return false;
		}
	}

	bool EncodeLevel(const Image& image, TextureFormat format, bool highQuality, std::vector<uint8_t>& outData)
	{
		if (!CanEncode(format) || !IsValidImage(image))
			return false;

		const TextureFormatInfo info = GetTextureFormatInfo(format);
		outData.resize(static_cast<size_t>(GetMipByteSize(format, image.Width, image.Height)));
		const EncodeWork work = GetEncodeWork(format, image.Width, image.Height);
		if (!info.IsBlockCompressed)
		{
			// Each texel's RGBA, cut to the channels the format stores. Imports are background work (JobPriority::Background):
			// their helpers never take the processors from the frame.
			JobSystem::ParallelFor(work.Count, work.MinBatchSize, [&](uint32_t begin, uint32_t end)
			{
				uint8_t rgba[4];
				for (uint32_t texel = begin; texel < end; texel++)
				{
					LoadRGBA(image, texel % image.Width, texel / image.Width, rgba);
					std::memcpy(outData.data() + static_cast<size_t>(texel) * info.BytesPerBlock, rgba, info.BytesPerBlock);
				}
			}, JobPriority::Background);
			return true;
		}

		const uint32_t blocksX = (image.Width + info.BlockWidth - 1) / info.BlockWidth;
		const int mode = highQuality ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL;
		BC7Encoder::EncodeSettings bc7Settings;
		bc7Settings.HighQuality = highQuality;
		JobSystem::ParallelFor(work.Count, work.MinBatchSize, [&](uint32_t begin, uint32_t end)
		{
			uint8_t rgba[16 * 4];
			uint8_t channels[16 * 2];
			for (uint32_t blockY = begin; blockY < end; blockY++)
			{
				for (uint32_t blockX = 0; blockX < blocksX; blockX++)
				{
					uint8_t* destination = outData.data() + (static_cast<size_t>(blockY) * blocksX + blockX) * info.BytesPerBlock;
					for (uint32_t texel = 0; texel < 16; texel++)
						LoadRGBA(image, blockX * 4 + texel % 4, blockY * 4 + texel / 4, rgba + texel * 4);
					switch (format)
					{
						case TextureFormat::BC1:
						case TextureFormat::BC1SRGB:
							// The encoder reads alpha even when it does not store it: it must be constant.
							for (uint32_t texel = 0; texel < 16; texel++)
								rgba[texel * 4 + 3] = 255;
							stb_compress_dxt_block(destination, rgba, 0, mode);
							break;
						case TextureFormat::BC3:
						case TextureFormat::BC3SRGB:
							stb_compress_dxt_block(destination, rgba, 1, mode);
							break;
						case TextureFormat::BC4:
							for (uint32_t texel = 0; texel < 16; texel++)
								channels[texel] = rgba[texel * 4];
							stb_compress_bc4_block(destination, channels);
							break;
						case TextureFormat::BC5:
							for (uint32_t texel = 0; texel < 16; texel++)
							{
								channels[texel * 2] = rgba[texel * 4];
								channels[texel * 2 + 1] = rgba[texel * 4 + 1];
							}
							stb_compress_bc5_block(destination, channels);
							break;
						case TextureFormat::BC7:
						case TextureFormat::BC7SRGB:
							BC7Encoder::EncodeBlock(rgba, destination, bc7Settings);
							break;
						default:
							break;
					}
				}
			}
		}, JobPriority::Background);
		return true;
	}

	void NormalizeNormalMap(Image& image)
	{
		if (image.Channels < 3)
			return;
		const size_t texelCount = static_cast<size_t>(image.Width) * image.Height;
		for (size_t texel = 0; texel < texelCount; texel++)
		{
			uint8_t* pixel = image.Pixels.data() + texel * image.Channels;
			glm::vec3 normal = glm::vec3(pixel[0], pixel[1], pixel[2]) / 255.0f * 2.0f - 1.0f;
			const float length = glm::length(normal);
			normal = length > 1e-6f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f);
			for (int channel = 0; channel < 3; channel++)
				pixel[channel] = static_cast<uint8_t>(std::clamp(std::lround((normal[channel] * 0.5f + 0.5f) * 255.0f), 0l, 255l));
		}
	}

	bool CookLevels(Image level0, const CookSettings& settings, std::vector<TextureMip>& outMips, std::string* outError)
	{
		if (!CanEncode(settings.Format))
			return Fail(outError, fmt::format("textures cannot be encoded as {}", TextureFormatToString(settings.Format)));
		if (!IsValidImage(level0))
			return Fail(outError, "the image is empty or its size does not match its pixels");

		const uint32_t mipCount = settings.GenerateMips ? TextureUtils::CalculateMipCount(level0.Width, level0.Height) : 1;
		outMips.clear();
		outMips.reserve(mipCount);
		Image current = std::move(level0);
		if (settings.NormalMap)
			NormalizeNormalMap(current);
		for (uint32_t level = 0; level < mipCount; level++)
		{
			TextureMip mip;
			mip.Width = current.Width;
			mip.Height = current.Height;
			if (!EncodeLevel(current, settings.Format, settings.HighQuality, mip.Data))
				return Fail(outError, fmt::format("encoding mip {} failed", level));
			outMips.push_back(std::move(mip));
			if (level + 1 == mipCount)
				break;

			Image next;
			if (!Halve(current, settings, next))
				return Fail(outError, fmt::format("filtering mip {} failed", level + 1));
			if (settings.NormalMap)
				NormalizeNormalMap(next);
			current = std::move(next);
		}
		return true;
	}

	bool CookHDRLevels(FloatImage level0, bool generateMips, TextureWrap wrap, std::vector<TextureMip>& outMips, std::string* outError)
	{
		if (level0.Width == 0 || level0.Height == 0 || level0.Pixels.size() != static_cast<size_t>(level0.Width) * level0.Height * 4)
			return Fail(outError, "the image is empty or its size does not match its pixels");

		const uint32_t mipCount = generateMips ? TextureUtils::CalculateMipCount(level0.Width, level0.Height) : 1;
		outMips.clear();
		outMips.reserve(mipCount);
		FloatImage current = std::move(level0);
		// Clamp to the half-float range and drop NaN and negative values some HDR encoders produce, before filtering spreads
		// them.
		for (float& value : current.Pixels)
			value = std::isfinite(value) ? std::clamp(value, 0.0f, 65504.0f) : 0.0f;
		for (uint32_t level = 0; level < mipCount; level++)
		{
			TextureMip mip;
			mip.Width = current.Width;
			mip.Height = current.Height;
			mip.Data.resize(current.Pixels.size() * sizeof(uint16_t));
			for (size_t index = 0; index < current.Pixels.size(); index++)
			{
				const uint16_t half = glm::packHalf1x16(std::clamp(current.Pixels[index], 0.0f, 65504.0f));
				std::memcpy(mip.Data.data() + index * sizeof(half), &half, sizeof(half));
			}
			outMips.push_back(std::move(mip));
			if (level + 1 == mipCount)
				break;

			FloatImage next;
			if (!HalveHDR(current, wrap, next))
				return Fail(outError, fmt::format("filtering mip {} failed", level + 1));
			current = std::move(next);
		}
		return true;
	}

}
