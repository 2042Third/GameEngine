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
}
