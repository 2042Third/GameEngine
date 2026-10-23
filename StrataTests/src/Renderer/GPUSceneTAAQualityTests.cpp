#include "Renderer/TAATestUtils.h"

#include <cmath>
#include <cstdint>
#include <vector>

// The TAA's presets and tunables (PostProcessComponent::TAAPreset, TemporalAntiAliasingSettings): each technique of the
// resolve and the tone mapping's sharpening, against Original, the first TAA.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	// A half-transparent quad over the scene (blended, drawn after the opaque surfaces).
	Entity AddTransparentQuad(Scene& scene, SceneTestAssets& assets, const glm::vec4& color, const glm::vec3& position, float size)
	{
		MaterialProperties properties = UnlitColor(color);
		properties.AlphaMode = MaterialAlphaMode::Blend;
		Entity quad = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(properties), position, "Quad");
		quad.GetComponent<TransformComponent>().Scale = glm::vec3(size);
		return quad;
	}

	// Moves the boxes, the quad and the camera of a TemporalScene one frame on.
	void Animate(TemporalScene& scene, Entity quad, uint32_t frame)
	{
		Entity box = scene.Boxes[static_cast<size_t>(frame) % scene.Boxes.size()];
		box.GetComponent<TransformComponent>().Translation.y += 0.1f;
		box.MarkModified<TransformComponent>();
		quad.GetComponent<TransformComponent>().Translation.x += 0.15f;
		quad.MarkModified<TransformComponent>();
		scene.Camera = LookAt(glm::vec3(0.05f * static_cast<float>(frame + 1), 9.0f, 8.0f), glm::vec3(0.0f, 0.0f, -2.5f));
	}

}

TEST_SUITE("GPU.SceneRenderer.TAA")
{
	TEST_CASE("Original resolves exactly as Custom with Original's values, whatever Custom's unused fields hold")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		TemporalScene scene(TemporalAntiAliasingPreset::Original);
		Entity quad = AddTransparentQuad(scene.Contents, assets, { 1.0f, 0.2f, 0.1f, 0.5f }, glm::vec3(-3.0f, 1.5f, -1.0f), 2.0f);
		PostProcessComponent& postProcess = GetPostProcess(scene.Contents);
		// Custom's start from Original (the stored tunables are Sharp's table, the defaults, so ClipWidth and the threshold
		// keep Sharp's values, which Original's modes do not read).
		PostProcessComponent custom = postProcess;
		custom.TAAPreset = TemporalAntiAliasingPreset::Custom;
		SetTemporalAntiAliasingSettings(custom, GetTemporalAntiAliasingCustomStart(postProcess));
		REQUIRE(custom.TAAClipWidth == c_TAASharpSettings.ClipWidth);
		REQUIRE(custom.TAADisocclusionThreshold == c_TAASharpSettings.DisocclusionThreshold);
		const PostProcessComponent original = postProcess;

		SceneRenderer originalRenderer = CreateRenderer("Original");
		SceneRenderer customRenderer = CreateRenderer("Custom");
		originalRenderer.SetViewportSize(c_Size, c_Size);
		customRenderer.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 16; frame++)
		{
			CAPTURE(frame);
			postProcess = original;
			const ReadbackImage originalImage = RenderToImage(originalRenderer, scene.Contents, scene.Camera);
			postProcess = custom;
			const ReadbackImage customImage = RenderToImage(customRenderer, scene.Contents, scene.Camera);
			CHECK(customRenderer.GetStats().TemporalFrames == frame + 1);
			CHECK(ReadHDRBits(originalRenderer.GetResolvedHDRTexture()) == ReadHDRBits(customRenderer.GetResolvedHDRTexture()));
			CHECK(originalImage.Pixels == customImage.Pixels);
			Animate(scene, quad, frame);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The history's alpha holds each pixel's signed view distance, and the sky's where nothing was drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Original);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.4f, 0.4f, 0.4f, 1.0f })), glm::vec3(0.0f), "Ground")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// Above an orthographic camera whose near plane lies behind it: a negative distance.
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.8f, 0.2f, 0.2f, 1.0f })), glm::vec3(-1.5f, 12.0f, 0.0f), "Above")
			.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		SceneCamera orthographic = OrthographicLookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 8.0f);
		orthographic.Projection = Math::OrthographicReverseZ(-4.0f, 4.0f, -4.0f, 4.0f, -10.0f, 100.0f);
		orthographic.Near = -10.0f;
		SceneRenderer renderer = CreateRenderer("Distance");
		renderer.SetViewportSize(c_Size, c_Size);
		// The first frame (a reset) and a blended one write it alike.
		for (uint32_t frame = 0; frame < 2; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene, orthographic));
			uint32_t ground = 0;
			uint32_t above = 0;
			for (const glm::vec4& texel : ReadHDRA(renderer.GetResolvedHDRTexture()))
			{
				ground += std::abs(texel.a - 10.0f) < 0.01f ? 1u : 0u;
				above += std::abs(texel.a + 2.0f) < 0.01f ? 1u : 0u;
			}
			CHECK(above >= 15u * 15u);
			CHECK(ground + above == c_Size * c_Size);
		}

		// Level with the ground: the sky above the horizon, the ground below it, nearer toward the bottom.
		const SceneCamera level = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -10.0f));
		REQUIRE(renderer.Render(scene, level));
		const std::vector<glm::vec4> texels = ReadHDRA(renderer.GetResolvedHDRTexture());
		for (uint32_t x = 0; x < c_Size; x++)
		{
			CAPTURE(x);
			CHECK(texels[x].a == RenderData::c_TemporalSkyDistance);
			const float bottom = texels[static_cast<size_t>(c_Size - 1) * c_Size + x].a;
			CHECK(bottom > 0.5f);
			CHECK(bottom < 3.0f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
