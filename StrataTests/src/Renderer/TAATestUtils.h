#pragma once

#include "Renderer/SceneRendererTestUtils.h"

#include <cstdint>
#include <cstring>
#include <vector>

// Helpers and scenes shared by the temporal anti-aliasing tests (GPU.SceneRenderer.TAA).
namespace Strata::Tests::TAA
{

	constexpr uint32_t c_Size = 64;

	constexpr glm::vec3 c_LuminanceWeights(0.2126f, 0.7152f, 0.0722f); // Include/Exposure.glsl

	inline const char* PathName(GPUDrivenMode mode)
	{
		return mode == GPUDrivenMode::Off ? "CPU path" : "GPU-driven path";
	}

	inline SceneRenderer CreateRenderer(const char* name, GPUDrivenMode mode = GPUDrivenMode::Off, bool temporal = true)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		specification.TemporalAntiAliasing = temporal;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	inline void LoadAssets(SceneTestAssets& assets)
	{
		for (const AssetHandle builtin : { BuiltinAssets::CubeMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
	}

	// What an application's device frame does after its work (GraphicsDevice::EndFrame): the GPU is done with it, and NVRHI
	// learns so (the frame constants' versions are used again). Tests that render many frames without reading anything back
	// need it.
	inline void FinishFrame(GPUContext& gpu)
	{
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

	inline ReadbackImage ReadHalfImage(nvrhi::ITexture* texture)
	{
		REQUIRE(texture);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture, image));
		REQUIRE(image.BytesPerPixel == 8);
		return image;
	}

	// An RGBA16_FLOAT image's texels, row by row.
	inline std::vector<glm::vec4> ReadHDRA(nvrhi::ITexture* texture)
	{
		const ReadbackImage image = ReadHalfImage(texture);
		std::vector<glm::vec4> texels;
		texels.reserve(static_cast<size_t>(image.Width) * image.Height);
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				texels.push_back(ReadHalfTexel(image, x, y));
		}
		return texels;
	}

	// An RGBA16_FLOAT image's colors, row by row.
	inline std::vector<glm::vec3> ReadHDR(nvrhi::ITexture* texture)
	{
		std::vector<glm::vec3> colors;
		for (const glm::vec4& texel : ReadHDRA(texture))
			colors.push_back(glm::vec3(texel));
		return colors;
	}

	// The half-float bits of an RGBA16_FLOAT image's colors (its alpha left out), row by row: for exact comparisons.
	inline std::vector<uint16_t> ReadHDRBits(nvrhi::ITexture* texture)
	{
		const ReadbackImage image = ReadHalfImage(texture);
		const size_t texels = static_cast<size_t>(image.Width) * image.Height;
		std::vector<uint16_t> bits(texels * 3);
		for (size_t texel = 0; texel < texels; texel++)
			std::memcpy(bits.data() + texel * 3, image.Pixels.data() + texel * image.BytesPerPixel, 3 * sizeof(uint16_t));
		return bits;
	}

	inline std::vector<uint32_t> ReadIDs(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), image));
		REQUIRE(image.BytesPerPixel == 4);
		std::vector<uint32_t> ids(static_cast<size_t>(image.Width) * image.Height);
		std::memcpy(ids.data(), image.Pixels.data(), ids.size() * sizeof(uint32_t));
		return ids;
	}

	inline uint32_t GetID(Entity entity)
	{
		return static_cast<uint32_t>(entity.GetHandle()) + 1;
	}

	// The temporal statistics of a frame that started the history again (the first frame, jitter phase 0), whose resolved
	// image is the frame's own.
	inline void CheckReset(SceneRenderer& renderer, uint32_t resets)
	{
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.TemporalAntiAliasing);
		CHECK(stats.TemporalFrames == 1);
		CHECK(stats.HistoryResets == resets);
		CHECK(stats.Jitter == SceneRenderer::GetTemporalJitter(0));
		REQUIRE(renderer.GetTemporalHistoryTexture());
		CHECK(renderer.GetResolvedHDRTexture() == renderer.GetTemporalHistoryTexture());
		CHECK(ReadHDR(renderer.GetResolvedHDRTexture()) == ReadHDR(renderer.GetHDRTexture()));
	}

	// Rows of boxes on a ground under a shadowed sun, seen from above at an angle.
	struct TemporalScene
	{
		Scene Contents;
		std::vector<Entity> Boxes;
		Entity Ground;
		SceneCamera Camera;

		explicit TemporalScene(TemporalAntiAliasingPreset preset)
		{
			AddTemporalPostProcess(Contents, preset);
			AddSun(Contents, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
			for (uint32_t index = 0; index < 12; index++)
				Boxes.push_back(AddMesh(Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 4) * 2.0f - 3.0f, 0.0f,
					-static_cast<float>(index / 4) * 2.5f), "Box"));
			Ground = AddMesh(Contents, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f, -0.5f, 0.0f), "Ground");
			Ground.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			Camera = LookAt(glm::vec3(0.0f, 9.0f, 8.0f), glm::vec3(0.0f, 0.0f, -2.5f));
		}
	};

	// An unlit ground and an unlit plane over it of the same luminance (the resolve's luminance weights are then equal), meeting
	// along a slanted edge through the middle of the view from above.
	struct EdgeScene
	{
		Scene Contents;
		SceneCamera Camera;
		glm::vec3 Ground = glm::vec3(0.6f, 0.3f, 0.3f);
		glm::vec3 Top;

		EdgeScene(SceneTestAssets& assets, TemporalAntiAliasingPreset preset)
		{
			AddTemporalPostProcess(Contents, preset);
			const float luminance = glm::dot(Ground, c_LuminanceWeights);
			Top = glm::vec3(0.2f, (luminance - 0.2126f * 0.2f - 0.0722f * 0.6f) / 0.7152f, 0.6f);
			Entity ground = AddMesh(Contents, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor(glm::vec4(Ground, 1.0f))), glm::vec3(0.0f), "Ground");
			ground.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			// Its edge at local x = -5 passes through the origin.
			const glm::quat rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			Entity top = AddMesh(Contents, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor(glm::vec4(Top, 1.0f))),
				rotation * glm::vec3(5.0f, 0.0f, 0.0f) + glm::vec3(0.0f, 0.01f, 0.0f), "Top");
			TransformComponent& transform = top.GetComponent<TransformComponent>();
			transform.Rotation = rotation;
			transform.Scale = glm::vec3(10.0f);
			Camera = OrthographicLookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 3.0f);
		}
	};

}
