#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Asset/TextureCooker.h"
#include "Strata/Renderer/BlockCompression.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "TestHelpers.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Random blocks of a block-compressed format: every encoding the format allows, the hardest test of a decoder. BC7
	// blocks cycle through the eight modes (random bytes would be mode 0 half of the time).
	std::vector<uint8_t> CreateRandomBlocks(TextureFormat format, uint32_t width, uint32_t height, uint32_t seed)
	{
		std::mt19937 random(seed);
		std::vector<uint8_t> blocks(static_cast<size_t>(GetMipByteSize(format, width, height)));
		for (uint8_t& byte : blocks)
			byte = static_cast<uint8_t>(random());
		if (format == TextureFormat::BC7 || format == TextureFormat::BC7SRGB)
		{
			for (size_t block = 0; block < blocks.size() / 16; block++)
			{
				const uint32_t mode = static_cast<uint32_t>(block % 8);
				uint8_t& first = blocks[block * 16];
				first = static_cast<uint8_t>((static_cast<uint32_t>(first) << (mode + 1)) | (1u << mode));
			}
		}
		return blocks;
	}

	// One level of blocks, sampled texel for texel (nearest filtering, no mips).
	Ref<Texture> CreateBlockTexture(TextureFormat format, uint32_t size, std::vector<uint8_t> blocks, const std::string& name)
	{
		TextureSpecification specification;
		specification.Format = format;
		specification.Filter = TextureFilter::Nearest;
		specification.Wrap = TextureWrap::Clamp;
		specification.Grayscale = GetTextureFormatInfo(format).Channels == 1;
		specification.DebugName = name;
		TextureMip level0;
		level0.Width = size;
		level0.Height = size;
		level0.Data = std::move(blocks);
		std::string error;
		Ref<Texture> texture = Texture::Create(specification, { level0 }, &error);
		REQUIRE_MESSAGE(texture, error);
		return texture;
	}

	Ref<Texture> CreateRandomBlockTexture(TextureFormat format, uint32_t size, uint32_t seed)
	{
		return CreateBlockTexture(format, size, CreateRandomBlocks(format, size, size, seed), fmt::format("Random{}", TextureFormatToString(format)));
	}

	enum class EncodedPattern
	{
		Gradient, // What each format is for: noisy color gradients, a gray ramp (BC4), a sphere's normals (BC5)
		Checker   // Two colors in cells of three texels, so most blocks hold an edge
	};

	// An image encoded by the importer's block encoders: noisy color gradients (with an alpha ramp for BC3 and BC7), a gray
	// ramp (BC4), a sphere's normals (BC5), or the checker.
	Ref<Texture> CreateEncodedTexture(TextureFormat format, uint32_t size, EncodedPattern pattern = EncodedPattern::Gradient, bool highQuality = true)
	{
		TextureCooker::Image image;
		image.Width = size;
		image.Height = size;
		const uint32_t channels = GetTextureFormatInfo(format).Channels;
		image.Channels = format == TextureFormat::BC5 ? 3 : channels;
		const bool alphaRamp = format == TextureFormat::BC3 || format == TextureFormat::BC3SRGB || format == TextureFormat::BC7 || format == TextureFormat::BC7SRGB;
		uint32_t random = 0x2545F491u;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				random ^= random << 13;
				random ^= random >> 17;
				random ^= random << 5;
				const uint32_t noise = random & 0x0F;
				if (pattern == EncodedPattern::Checker)
				{
					const bool first = (x / 3 + y / 3) % 2 == 0;
					const uint8_t checker[4] = { static_cast<uint8_t>(first ? 230 : 40), static_cast<uint8_t>(first ? 120 : 200), static_cast<uint8_t>(first ? 30 : 160), 255 };
					image.Pixels.insert(image.Pixels.end(), checker, checker + image.Channels);
				}
				else if (format == TextureFormat::BC5)
				{
					const glm::vec2 offset = (glm::vec2(x, y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
					const float radiusSquared = std::min(glm::dot(offset, offset), 0.95f);
					const glm::vec3 normal = glm::normalize(glm::vec3(offset, std::sqrt(1.0f - radiusSquared)));
					for (int channel = 0; channel < 3; channel++)
						image.Pixels.push_back(static_cast<uint8_t>(std::lround((normal[channel] * 0.5f + 0.5f) * 255.0f)));
				}
				else if (channels == 1)
				{
					image.Pixels.push_back(static_cast<uint8_t>((x + y) * 255 / (2 * (size - 1))));
				}
				else
				{
					image.Pixels.push_back(static_cast<uint8_t>((x * 255 / (size - 1)) ^ noise));
					image.Pixels.push_back(static_cast<uint8_t>((y * 255 / (size - 1)) ^ (noise << 1)));
					image.Pixels.push_back(static_cast<uint8_t>((x + y) * 127 / (size - 1)));
					image.Pixels.push_back(static_cast<uint8_t>(alphaRamp ? x * 255 / (size - 1) : 255));
				}
			}
		}
		std::vector<uint8_t> blocks;
		REQUIRE(TextureCooker::EncodeLevel(image, format, highQuality, blocks));
		const char* patternName = pattern == EncodedPattern::Checker ? "Checker" : "Gradient";
		return CreateBlockTexture(format, size, std::move(blocks), fmt::format("Encoded{}{}{}", TextureFormatToString(format), patternName, highQuality ? "" : "Fast"));
	}

	// How far apart a block's two endpoints are in a channel. GPUs interpolate BC1 to BC5 approximately (the
	// specifications leave the rounding open): an NVIDIA RTX 5090 is off by up to 1/48 of that distance in BC1 green and
	// 1/40 in BC4's six-value mode.
	int GetEndpointDistance(TextureFormat format, const uint8_t* block, uint32_t channel)
	{
		auto expand = [](uint16_t color, uint32_t channel)
		{
			const uint32_t bits = channel == 1 ? 6 : 5;
			const uint32_t value = channel == 0 ? (color >> 11) & 0x1F : (channel == 1 ? (color >> 5) & 0x3F : color & 0x1F);
			const uint32_t shifted = value << (8 - bits);
			return static_cast<int>(shifted | (shifted >> bits));
		};
		switch (format)
		{
			case TextureFormat::BC1:
			case TextureFormat::BC1SRGB:
			case TextureFormat::BC3:
			case TextureFormat::BC3SRGB:
			{
				const uint8_t* color = format == TextureFormat::BC3 || format == TextureFormat::BC3SRGB ? block + 8 : block;
				const uint16_t first = static_cast<uint16_t>(color[0] | (color[1] << 8));
				const uint16_t second = static_cast<uint16_t>(color[2] | (color[3] << 8));
				return std::abs(expand(first, channel) - expand(second, channel));
			}
			case TextureFormat::BC4:
				return std::abs(static_cast<int>(block[0]) - static_cast<int>(block[1]));
			case TextureFormat::BC5:
			{
				const uint8_t* half = block + (channel == 1 ? 8 : 0);
				return std::abs(static_cast<int>(half[0]) - static_cast<int>(half[1]));
			}
			default:
				return 0;
		}
	}

	float SRGBToLinear(float value)
	{
		return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
	}

	float LinearToSRGB(float value)
	{
		return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
	}

	// What an unlit surface sampling the texture shows: the sampled value (sRGB formats decode to linear) encoded to sRGB.
	float GetDisplayed(float encoded, bool srgbFormat)
	{
		const float linear = srgbFormat ? SRGBToLinear(encoded / 255.0f) : encoded / 255.0f;
		return LinearToSRGB(std::clamp(linear, 0.0f, 1.0f)) * 255.0f;
	}

	struct SamplingCheck
	{
		uint32_t Outside = 0;       // Channels whose sampled value was more than one step from the CPU decode
		int MaxStepsFromExact = 0;  // Largest difference to the displayed CPU decode, in output steps
		std::string FirstOutside;   // Where the first channel outside was, for the failure message
	};

	// Checks an image of an unlit quad showing `texture` texel for texel against the CPU decode of its blocks: every
	// channel must show a value sampled within one step (1/255) of the decoded one, plus, with approximateInterpolation, a
	// 32nd of the block's endpoint distance (see GetEndpointDistance).
	SamplingCheck CheckSampledImage(const ReadbackImage& image, const Texture& texture, std::span<const uint8_t> blocks, bool approximateInterpolation = false)
	{
		const TextureFormat format = texture.GetSpecification().Format;
		const uint32_t size = texture.GetWidth();
		REQUIRE(image.Width == size);
		REQUIRE(image.Height == size);
		const uint32_t decodedChannels = GetTextureFormatInfo(GetDecodedFormat(format)).BytesPerBlock;
		std::vector<uint8_t> decoded(static_cast<size_t>(size) * size * decodedChannels);
		REQUIRE(BlockCompression::DecodeLevel(format, size, size, blocks, decoded));
		const bool srgb = GetTextureFormatInfo(format).IsSRGB;
		const bool grayscale = texture.GetSpecification().Grayscale;

		SamplingCheck check;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const glm::u8vec4 shown = GetPixelRGBA8(image, x, y);
				const size_t texel = static_cast<size_t>(y) * size + x;
				for (uint32_t channel = 0; channel < 3; channel++)
				{
					// Channels the format does not store sample as zero (gray textures repeat red).
					const bool stored = grayscale || channel < decodedChannels;
					const uint32_t storedChannel = grayscale ? 0 : channel;
					const int value = stored ? decoded[texel * decodedChannels + storedChannel] : 0;
					const uint32_t blockBytes = GetTextureFormatInfo(format).BytesPerBlock;
					const uint8_t* block = blocks.data() + ((y / 4) * ((size + 3) / 4) + x / 4) * blockBytes;
					const float tolerance = 1.0f + (approximateInterpolation ? static_cast<float>(GetEndpointDistance(format, block, storedChannel)) / 32.0f : 0.0f);
					const int exact = static_cast<int>(std::lround(GetDisplayed(static_cast<float>(value), srgb)));
					const int lowest = stored ? static_cast<int>(std::floor(GetDisplayed(std::max(static_cast<float>(value) - tolerance, 0.0f), srgb) - 0.05f)) : 0;
					const int highest = stored ? static_cast<int>(std::ceil(GetDisplayed(std::min(static_cast<float>(value) + tolerance, 255.0f), srgb) + 0.05f)) : 0;
					const int actual = shown[static_cast<int>(channel)];
					if (actual < lowest || actual > highest)
					{
						if (check.Outside++ == 0)
						{
							std::string blockText;
							for (uint32_t byte = 0; byte < blockBytes; byte++)
								blockText += fmt::format("{:02X} ", block[byte]);
							check.FirstOutside = fmt::format("texel ({}, {}) channel {}: decoded {}, shown {} (expected {} to {}); block {}", x, y, channel,
								value, actual, lowest, highest, blockText);
						}
					}
					check.MaxStepsFromExact = std::max(check.MaxStepsFromExact, std::abs(actual - exact));
				}
			}
		}
		return check;
	}

	// Draws an unlit quad that fills the view with the texture, texel for texel.
	ReadbackImage RenderTexturedQuad(AssetManagerBase& manager, AssetHandle texture, uint32_t size)
	{
		MaterialProperties properties = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		properties.BaseColorMap = texture;
		AssetMetadata metadata;
		metadata.Name = "QuadMaterial";
		const AssetHandle material = manager.AddMemoryAsset(Material::Create(properties), metadata);
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, material, glm::vec3(0.0f), "Quad");
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const ReadbackImage image = RenderToImage(renderer, scene, OrthographicLookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), 1.0f));
		CHECK(renderer.GetStats().PendingAssets == 0);
		return image;
	}

	// Lets texture loads decode block-compressed data, as on devices without BC support, while it lives.
	class ScopedBlockCompressionFallback
	{
	public:
		ScopedBlockCompressionFallback() { Texture::SetBlockCompressionSupported(false); }
		~ScopedBlockCompressionFallback() { Texture::SetBlockCompressionSupported(Renderer::GetGraphicsDevice().GetInfo().SupportsBCCompression); }

		ScopedBlockCompressionFallback(const ScopedBlockCompressionFallback&) = delete;
		ScopedBlockCompressionFallback& operator=(const ScopedBlockCompressionFallback&) = delete;
	};

	// 4x2 RGBA8 texture whose texels all differ.
	Ref<Texture> CreatePatternTexture(TextureFormat format, std::vector<uint8_t>& outLevel0)
	{
		TextureMip level0;
		level0.Width = 4;
		level0.Height = 2;
		for (uint32_t texel = 0; texel < 8; texel++)
			level0.Data.insert(level0.Data.end(), { static_cast<uint8_t>(texel * 30), static_cast<uint8_t>(255 - texel * 20), static_cast<uint8_t>(texel * 7), 255 });
		outLevel0 = level0.Data;

		std::vector<TextureMip> mips = { level0 };
		REQUIRE(TextureUtils::GenerateMips(mips, format, false));
		TextureSpecification specification;
		specification.Format = format;
		specification.DebugName = "PatternTexture";
		return Texture::Create(specification, std::move(mips));
	}

	// Finalizes the asset without a budget in a batch of the renderer's upload context, as a manager does.
	bool Upload(Asset& asset)
	{
		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		const bool finalized = asset.FinalizeOnMainThread(AssetFinalizeContext { &upload, nullptr }) == AssetFinalizeResult::Done;
		upload.Submit();
		return finalized;
	}

}

TEST_SUITE("GPU.Assets")
{
	TEST_CASE("Textures upload their mip chain and own a bindless slot")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t allocatedBefore = bindless.GetAllocatedCount();

		for (TextureFormat format : { TextureFormat::RGBA8, TextureFormat::RGBA8SRGB })
		{
			std::vector<uint8_t> level0;
			Ref<Texture> texture = CreatePatternTexture(format, level0);
			REQUIRE(texture);
			REQUIRE(Upload(*texture));

			REQUIRE(texture->GetGPUTexture());
			CHECK(texture->GetGPUTexture()->getDesc().mipLevels == 3);
			CHECK(texture->GetBindlessSlot() >= BindlessTextureTable::c_ReservedSlots);
			CHECK(texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);
			CHECK(bindless.GetAllocatedCount() == allocatedBefore + 1);
			CHECK(texture->GetMips().empty()); // CPU copy released after upload
			CHECK(texture->GetWidth() == 4);
			// The mip chain (4x2, 2x1, 1x1) on the GPU, nothing left on the CPU.
			CHECK(texture->GetMemoryUsage().GpuTextures == (4 * 2 + 2 * 1 + 1 * 1) * 4);
			CHECK(texture->GetMemoryUsage().Cpu == 0);
			CHECK(texture->Serialize().empty()); // CPU copy released

			// Finalizing twice is harmless.
			CHECK(Upload(*texture));
			CHECK(bindless.GetAllocatedCount() == allocatedBefore + 1);

			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), image));
			CHECK(image.Pixels == level0);

			texture.reset();
			CHECK(bindless.GetAllocatedCount() == allocatedBefore);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("HDR textures upload as half floats")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());

		TextureMip level0;
		level0.Width = 2;
		level0.Height = 2;
		level0.Data.resize(2 * 2 * 8);
		auto* halves = reinterpret_cast<uint16_t*>(level0.Data.data());
		for (int index = 0; index < 16; index++)
			halves[index] = glm::packHalf1x16(static_cast<float>(index) * 0.5f);
		const std::vector<uint8_t> expected = level0.Data;

		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA16F;
		Ref<Texture> texture = Texture::Create(specification, { level0 });
		REQUIRE(texture);
		REQUIRE(Upload(*texture));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), image));
		CHECK(image.Format == nvrhi::Format::RGBA16_FLOAT);
		CHECK(image.Pixels == expected);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Meshes upload vertex and index buffers")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Ref<Mesh> mesh = MeshFactory::CreateTorus();
		REQUIRE(mesh);
		const AssetMemoryUsage cpuOnly = mesh->GetMemoryUsage();
		CHECK(cpuOnly.GetGpu() == 0);
		REQUIRE(Upload(*mesh));

		REQUIRE(mesh->GetPositionBuffer());
		REQUIRE(mesh->GetAttributeBuffer());
		REQUIRE(mesh->GetIndexBuffer());
		const uint64_t attributeBytes = mesh->GetPositions().size() * sizeof(MeshVertexAttributes);
		CHECK(mesh->GetPositionBuffer()->getDesc().byteSize == mesh->GetPositions().size() * sizeof(glm::vec3));
		CHECK(mesh->GetAttributeBuffer()->getDesc().byteSize == attributeBytes);
		CHECK(mesh->GetIndexBuffer()->getDesc().byteSize == mesh->GetIndices().size() * sizeof(uint32_t));
		CHECK(mesh->GetIndexBuffer()->getDesc().isIndexBuffer);
		// Each copy counts once, in its own pool: the CPU keeps the positions and indices physics reads, the vertex attributes
		// went with the upload, and the buffers are added on the GPU.
		const AssetMemoryUsage uploaded = mesh->GetMemoryUsage();
		CHECK(uploaded.Cpu == cpuOnly.Cpu - attributeBytes);
		CHECK(uploaded.GpuBuffers == mesh->GetPositionBuffer()->getDesc().byteSize + mesh->GetAttributeBuffer()->getDesc().byteSize
			+ mesh->GetIndexBuffer()->getDesc().byteSize);
		CHECK(uploaded.GpuTextures == 0);
		CHECK_FALSE(mesh->GetPositions().empty());
		CHECK_FALSE(mesh->GetIndices().empty());
		CHECK(mesh->GetAttributes().empty());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Asset managers upload loaded assets")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());

		std::vector<uint8_t> level0;
		Ref<Texture> source = CreatePatternTexture(TextureFormat::RGBA8SRGB, level0);
		REQUIRE(source);
		std::map<uint64_t, std::vector<uint8_t>> data = {
			{ 0x9000, source->Serialize() },
			{ 0x9001, MeshFactory::CreateCone()->Serialize() } };
		AssetMetadata texture;
		texture.Handle = UUID(0x9000);
		texture.Type = AssetType::Texture;
		texture.Path = "Pattern.png";
		AssetMetadata mesh;
		mesh.Handle = UUID(0x9001);
		mesh.Type = AssetType::Mesh;
		mesh.Path = "Cone.mesh";

		const std::filesystem::path path = Tests::CreateTemporaryDirectory("GPUAssetPack") / "Game.stpak";
		REQUIRE(AssetPackWriter::Write(path, { texture, mesh }, [&data](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(metadata.Handle));
			return true;
		}));

		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);

		// Built-in assets are uploaded as soon as they are registered.
		Ref<Mesh> cube = std::static_pointer_cast<Mesh>(manager->GetAsset(BuiltinAssets::CubeMesh));
		REQUIRE(cube);
		CHECK(cube->GetIndexBuffer() != nullptr);

		Ref<Asset> loadedTexture = manager->LoadAssetSync(UUID(0x9000));
		REQUIRE(loadedTexture);
		Ref<Texture> gpuTexture = std::static_pointer_cast<Texture>(loadedTexture);
		REQUIRE(gpuTexture->GetGPUTexture());
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(gpuTexture->GetGPUTexture(), image));
		CHECK(image.Pixels == level0);

		Ref<Asset> loadedMesh = manager->LoadAssetSync(UUID(0x9001));
		REQUIRE(loadedMesh);
		CHECK(std::static_pointer_cast<Mesh>(loadedMesh)->GetIndexBuffer() != nullptr);

		// A tiny upload budget spreads finalization over several updates: each takes its first step only, one of the smallest
		// size at most.
		manager->UnloadAsset(UUID(0x9000));
		manager->UnloadAsset(UUID(0x9001));
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.UploadBytesPerFrame = 1;
		manager->SetResidencyBudgets(budgets);
		manager->RequestLoad(UUID(0x9000));
		manager->RequestLoad(UUID(0x9001));
		uint32_t updates = 0;
		const auto ready = [&](uint64_t handle) { return manager->GetAssetState(UUID(handle)) == AssetState::Ready; };
		while (updates < 64 && !(ready(0x9000) && ready(0x9001)))
		{
			manager->Update();
			updates++;
			CHECK(manager->GetStats().UploadedBytesLastFrame <= GPUUploadQueue::c_MinStepBytes);
		}
		CHECK(updates >= 2);
		CHECK(ready(0x9000));
		CHECK(ready(0x9001));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Block-compressed textures sample within 1/255 of their CPU decode, and like it when decoded at load time")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		REQUIRE(gpu.GetDevice().GetInfo().SupportsBCCompression);
		constexpr uint32_t c_Size = c_SceneTestSize;
		struct Encoded
		{
			TextureFormat Format;
			EncodedPattern Pattern;
			bool HighQuality;
		};
		// Every format the importer encodes, as the encoders store them; BC7, whose encoder searches modes, partitions and
		// rotations, also a checker of edges between two colors and both qualities.
		const Encoded textures[] = { { TextureFormat::BC1, EncodedPattern::Gradient, true }, { TextureFormat::BC1SRGB, EncodedPattern::Gradient, true },
			{ TextureFormat::BC3SRGB, EncodedPattern::Gradient, true }, { TextureFormat::BC4, EncodedPattern::Gradient, true },
			{ TextureFormat::BC5, EncodedPattern::Gradient, true }, { TextureFormat::BC7, EncodedPattern::Gradient, true },
			{ TextureFormat::BC7, EncodedPattern::Checker, true }, { TextureFormat::BC7, EncodedPattern::Gradient, false },
			{ TextureFormat::BC7SRGB, EncodedPattern::Gradient, true }, { TextureFormat::BC7SRGB, EncodedPattern::Checker, true },
			{ TextureFormat::BC7SRGB, EncodedPattern::Checker, false } };

		// Sampled through the bindless table by the scene renderer, block-compressed on the GPU.
		std::map<uint64_t, ReadbackImage> compressedImages;
		std::map<uint64_t, std::vector<uint8_t>> cooked;
		std::vector<AssetMetadata> entries;
		{
			SceneTestAssets assets;
			for (uint64_t index = 0; index < std::size(textures); index++)
			{
				const Encoded& encoded = textures[index];
				const Ref<Texture> texture = CreateEncodedTexture(encoded.Format, c_Size, encoded.Pattern, encoded.HighQuality);
				const std::string name = texture->GetSpecification().DebugName;
				CAPTURE(name);
				const std::vector<uint8_t> blocks(texture->GetMipData(0).begin(), texture->GetMipData(0).end());
				AssetMetadata metadata;
				metadata.Handle = UUID(0xBC00 + index);
				metadata.Type = AssetType::Texture;
				metadata.Path = name + ".png";
				cooked[static_cast<uint64_t>(metadata.Handle)] = texture->Serialize();
				entries.push_back(metadata);

				const AssetHandle handle = assets.Add(texture, metadata.Path);
				CHECK(texture->GetSpecification().Format == encoded.Format);
				CHECK(texture->GetMemoryUsage().GpuTextures == GetMipByteSize(encoded.Format, c_Size, c_Size));
				const ReadbackImage image = RenderTexturedQuad(*assets.Manager, handle, c_Size);
				const SamplingCheck check = CheckSampledImage(image, *texture, blocks);
				MESSAGE(name, " on the GPU: ", check.Outside, " channels sampled more than 1/255 from the CPU decode, at most ",
					check.MaxStepsFromExact, " output steps from it");
				CHECK_MESSAGE(check.Outside == 0, check.FirstOutside);
				compressedImages[index] = image;
			}
		}

		// The same cooked textures loaded from a pack while the device counts as one without BC support: the loader decodes
		// them, and they show the same texels.
		const std::filesystem::path pack = CreateTemporaryDirectory("BlockCompressionFallback") / "Game.stpak";
		REQUIRE(AssetPackWriter::Write(pack, entries, [&cooked](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string*)
		{
			outData = cooked.at(static_cast<uint64_t>(metadata.Handle));
			return true;
		}));
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack);
		REQUIRE(manager);
		AssetManager::SetActive(manager);
		{
			ScopedBlockCompressionFallback fallback;
			for (uint64_t index = 0; index < std::size(textures); index++)
			{
				const TextureFormat format = textures[index].Format;
				const AssetHandle handle = UUID(0xBC00 + index);
				const std::string name = entries[index].Path;
				CAPTURE(name);
				const Ref<Texture> texture = std::static_pointer_cast<Texture>(manager->LoadAssetSync(handle));
				REQUIRE(texture);
				CHECK(texture->GetSpecification().Format == GetDecodedFormat(format));
				CHECK(texture->GetSpecification().Grayscale == (format == TextureFormat::BC4));
				// Residency counts what the device holds: the decoded texels.
				CHECK(texture->GetMemoryUsage().GpuTextures == GetMipByteSize(GetDecodedFormat(format), c_Size, c_Size));

				const ReadbackImage image = RenderTexturedQuad(*manager, handle, c_Size);
				const Ref<Texture> reference = Texture::Deserialize(cooked.at(static_cast<uint64_t>(handle)));
				REQUIRE(reference);
				// Its cooked layout is still the pack's: the block-compressed levels, where streaming would read them.
				CHECK(texture->GetCookedLayout().Format == format);
				CHECK(texture->GetMipTailByteRange().Offset == reference->GetMipTailByteRange().Offset);
				CHECK(texture->GetMipTailByteRange().Size == reference->GetMipTailByteRange().Size);
				const SamplingCheck check = CheckSampledImage(image, *reference, reference->GetMipData(0));
				CHECK(check.Outside == 0);
				CHECK(check.MaxStepsFromExact <= 1);
				const int difference = MaxDifference(image, compressedImages[index]);
				MESSAGE(name, " decoded at load time: at most ", check.MaxStepsFromExact, " output steps from the CPU decode, ",
					difference, " from the GPU's decode");
				// BC7 decodes exactly everywhere; the other formats' interpolation may round differently on the GPU.
				if (format == TextureFormat::BC7 || format == TextureFormat::BC7SRGB)
					CHECK(difference == 0);
			}
		}
		AssetManager::SetActive(nullptr);
		CHECK(Texture::IsBlockCompressionSupported());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("GPUs decode random blocks of every format like the CPU decoders")
	{
		// Random blocks hold every encoding a format allows (both BC1 and BC4 modes, extreme endpoints, all of BC7's modes,
		// partitions and rotations), which encoders rarely produce. BC7 must match exactly; BC1 to BC5 within the
		// approximate interpolation GPUs may use (GetEndpointDistance).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		REQUIRE(gpu.GetDevice().GetInfo().SupportsBCCompression);
		constexpr uint32_t c_Size = c_SceneTestSize;
		SceneTestAssets assets;
		uint32_t seed = 100;
		for (TextureFormat format : { TextureFormat::BC1, TextureFormat::BC1SRGB, TextureFormat::BC3SRGB, TextureFormat::BC4, TextureFormat::BC5,
			TextureFormat::BC7, TextureFormat::BC7SRGB })
		{
			const std::string formatName = TextureFormatToString(format);
			CAPTURE(formatName);
			const Ref<Texture> texture = CreateRandomBlockTexture(format, c_Size, seed++);
			const std::vector<uint8_t> blocks(texture->GetMipData(0).begin(), texture->GetMipData(0).end());
			const AssetHandle handle = assets.Add(texture, texture->GetSpecification().DebugName);
			const ReadbackImage image = RenderTexturedQuad(*assets.Manager, handle, c_Size);
			const bool bc7 = format == TextureFormat::BC7 || format == TextureFormat::BC7SRGB;
			const SamplingCheck check = CheckSampledImage(image, *texture, blocks, !bc7);
			MESSAGE(formatName, " random blocks: ", check.Outside, " channels outside, at most ", check.MaxStepsFromExact, " output steps from the CPU decode");
			CHECK_MESSAGE(check.Outside == 0, check.FirstOutside);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
