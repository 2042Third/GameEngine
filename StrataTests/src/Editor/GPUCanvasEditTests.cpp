#include <doctest/doctest.h>

#include "Editor/CanvasEdit.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Math/Math.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Scene.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>

using namespace Strata;
using namespace Strata::Tests;

// The 2D canvas mode's rendering (Editor/CanvasEdit.h): editor code, so its GPU tests build only with the editor.
TEST_SUITE("GPU.Editor.CanvasEdit")
{
	TEST_CASE("The canvas mode's cropped camera renders the visible part of the whole view, pixel for pixel")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("Crop");
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), { -1.5f, 0.5f, 0.0f }, "Red");
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), { 1.0f, -0.5f, -1.0f }, "Green");
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f })), { 0.3f, 1.2f, 1.0f }, "Blue");
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			SceneCamera camera = orthographic ? OrthographicLookAt({ 0.0f, 1.0f, 6.0f }, { 0.0f, 0.0f, 0.0f }, 5.0f) : LookAt({ 0.0f, 1.0f, 6.0f }, { 0.0f, 0.0f, 0.0f }, 2.0f);
			if (orthographic)
				camera.Projection = Math::OrthographicReverseZ(-5.0f, 5.0f, -2.5f, 2.5f, 0.1f, 100.0f);
			camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			renderer.SetViewportSize(800, 400);
			const ReadbackImage whole = RenderToImage(renderer, scene, camera);

			const UIBox display { { 0.0f, 0.0f }, { 800.0f, 400.0f } };
			const UIBox visible { { 200.0f, 100.0f }, { 400.0f, 200.0f } };
			renderer.SetViewportSize(400, 200);
			const ReadbackImage part = RenderToImage(renderer, scene, CanvasEdit::CropCamera(camera, display, visible));
			uint32_t mismatches = 0;
			uint32_t colored = 0;
			for (uint32_t y = 0; y < part.Height; y++)
			{
				for (uint32_t x = 0; x < part.Width; x++)
				{
					const glm::ivec4 a(GetPixelRGBA8(part, x, y));
					const glm::ivec4 b(GetPixelRGBA8(whole, x + 200, y + 100));
					const glm::ivec4 difference = glm::abs(a - b);
					if (std::max({ difference.r, difference.g, difference.b }) > 8)
						mismatches++;
					if (a.r + a.g + a.b > 0)
						colored++;
				}
			}
			CHECK(colored > part.Width * part.Height / 10); // The cubes are in view
			CHECK(mismatches <= part.Width * part.Height / 200); // Only edges may round differently
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
