#pragma once

#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// Helpers shared by the scene renderer GPU tests.
namespace Strata::Tests
{

	constexpr uint32_t c_SceneTestSize = 64;

	// An active asset manager holding only the built-in assets plus assets added by the test.
	struct SceneTestAssets
	{
		Ref<RuntimeAssetManager> Manager;

		SceneTestAssets()
		{
			const std::filesystem::path pack = CreateTemporaryDirectory("SceneRendererAssets") / "Empty.stpak";
			REQUIRE(AssetPack::Write(pack, {}, [](const AssetMetadata&, std::vector<uint8_t>&, std::string*) { return true; }));
			Manager = RuntimeAssetManager::Create(pack);
			REQUIRE(Manager);
			AssetManager::SetActive(Manager);
		}

		~SceneTestAssets()
		{
			AssetManager::SetActive(nullptr);
		}

		SceneTestAssets(const SceneTestAssets&) = delete;
		SceneTestAssets& operator=(const SceneTestAssets&) = delete;

		AssetHandle Add(const Ref<Asset>& asset, const std::string& name)
		{
			REQUIRE(asset);
			AssetMetadata metadata;
			metadata.Name = name;
			return Manager->AddMemoryAsset(asset, metadata);
		}

		AssetHandle AddMaterial(const MaterialProperties& properties)
		{
			return Add(Material::Create(properties), "TestMaterial");
		}

		// A texture from tightly packed pixels (RGBA8 or RGBA8SRGB), uploaded through the asset manager.
		AssetHandle AddTexture(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, TextureFormat format, TextureFilter filter = TextureFilter::Nearest,
			TextureWrap wrap = TextureWrap::Repeat)
		{
			TextureMip level0;
			level0.Width = width;
			level0.Height = height;
			level0.Data = rgba;
			TextureSpecification specification;
			specification.Format = format;
			specification.Filter = filter;
			specification.Wrap = wrap;
			specification.DebugName = "TestTexture";
			// One level: tests sample exact texels, mips would blend them at a distance.
			return Add(Texture::Create(specification, { level0 }), "TestTexture");
		}

		// A texture whose texels all have one color.
		AssetHandle AddSolidTexture(const glm::u8vec4& color, TextureFormat format)
		{
			std::vector<uint8_t> pixels;
			for (int texel = 0; texel < 4; texel++)
				AppendPixel(pixels, color.r, color.g, color.b, color.a);
			return AddTexture(2, 2, pixels, format);
		}
	};

	inline SceneCamera LookAt(const glm::vec3& position, const glm::vec3& target, float aspectRatio = 1.0f)
	{
		SceneCamera camera;
		const glm::vec3 forward = glm::normalize(target - position);
		const glm::vec3 up = std::abs(forward.y) > 0.999f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		camera.View = glm::lookAt(position, target, up);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), aspectRatio, 0.1f, 100.0f);
		camera.Position = position;
		camera.Near = 0.1f;
		camera.Far = 100.0f;
		camera.VerticalFOV = glm::radians(60.0f);
		camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		return camera;
	}

	// Orthographic camera showing `height` world units vertically.
	inline SceneCamera OrthographicLookAt(const glm::vec3& position, const glm::vec3& target, float height)
	{
		SceneCamera camera = LookAt(position, target);
		camera.Projection = Math::OrthographicReverseZ(-height * 0.5f, height * 0.5f, -height * 0.5f, height * 0.5f, 0.1f, 100.0f);
		camera.Orthographic = true;
		camera.VerticalFOV = height;
		return camera;
	}

	// Plain output: no tone curve, no vignette, no bloom, ambient occlusion or anti-aliasing, so unlit colors come out
	// unchanged.
	inline PostProcessComponent& AddNeutralPostProcess(Scene& scene)
	{
		PostProcessComponent& postProcess = scene.CreateEntity("PostProcess").AddComponent<PostProcessComponent>();
		postProcess.Tonemapper = TonemapOperator::None;
		postProcess.AutoExposure = false;
		postProcess.Exposure = 0.0f;
		postProcess.Vignette = 0.0f;
		postProcess.Bloom = false;
		postProcess.AmbientOcclusion = false;
		postProcess.AntiAliasing = false;
		return postProcess;
	}

	inline Entity AddMesh(Scene& scene, AssetHandle mesh, AssetHandle material, const glm::vec3& position, const std::string& name)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetComponent<TransformComponent>().Translation = position;
		MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
		renderer.Mesh = mesh;
		renderer.Material = material;
		return entity;
	}

	// A directional light travelling along `direction`.
	inline Entity AddSun(Scene& scene, const glm::vec3& direction, float intensity, bool castShadows = true)
	{
		Entity sun = scene.CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(direction);
		DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
		light.Intensity = intensity;
		light.CastShadows = castShadows;
		return sun;
	}

	inline ReadbackImage RenderToImage(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		REQUIRE(image.Width == renderer.GetViewportSize().x);
		return image;
	}

	inline MaterialProperties UnlitColor(const glm::vec4& color)
	{
		MaterialProperties properties;
		properties.BaseColor = color;
		properties.Unlit = true;
		return properties;
	}

	// An unlit surface of the given (HDR) luminance.
	inline MaterialProperties Emitting(float luminance)
	{
		MaterialProperties properties = UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		properties.EmissiveColor = glm::vec3(1.0f);
		properties.EmissiveIntensity = luminance;
		return properties;
	}

	inline glm::uvec2 ProjectToPixel(const SceneCamera& camera, const glm::vec3& point, uint32_t width, uint32_t height)
	{
		const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		return glm::uvec2(static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * static_cast<float>(width)),
			static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * static_cast<float>(height)));
	}

	inline glm::uvec2 ProjectToPixel(const SceneCamera& camera, const glm::vec3& point, uint32_t size)
	{
		return ProjectToPixel(camera, point, size, size);
	}

	inline int Red(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		return static_cast<int>(GetPixelRGBA8(image, x, y).r);
	}

	inline int Red(const ReadbackImage& image, const glm::uvec2& pixel)
	{
		return Red(image, pixel.x, pixel.y);
	}

	// Largest per-channel difference between two RGBA8 images of the same size.
	inline int MaxDifference(const ReadbackImage& a, const ReadbackImage& b)
	{
		REQUIRE(a.Width == b.Width);
		REQUIRE(a.Height == b.Height);
		REQUIRE(a.Pixels.size() == b.Pixels.size());
		int difference = 0;
		for (size_t index = 0; index < a.Pixels.size(); index++)
			difference = std::max(difference, std::abs(static_cast<int>(a.Pixels[index]) - static_cast<int>(b.Pixels[index])));
		return difference;
	}

	inline glm::vec4 ReadHalfTexel(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		REQUIRE(x < image.Width);
		REQUIRE(y < image.Height);
		uint16_t texel[4];
		std::memcpy(texel, image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * image.BytesPerPixel, sizeof(texel));
		return glm::vec4(glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]), glm::unpackHalf1x16(texel[2]), glm::unpackHalf1x16(texel[3]));
	}

	inline float ReadFloatTexel(const ReadbackImage& image, const glm::uvec2& pixel)
	{
		REQUIRE(pixel.x < image.Width);
		REQUIRE(pixel.y < image.Height);
		float value = 0.0f;
		std::memcpy(&value, image.Pixels.data() + (static_cast<size_t>(pixel.y) * image.Width + pixel.x) * sizeof(float), sizeof(float));
		return value;
	}

	// Equirectangular HDR texture: `top` above the horizon, `bottom` below it.
	inline Ref<Texture> CreateEnvironmentTexture(const glm::vec3& top, const glm::vec3& bottom)
	{
		TextureMip level0;
		level0.Width = 64;
		level0.Height = 32;
		level0.Data.resize(64 * 32 * 8);
		for (uint32_t y = 0; y < 32; y++)
		{
			const glm::vec3 color = y < 16 ? top : bottom;
			for (uint32_t x = 0; x < 64; x++)
			{
				const uint16_t texel[4] = { glm::packHalf1x16(color.r), glm::packHalf1x16(color.g), glm::packHalf1x16(color.b), glm::packHalf1x16(1.0f) };
				std::memcpy(level0.Data.data() + (static_cast<size_t>(y) * 64 + x) * sizeof(texel), texel, sizeof(texel));
			}
		}
		std::vector<TextureMip> mips = { level0 };
		REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA16F, false));
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA16F;
		specification.Wrap = TextureWrap::Clamp;
		Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);
		return texture;
	}

}
