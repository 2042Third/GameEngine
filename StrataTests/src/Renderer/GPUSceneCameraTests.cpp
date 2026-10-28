#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The image of a camera: the output's dither and the tone, color, exposure, bloom and lens of PostProcessComponent.
namespace
{

	constexpr uint32_t c_RampWidth = 512;
	constexpr uint32_t c_RampHeight = 64;

	// The linear luminance an 8-bit target shows as `code` (a fraction of a code included), for flat emissive fields.
	float LuminanceForCode(float code)
	{
		return PostProcessMath::SRGBToLinear(code / 255.0f);
	}

	// A wall of one emissive luminance filling the view, shown without a tone curve, exposure, bloom or vignette.
	struct FlatField
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		Entity Wall;
		SceneCamera Camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));

		FlatField(SceneTestAssets& assets, float luminance)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			Wall = AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(luminance)), glm::vec3(0.0f), "Wall");
			Wall.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		}
	};

	// A ramp of four codes across the view: an emissive wall whose map holds two texels four codes apart, filtered linearly,
	// seen by an orthographic camera that shows the half of the map between the texels' centers.
	struct Ramp
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), 1.0f);

		explicit Ramp(SceneTestAssets& assets)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			std::vector<uint8_t> texels;
			AppendPixel(texels, 128, 128, 128, 255);
			AppendPixel(texels, 132, 132, 132, 255);
			MaterialProperties material = Emitting(1.0f);
			material.EmissiveMap = assets.AddTexture(2, 1, texels, TextureFormat::RGBA8SRGB, TextureFilter::Linear, TextureWrap::Clamp);
			Entity wall = AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(material), glm::vec3(0.0f), "Ramp");
			wall.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 1.0f, 1.0f);
		}
	};

	// Each pixel's code as the HDR image says it must show without quantization: the scene color, sRGB-encoded as the tone
	// mapping encodes it (Tonemapper None, the manual exposure 0 and the pre-exposure 1 leave it unchanged), in codes of
	// `levels`.
	std::vector<float> ExactCodes(SceneRenderer& renderer, float levels)
	{
		ReadbackImage hdr;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		std::vector<float> codes(static_cast<size_t>(hdr.Width) * hdr.Height);
		for (uint32_t y = 0; y < hdr.Height; y++)
		{
			for (uint32_t x = 0; x < hdr.Width; x++)
			{
				const glm::vec3 linear = glm::clamp(glm::vec3(ReadHalfTexel(hdr, x, y)), glm::vec3(0.0f), glm::vec3(1.0f));
				codes[static_cast<size_t>(y) * hdr.Width + x] = PostProcessMath::LinearToSRGB(linear).r * levels;
			}
		}
		return codes;
	}

	// The red channel's code of every pixel of an RGBA8 image or an R10G10B10A2 one.
	std::vector<float> ReadCodes(const ReadbackImage& image)
	{
		std::vector<float> codes(static_cast<size_t>(image.Width) * image.Height);
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
			{
				const size_t index = static_cast<size_t>(y) * image.Width + x;
				if (image.Format == nvrhi::Format::R10G10B10A2_UNORM)
				{
					uint32_t packed = 0;
					std::memcpy(&packed, image.Pixels.data() + index * 4, sizeof(packed));
					codes[index] = static_cast<float>(packed & 0x3FFu);
				}
				else
				{
					codes[index] = static_cast<float>(GetPixelRGBA8(image, x, y).r);
				}
			}
		}
		return codes;
	}

	// The largest difference between the mean of `codes` and that of `exact` over each band of `bandWidth` columns.
	float LargestBandError(const std::vector<float>& codes, const std::vector<float>& exact, uint32_t width, uint32_t height, uint32_t bandWidth)
	{
		float largest = 0.0f;
		for (uint32_t band = 0; band < width / bandWidth; band++)
		{
			double sum = 0.0;
			double exactSum = 0.0;
			for (uint32_t y = 0; y < height; y++)
			{
				for (uint32_t x = band * bandWidth; x < (band + 1) * bandWidth; x++)
				{
					sum += codes[static_cast<size_t>(y) * width + x];
					exactSum += exact[static_cast<size_t>(y) * width + x];
				}
			}
			largest = std::max(largest, static_cast<float>(std::abs(sum - exactSum) / (static_cast<double>(bandWidth) * height)));
		}
		return largest;
	}

	// The distinct red codes of an image.
	std::set<int> DistinctCodes(const ReadbackImage& image)
	{
		std::set<int> codes;
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				codes.insert(Red(image, x, y));
		}
		return codes;
	}

	double MeanRed(const ReadbackImage& image)
	{
		double sum = 0.0;
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				sum += Red(image, x, y);
		}
		return sum / (static_cast<double>(image.Width) * image.Height);
	}

	// The curves before Filmic, transcribed from PostProcess/Tonemap.frag for a gray value: what they must keep showing.
	float EarlierCurve(TonemapOperator tonemapper, float gray)
	{
		const glm::vec3 color(gray);
		switch (tonemapper)
		{
			case TonemapOperator::Reinhard:
				return gray / (1.0f + gray);
			case TonemapOperator::ACES:
			{
				const glm::mat3 input(0.59719f, 0.07600f, 0.02840f, 0.35458f, 0.90834f, 0.13383f, 0.04823f, 0.01566f, 0.83777f);
				const glm::mat3 output(1.60475f, -0.10208f, -0.00327f, -0.53108f, 1.10813f, -0.07276f, -0.07367f, -0.00605f, 1.07602f);
				const glm::vec3 v = input * color;
				const glm::vec3 a = v * (v + 0.0245786f) - 0.000090537f;
				const glm::vec3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
				return glm::clamp(output * (a / b), 0.0f, 1.0f).r;
			}
			case TonemapOperator::AgX:
			{
				const glm::mat3 inset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f, 0.878468636469772f, 0.0784336f,
					0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
				const glm::mat3 outset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f, 1.15190312990417f,
					-0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
				glm::vec3 v = inset * glm::max(color, glm::vec3(1e-10f));
				v = glm::clamp((glm::log2(v) + 12.47393f) / (4.026069f + 12.47393f), 0.0f, 1.0f);
				const glm::vec3 v2 = v * v;
				const glm::vec3 v4 = v2 * v2;
				v = 15.5f * v4 * v2 - 40.14f * v4 * v + 31.96f * v4 - 6.868f * v2 * v + 0.4298f * v2 + 0.1191f * v - 0.00232f;
				v = outset * v;
				return std::pow(std::max(v.r, 0.0f), 2.2f);
			}
			case TonemapOperator::KhronosNeutral:
			{
				// A gray stays gray: the desaturation mixes the compressed peak with itself.
				const float startCompression = 0.8f - 0.04f;
				const float offset = gray < 0.08f ? gray - 6.25f * gray * gray : 0.04f;
				const float value = gray - offset;
				if (value < startCompression)
					return value;
				const float d = 1.0f - startCompression;
				return 1.0f - d * d / (value + d - startCompression);
			}
			default:
				return std::clamp(gray, 0.0f, 1.0f);
		}
	}

	// A small bright square (1% of the view's height) on black, for the bloom: Tonemapper None, manual exposure, bloom on.
	struct BloomScene
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera;

		explicit BloomScene(SceneTestAssets& assets)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			PostProcess->Bloom = true;
			PostProcess->BloomIntensity = 0.04f;
			const float halfHeight = 2.0f * std::tan(glm::radians(30.0f));
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1000.0f)), glm::vec3(0.0f), "Light")
				.GetComponent<TransformComponent>().Scale = glm::vec3(0.02f * halfHeight);
			Camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), 16.0f / 9.0f);
		}
	};

	// The halo the bloom chain made (its level 0, half resolution) around the square: the distance from the center, as a
	// fraction of the image's width, at which the glow along the horizontal falls to 1% of its value 2% of the height out; and
	// the glow's energy (the chain summed and normalized as the tone mapping mixes it) relative to the source's.
	struct BloomHalo
	{
		float RadiusOfWidth = 0.0f;
		float Energy = 0.0f;
	};

	BloomHalo MeasureBloomHalo(SceneRenderer& renderer, BloomScene& bloom, glm::uvec2 size)
	{
		renderer.SetViewportSize(size.x, size.y);
		RenderToImage(renderer, bloom.SceneData, bloom.Camera);
		ReadbackImage chain;
		REQUIRE(Renderer::ReadTexture(renderer.GetBloomTexture(), chain));
		ReadbackImage hdr;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		const float span = SceneRenderer::GetBloomSpan(bloom.PostProcess->BloomRadius, size.y, RenderData::c_MaxBloomLevels);
		BloomHalo halo;
		double glow = 0.0;
		for (uint32_t y = 0; y < chain.Height; y++)
		{
			for (uint32_t x = 0; x < chain.Width; x++)
				glow += ReadHalfTexel(chain, x, y).r;
		}
		double source = 0.0;
		for (uint32_t y = 0; y < hdr.Height; y++)
		{
			for (uint32_t x = 0; x < hdr.Width; x++)
				source += ReadHalfTexel(hdr, x, y).r;
		}
		// A texel of level 0 covers 2x2 output pixels.
		halo.Energy = static_cast<float>(glow * 4.0 / span / source);
		const uint32_t row = chain.Height / 2;
		const uint32_t center = chain.Width / 2;
		const float reference = ReadHalfTexel(chain, center + static_cast<uint32_t>(0.02f * static_cast<float>(chain.Height)), row).r;
		uint32_t x = center;
		while (x + 1 < chain.Width && ReadHalfTexel(chain, x, row).r > 0.01f * reference)
			x++;
		halo.RadiusOfWidth = static_cast<float>(x - center) / static_cast<float>(chain.Width);
		return halo;
	}

	// The code a pixel of a flat field of `linear` luminance (Tonemapper None, manual exposure 0) shows with the lens vignette.
	float VignettedCode(float linear, glm::uvec2 pixel, glm::uvec2 size, const glm::vec2& tangents, float vignette)
	{
		const glm::vec2 uv = (glm::vec2(pixel) + 0.5f) / glm::vec2(size);
		const glm::vec2 field = glm::vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f) * tangents;
		const float secantSquared = 1.0f + glm::dot(field, field);
		const float factor = glm::mix(1.0f, 1.0f / (secantSquared * secantSquared), vignette);
		return PostProcessMath::LinearToSRGB(glm::vec3(linear * factor)).r * 255.0f;
	}

	SceneRendererSpecification Dithered()
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.DitherOutput = true;
		return specification;
	}

}

TEST_SUITE("GPU.SceneRenderer.Camera")
{
	TEST_CASE("Dithering keeps a gradient's mean and breaks up its bands")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Ramp ramp(assets);

		for (bool dither : { true, false })
		{
			SceneRendererSpecification specification = ExactSpecification();
			specification.DitherOutput = dither;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_RampWidth, c_RampHeight);
			const ReadbackImage image = RenderToImage(renderer, ramp.SceneData, ramp.Camera);
			const std::vector<float> exact = ExactCodes(renderer, 255.0f);
			const auto [low, high] = std::minmax_element(exact.begin(), exact.end());
			CHECK(*high - *low > 3.0f); // The ramp spans about four codes
			// Bands of 32 columns: a quarter of a code each.
			const float error = LargestBandError(ReadCodes(image), exact, c_RampWidth, c_RampHeight, 32);
			CAPTURE(dither);
			CHECK(renderer.GetStats().Dithered == dither);
			if (dither)
				CHECK(error < 0.1f);
			else
				CHECK(error > 0.2f); // Rounding steps the bands
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The dither on the GPU is its CPU twin")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (float code : { 100.5f, 3.25f, 0.3f })
		{
			FlatField field(assets, LuminanceForCode(code));
			SceneRenderer renderer(Dithered());
			renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
			const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
			ReadbackImage hdr;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
			uint32_t equal = 0;
			uint32_t close = 0;
			for (uint32_t y = 0; y < image.Height; y++)
			{
				for (uint32_t x = 0; x < image.Width; x++)
				{
					// The value the shader sees: the half-float scene color, sRGB-encoded in float.
					const glm::vec3 linear = glm::clamp(glm::vec3(ReadHalfTexel(hdr, x, y)), glm::vec3(0.0f), glm::vec3(1.0f));
					const glm::vec3 expected = glm::round(PostProcessMath::DitherToCodes(PostProcessMath::LinearToSRGB(linear), glm::uvec2(x, y), 255.0f) * 255.0f);
					const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
					const glm::vec3 actual(pixel.r, pixel.g, pixel.b);
					const glm::vec3 differences = glm::abs(actual - expected);
					const float difference = std::max({ differences.r, differences.g, differences.b });
					equal += difference == 0.0f ? 1u : 0u;
					close += difference <= 1.0f ? 1u : 0u;
				}
			}
			const uint32_t pixels = image.Width * image.Height;
			CAPTURE(code);
			CHECK(equal >= pixels - pixels / 1000);
			CHECK(close == pixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("FXAA frames dither once, after FXAA")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		FlatField field(assets, LuminanceForCode(100.5f));
		field.PostProcess->AntiAliasing = true;
		field.PostProcess->AntiAliasingMethod = AntiAliasingMode::FXAA;
		SceneRenderer renderer(Dithered());
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
		REQUIRE(renderer.GetLDRTexture());
		CHECK(renderer.GetLDRTexture()->getDesc().format == nvrhi::Format::R10G10B10A2_UNORM);
		CHECK(renderer.GetStats().Dithered);
		// The 10-bit intermediate holds 403 / 1023, code 100.46: dithered once, the mean stays there.
		CHECK(std::abs(MeanRed(image) - 100.457) < 0.06);
		for (int code : DistinctCodes(image))
		{
			CAPTURE(code);
			CHECK((code >= 99 && code <= 101));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A 10-bit target is dithered at 10-bit precision")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Ramp ramp(assets);
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::TextureDesc desc;
		desc.width = c_RampWidth;
		desc.height = c_RampHeight;
		desc.format = nvrhi::Format::R10G10B10A2_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = "Test.Target10Bit";
		nvrhi::TextureHandle target = device->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		REQUIRE(framebuffer);

		for (bool dither : { true, false })
		{
			SceneRendererSpecification specification = ExactSpecification();
			specification.DitherOutput = dither;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_RampWidth, c_RampHeight);
			REQUIRE(renderer.Render(ramp.SceneData, ramp.Camera, framebuffer));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(target, image));
			REQUIRE(image.Format == nvrhi::Format::R10G10B10A2_UNORM);
			const std::vector<float> codes = ReadCodes(image);
			const std::vector<float> exact = ExactCodes(renderer, 1023.0f);
			float largest = 0.0f;
			for (size_t index = 0; index < codes.size(); index++)
				largest = std::max(largest, std::abs(codes[index] - exact[index]));
			// Bands of 8 columns: the ramp spans sixteen 10-bit codes, so a quarter of a code each.
			const float bandError = LargestBandError(codes, exact, c_RampWidth, c_RampHeight, 8);
			CAPTURE(dither);
			CHECK(renderer.GetStats().Dithered == dither);
			// A triangular dither of one code errs by less than 1.5 codes; one of the 8-bit output by up to 6 10-bit codes.
			CHECK(largest < 1.5f);
			if (dither)
				CHECK(bandError < 0.1f);
			else
				CHECK(bandError > 0.2f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("DitherOutput false quantizes without noise")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		enum class Method
		{
			Off,
			FXAA,
			TAA
		};
		for (Method method : { Method::Off, Method::FXAA, Method::TAA })
		{
			for (bool dither : { false, true })
			{
				SceneRendererSpecification specification = ExactSpecification();
				specification.DitherOutput = dither;
				for (float code : { 100.5f, 3.25f, 254.6f })
				{
					FlatField field(assets, LuminanceForCode(code));
					field.PostProcess->AntiAliasing = method != Method::Off;
					field.PostProcess->AntiAliasingMethod = method == Method::TAA ? AntiAliasingMode::TAA : AntiAliasingMode::FXAA;
					SceneRenderer renderer(specification);
					renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
					const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
					CAPTURE(static_cast<int>(method));
					CAPTURE(dither);
					CAPTURE(code);
					if (dither)
						CHECK(DistinctCodes(image).size() >= 2);
					else
						CHECK(DistinctCodes(image).size() == 1);
				}
				// Black (nothing drawn) and white (an emissive far above 1) are exact either way.
				for (float luminance : { 0.0f, 10.0f })
				{
					FlatField field(assets, luminance);
					field.PostProcess->AntiAliasing = method != Method::Off;
					field.PostProcess->AntiAliasingMethod = method == Method::TAA ? AntiAliasingMode::TAA : AntiAliasingMode::FXAA;
					SceneRenderer renderer(specification);
					renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
					const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
					const std::set<int> codes = DistinctCodes(image);
					CAPTURE(static_cast<int>(method));
					CAPTURE(dither);
					CAPTURE(luminance);
					CHECK(codes == std::set<int> { luminance > 0.0f ? 255 : 0 });
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Filmic on the GPU matches its CPU reference")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const glm::vec3 colors[] = { { 1.0f, 0.05f, 0.05f }, { 0.05f, 1.0f, 0.05f }, { 0.05f, 0.05f, 1.0f }, { 1.0f, 1.0f, 0.05f }, { 0.8f, 0.5f, 0.35f },
			{ 0.2f, 0.4f, 0.08f }, { 0.25f, 0.45f, 0.9f }, { 1.0f, 1.0f, 1.0f } };
		constexpr uint32_t c_Size = 16;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		uint32_t compared = 0;
		for (const glm::vec3& color : colors)
		{
			// Scaled to the luminance of middle gray, then exposed from -3 to +4 stops (whole stops: the pre-exposure divides the
			// manual exposure out exactly).
			const glm::vec3 scaled = color * (0.18f / glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f)));
			MaterialProperties material = Emitting(1.0f);
			material.EmissiveColor = scaled;
			FlatField field(assets, 1.0f);
			field.Wall.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(material);
			field.Wall.MarkModified<MeshRendererComponent>();
			field.PostProcess->Tonemapper = TonemapOperator::Filmic;
			const PostProcessMath::FilmicCurve look = PostProcessMath::MakeFilmicCurve(field.PostProcess->Toe, field.PostProcess->Shoulder);
			for (int stops = -3; stops <= 4; stops++)
			{
				field.PostProcess->Exposure = static_cast<float>(stops);
				const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
				ReadbackImage hdr;
				REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
				const float preExposure = renderer.GetStats().PreExposure;
				for (uint32_t y = 0; y < c_Size; y++)
				{
					for (uint32_t x = 0; x < c_Size; x++)
					{
						const glm::vec3 exposed = glm::vec3(ReadHalfTexel(hdr, x, y)) * (std::exp2(static_cast<float>(stops)) / preExposure);
						const glm::vec3 expected = PostProcessMath::LinearToSRGB(PostProcessMath::ApplyFilmic(exposed, look)) * 255.0f;
						const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
						const glm::vec3 difference = glm::abs(glm::vec3(pixel.r, pixel.g, pixel.b) - expected);
						CAPTURE(color);
						CAPTURE(stops);
						CHECK(std::max({ difference.r, difference.g, difference.b }) <= 1.0f);
						compared++;
					}
				}
			}
		}
		CHECK(compared == 8 * 8 * c_Size * c_Size);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Automatic exposure under Filmic shows middle gray at sRGB 118 with Exposure 0")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// A histogram bin's center (Include/Exposure.glsl's HistogramBinToLog2): the metering returns bin centers, and half a
		// bin (0.055 stops) times Filmic's slope would move the result by up to 2.7 codes.
		constexpr uint32_t c_Bin = 120;
		const float log2Luminance = RenderData::c_HistogramMinLog2
			+ (static_cast<float>(c_Bin - 1) + 0.5f) / static_cast<float>(RenderData::c_HistogramBins - 2) * (RenderData::c_HistogramMaxLog2 - RenderData::c_HistogramMinLog2);
		FlatField field(assets, std::exp2(log2Luminance));
		field.PostProcess->AutoExposure = true;
		field.PostProcess->Exposure = 0.0f;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		for (TonemapOperator tonemapper : { TonemapOperator::Filmic, TonemapOperator::ACES })
		{
			field.PostProcess->Tonemapper = tonemapper;
			renderer.ResetExposureAdaptation();
			const int center = Red(RenderToImage(renderer, field.SceneData, field.Camera), c_SceneTestSize / 2, c_SceneTestSize / 2);
			CAPTURE(static_cast<int>(tonemapper));
			if (tonemapper == TonemapOperator::Filmic)
				CHECK((center >= 117 && center <= 119));
			else
				CHECK(std::abs(center - 91) <= 1); // ACES shows middle gray at sRGB 91: it needs about +0.8 EV
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The earlier curves are unchanged")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		for (TonemapOperator tonemapper : { TonemapOperator::ACES, TonemapOperator::AgX, TonemapOperator::KhronosNeutral, TonemapOperator::Reinhard })
		{
			for (float gray : { 0.005f, 0.05f, 0.18f, 0.5f, 1.0f, 2.0f, 5.0f, 11.5f })
			{
				FlatField field(assets, gray);
				field.PostProcess->Tonemapper = tonemapper;
				const int code = Red(RenderToImage(renderer, field.SceneData, field.Camera), c_SceneTestSize / 2, c_SceneTestSize / 2);
				const float expected = PostProcessMath::LinearToSRGB(glm::vec3(EarlierCurve(tonemapper, gray))).r * 255.0f;
				CAPTURE(static_cast<int>(tonemapper));
				CAPTURE(gray);
				CHECK(std::abs(static_cast<float>(code) - expected) <= 1.0f);
			}
		}
		// The transcription's middle grays: ACES 91, AgX 128, Khronos Neutral 105, Reinhard 109.
		auto middleGray = [](TonemapOperator tonemapper) { return std::lround(PostProcessMath::LinearToSRGB(glm::vec3(EarlierCurve(tonemapper, 0.18f))).r * 255.0f); };
		CHECK(middleGray(TonemapOperator::ACES) == 91);
		CHECK(middleGray(TonemapOperator::AgX) == 128);
		CHECK(middleGray(TonemapOperator::KhronosNeutral) == 105);
		CHECK(middleGray(TonemapOperator::Reinhard) == 109);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Grading at its defaults renders bit-identically to no grading; white balance, slope, offset and power follow the CPU math")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_Size = 16;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const glm::vec3 colors[] = { { 0.18f, 0.18f, 0.18f }, { 0.4f, 0.2f, 0.1f }, { 0.05f, 0.1f, 0.3f }, { 0.01f, 0.012f, 0.008f } };
		struct Grading
		{
			float Temperature;
			float Tint;
			glm::vec3 Slope;
			glm::vec3 Offset;
			glm::vec3 Power;
		};
		const Grading gradings[] = {
			{ 3200.0f, 0.0f, glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f) },
			{ 9000.0f, -0.5f, glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f) },
			{ 6500.0f, 0.0f, glm::vec3(1.2f, 1.0f, 0.8f), glm::vec3(0.0f), glm::vec3(1.0f) },
			{ 6500.0f, 0.0f, glm::vec3(1.0f), glm::vec3(0.05f, 0.0f, -0.05f), glm::vec3(1.0f) },
			{ 5600.0f, 0.3f, glm::vec3(1.1f, 0.9f, 1.0f), glm::vec3(0.0f, 0.02f, 0.0f), glm::vec3(1.3f, 1.0f, 0.8f) },
		};
		for (const glm::vec3& color : colors)
		{
			MaterialProperties material = Emitting(1.0f);
			material.EmissiveColor = color;
			FlatField field(assets, 1.0f);
			field.Wall.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(material);
			field.Wall.MarkModified<MeshRendererComponent>();
			// The neutral grading takes no branch: the image equals the one before grading existed, a plain clip.
			const ReadbackImage neutral = RenderToImage(renderer, field.SceneData, field.Camera);
			ReadbackImage hdr;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
			const glm::vec3 scene = glm::vec3(ReadHalfTexel(hdr, c_Size / 2, c_Size / 2));
			const glm::u8vec4 plain = GetPixelRGBA8(neutral, c_Size / 2, c_Size / 2);
			const glm::vec3 plainExpected = PostProcessMath::LinearToSRGB(glm::clamp(scene, 0.0f, 1.0f)) * 255.0f;
			CHECK(std::abs(static_cast<float>(plain.r) - plainExpected.r) <= 0.5f);
			for (const Grading& grading : gradings)
			{
				field.PostProcess->WhiteBalanceTemperature = grading.Temperature;
				field.PostProcess->WhiteBalanceTint = grading.Tint;
				field.PostProcess->ColorSlope = grading.Slope;
				field.PostProcess->ColorOffset = grading.Offset;
				field.PostProcess->ColorPower = grading.Power;
				const ReadbackImage image = RenderToImage(renderer, field.SceneData, field.Camera);
				const PostProcessMath::ColorGrading cpu = PostProcessMath::MakeColorGrading(grading.Temperature, grading.Tint, grading.Slope, grading.Offset,
					grading.Power);
				const glm::vec3 expected = PostProcessMath::LinearToSRGB(glm::clamp(PostProcessMath::ApplyColorGrading(scene, cpu), 0.0f, 1.0f)) * 255.0f;
				const glm::u8vec4 pixel = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
				const glm::vec3 difference = glm::abs(glm::vec3(pixel.r, pixel.g, pixel.b) - expected);
				CAPTURE(color);
				CAPTURE(grading.Temperature);
				CHECK(std::max({ difference.r, difference.g, difference.b }) <= 1.0f);
			}
			field.PostProcess->WhiteBalanceTemperature = 6500.0f;
			field.PostProcess->WhiteBalanceTint = 0.0f;
			field.PostProcess->ColorSlope = glm::vec3(1.0f);
			field.PostProcess->ColorOffset = glm::vec3(0.0f);
			field.PostProcess->ColorPower = glm::vec3(1.0f);
			CHECK(MaxDifference(RenderToImage(renderer, field.SceneData, field.Camera), neutral) == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bloom covers the same share of the image at 1080 and 2160 rows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const glm::uvec2 sizes[] = { { 1920, 1080 }, { 3840, 2160 } };
		BloomHalo halos[2];
		for (size_t index = 0; index < 2; index++)
		{
			BloomScene bloom(assets);
			SceneRenderer renderer(ExactSpecification());
			halos[index] = MeasureBloomHalo(renderer, bloom, sizes[index]);
			CAPTURE(sizes[index]);
			CHECK(renderer.GetStats().BloomLevels == (index == 0 ? 6u : 7u));
		}
		CHECK(halos[0].RadiusOfWidth == doctest::Approx(halos[1].RadiusOfWidth).epsilon(0.1));
		CHECK(halos[0].Energy == doctest::Approx(halos[1].Energy).epsilon(0.1));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bloom radius scales the halo")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		BloomScene bloom(assets);
		SceneRenderer renderer(ExactSpecification());
		const float standard = MeasureBloomHalo(renderer, bloom, { 1920, 1080 }).RadiusOfWidth;
		bloom.PostProcess->BloomRadius = 0.03f;
		const float half = MeasureBloomHalo(renderer, bloom, { 1920, 1080 }).RadiusOfWidth;
		bloom.PostProcess->BloomRadius = 0.12f;
		const float twice = MeasureBloomHalo(renderer, bloom, { 1920, 1080 }).RadiusOfWidth;
		CHECK(half / standard == doctest::Approx(0.5).epsilon(0.25));
		CHECK(twice / standard == doctest::Approx(2.0).epsilon(0.25));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("At 1080 rows the default bloom is 0.6.0's")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		BloomScene bloom(assets);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(1920, 1080);
		const ReadbackImage standard = RenderToImage(renderer, bloom.SceneData, bloom.Camera);
		CHECK(renderer.GetStats().BloomLevels == 6);
		CHECK(SceneRenderer::GetBloomSpan(PostProcessComponent().BloomRadius, 1080, RenderData::c_MaxBloomLevels) == 6.0f);
		// The radius as the Inspector shows and a file stores it (0.0592593, seven digits) snaps to the same six levels, bit for
		// bit: no seventh level of a weight near 0.
		for (const float radius : { 0.0592593f, 64.0f / 1080.0f + 1e-6f, 64.0f / 1080.0f - 1e-6f })
		{
			bloom.PostProcess->BloomRadius = radius;
			CAPTURE(radius);
			CHECK(MaxDifference(RenderToImage(renderer, bloom.SceneData, bloom.Camera), standard) == 0);
			CHECK(renderer.GetStats().BloomLevels == 6);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The vignette leaves the center, is symmetric and falls like cos^4 of the field angle")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		FlatField field(assets, LuminanceForCode(200.0f));
		constexpr uint32_t c_Width = 160;
		constexpr uint32_t c_Height = 90;
		field.Camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage plain = RenderToImage(renderer, field.SceneData, field.Camera);
		field.PostProcess->Vignette = 0.12f;
		const ReadbackImage vignetted = RenderToImage(renderer, field.SceneData, field.Camera);
		CHECK(GetPixelRGBA8(vignetted, c_Width / 2, c_Height / 2) == GetPixelRGBA8(plain, c_Width / 2, c_Height / 2));
		for (uint32_t y = 0; y < c_Height; y += 7)
		{
			for (uint32_t x = 0; x < c_Width; x += 5)
			{
				CAPTURE(x);
				CAPTURE(y);
				CHECK(std::abs(Red(vignetted, x, y) - Red(vignetted, c_Width - 1 - x, y)) <= 1);
				CHECK(std::abs(Red(vignetted, x, y) - Red(vignetted, x, c_Height - 1 - y)) <= 1);
			}
		}
		const glm::vec2 tangents = SceneRenderer::GetVignetteTangents(field.Camera, { c_Width, c_Height });
		for (const glm::uvec2& pixel : { glm::uvec2(0, 0), glm::uvec2(0, c_Height / 2), glm::uvec2(c_Width / 2, 0) })
		{
			CAPTURE(pixel);
			CHECK(std::abs(static_cast<float>(Red(vignetted, pixel)) - VignettedCode(LuminanceForCode(200.0f), pixel, { c_Width, c_Height }, tangents, 0.12f)) <= 1.0f);
		}
		// At 60 degrees and 16:9, the corners darken by about 9.9%.
		const glm::vec2 corner = tangents;
		const float cos4 = 1.0f / ((1.0f + glm::dot(corner, corner)) * (1.0f + glm::dot(corner, corner)));
		CHECK(0.12f * (1.0f - cos4) == doctest::Approx(0.099f).epsilon(0.02));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Orthographic views vignette as a 60 degree view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		FlatField field(assets, LuminanceForCode(200.0f));
		constexpr uint32_t c_Width = 160;
		constexpr uint32_t c_Height = 90;
		field.Camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), 1.0f);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage plain = RenderToImage(renderer, field.SceneData, field.Camera);
		field.PostProcess->Vignette = 0.12f;
		const ReadbackImage vignetted = RenderToImage(renderer, field.SceneData, field.Camera);
		const SceneCamera perspective = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		const glm::vec2 tangents = SceneRenderer::GetVignetteTangents(perspective, { c_Width, c_Height });
		CHECK(SceneRenderer::GetVignetteTangents(field.Camera, { c_Width, c_Height }) == tangents);
		for (const glm::uvec2& pixel : { glm::uvec2(0, 0), glm::uvec2(0, c_Height / 2) })
		{
			CAPTURE(pixel);
			CHECK(Red(vignetted, pixel) < Red(plain, pixel));
			CHECK(std::abs(static_cast<float>(Red(vignetted, pixel)) - VignettedCode(LuminanceForCode(200.0f), pixel, { c_Width, c_Height }, tangents, 0.12f)) <= 1.0f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
