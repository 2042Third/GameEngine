#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/SceneGizmos.h"

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	// Looking straight down on the y = 0 plane, 8 pixels per world unit, with pixel centers on whole coordinates: the
	// world origin is the center of pixel (32, 32), +X goes right and +Z down the image.
	SceneCamera TopDownCamera()
	{
		const float offset = 0.5f / 8.0f;
		return OrthographicLookAt(glm::vec3(-offset, 10.0f, -offset), glm::vec3(-offset, 0.0f, -offset), 8.0f);
	}

	glm::uvec2 GridPixel(float x, float z)
	{
		return glm::uvec2(static_cast<uint32_t>(32.0f + x * 8.0f), static_cast<uint32_t>(32.0f + z * 8.0f));
	}

	glm::u8vec4 PixelAt(const ReadbackImage& image, const glm::uvec2& pixel)
	{
		REQUIRE(pixel.x < image.Width);
		REQUIRE(pixel.y < image.Height);
		return GetPixelRGBA8(image, pixel.x, pixel.y);
	}

	// Whether a pixel within `radius` of `pixel` has (about) the given color: one pixel wide lines may land on either
	// side of their ideal position depending on the rasterizer.
	bool HasColorNear(const ReadbackImage& image, const glm::uvec2& pixel, const glm::u8vec4& color, int radius = 1)
	{
		for (int y = -radius; y <= radius; y++)
		{
			for (int x = -radius; x <= radius; x++)
			{
				const int px = static_cast<int>(pixel.x) + x;
				const int py = static_cast<int>(pixel.y) + y;
				if (px < 0 || py < 0 || px >= static_cast<int>(image.Width) || py >= static_cast<int>(image.Height))
					continue;
				const glm::ivec4 difference = glm::abs(glm::ivec4(GetPixelRGBA8(image, static_cast<uint32_t>(px), static_cast<uint32_t>(py))) - glm::ivec4(color));
				if (difference.r <= 2 && difference.g <= 2 && difference.b <= 2)
					return true;
			}
		}
		return false;
	}

}

TEST_SUITE("GPU.SceneRenderer.Overlays")
{
	TEST_CASE("The grid shows minor, major and axis lines hidden behind geometry")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// A white block above the grid over x = 1.5..3.5, z = -3.5..-1.5.
		Entity block = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f })), glm::vec3(2.5f, 0.5f, -2.5f), "Block");
		block.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = TopDownCamera();
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);
		CHECK(PixelAt(plain, GridPixel(0.0f, 2.5f)) == glm::u8vec4(0, 0, 0, 255));

		SceneRenderOptions options;
		options.ShowGrid = true;
		options.GridMajorEvery = 2;
		options.GridMinorColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.5f);
		options.GridMajorColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
		options.GridAxisXColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		options.GridAxisZColor = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage grid;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), grid));

		CHECK(PixelAt(grid, GridPixel(2.5f, 0.0f)) == glm::u8vec4(255, 0, 0, 255));   // X axis (z = 0)
		CHECK(PixelAt(grid, GridPixel(0.0f, 2.5f)) == glm::u8vec4(0, 0, 255, 255));   // Z axis (x = 0)
		CHECK(PixelAt(grid, GridPixel(2.0f, 1.5f)) == glm::u8vec4(255, 255, 0, 255)); // Major line (every second)
		const glm::u8vec4 minor = PixelAt(grid, GridPixel(1.0f, 1.5f));
		CHECK(std::abs(minor.r - 64) <= 2); // Half-transparent mid gray over black
		CHECK(minor.r == minor.b);
		CHECK(PixelAt(grid, GridPixel(1.5f, 1.5f)) == glm::u8vec4(0, 0, 0, 255)); // Between lines
		CHECK(PixelAt(grid, GridPixel(1.5f, 1.0f + 1.0f / 8.0f)) == glm::u8vec4(0, 0, 0, 255)); // Lines are one pixel wide
		// The block hides the grid.
		CHECK(PixelAt(grid, GridPixel(2.0f, -2.5f)) == glm::u8vec4(255, 255, 255, 255));
		CHECK(PixelAt(grid, GridPixel(1.0f, -2.5f)) == PixelAt(grid, GridPixel(1.0f, 1.5f)));

		// Beyond the fade distance nothing remains.
		options.GridFadeDistance = 5.0f; // The camera is 10 units above the grid
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), grid));
		CHECK(MaxDifference(grid, plain) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Selected entities get an outline around their visible silhouette")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity left = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(-1.0f, 0.0f, 0.0f), "Left");
		Entity right = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), glm::vec3(1.0f, 0.0f, 0.0f), "Right");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);

		// Silhouette edges along the middle row.
		const uint32_t row = c_Size / 2;
		auto firstPixel = [&](const glm::u8vec4& color, bool fromLeft)
		{
			for (uint32_t step = 0; step < c_Size; step++)
			{
				const uint32_t x = fromLeft ? step : c_Size - 1 - step;
				if (GetPixelRGBA8(plain, x, row) == color)
					return x;
			}
			FAIL("Color not found in the row");
			return 0u;
		};
		const uint32_t leftEdge = firstPixel(glm::u8vec4(255, 0, 0, 255), true);
		const uint32_t rightEdge = firstPixel(glm::u8vec4(0, 255, 0, 255), false);
		REQUIRE(leftEdge >= 6);
		REQUIRE(rightEdge + 6 < c_Size);

		const Entity selection[] = { left };
		SceneRenderOptions options;
		options.SelectedEntities = selection;
		options.SelectionColor = glm::vec4(1.0f, 0.6f, 0.0f, 1.0f);
		options.OutlineWidth = 2;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().OutlinedEntities == 1);
		ReadbackImage outlined;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), outlined));
		const glm::u8vec4 orange(255, 153, 0, 255);
		CHECK(GetPixelRGBA8(outlined, leftEdge - 1, row) == orange);
		CHECK(GetPixelRGBA8(outlined, leftEdge - 2, row) == orange);
		CHECK(GetPixelRGBA8(outlined, leftEdge - 3, row) == glm::u8vec4(0, 0, 0, 255));
		CHECK(GetPixelRGBA8(outlined, leftEdge, row) == glm::u8vec4(255, 0, 0, 255)); // The object itself is untouched
		CHECK(GetPixelRGBA8(outlined, rightEdge + 1, row) == glm::u8vec4(0, 0, 0, 255)); // Not selected

		// Wider outlines; selecting both; entities of another scene are ignored.
		options.OutlineWidth = 4;
		const Entity both[] = { left, right, left };
		options.SelectedEntities = both;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().OutlinedEntities == 2);
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), outlined));
		CHECK(GetPixelRGBA8(outlined, leftEdge - 4, row) == orange);
		CHECK(GetPixelRGBA8(outlined, rightEdge + 4, row) == orange);

		Scene other;
		const Entity foreign[] = { other.CreateEntity("Foreign") };
		options.SelectedEntities = foreign;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().OutlinedEntities == 0);
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), outlined));
		CHECK(MaxDifference(outlined, plain) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Debug lines are hidden by geometry unless drawn on top")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.5f, 0.5f, 0.5f, 1.0f })), glm::vec3(0.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);
		const glm::u8vec4 wall = GetPixelRGBA8(plain, c_Size / 2, c_Size / 2);

		DebugDraw debugDraw;
		debugDraw.Line(glm::vec3(-1.0f, 0.5f, -1.0f), glm::vec3(1.0f, 0.5f, -1.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));                         // Behind the wall
		debugDraw.Line(glm::vec3(-1.0f, -0.5f, -1.0f), glm::vec3(1.0f, -0.5f, -1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), DebugDrawDepth::OnTop); // Behind, on top
		debugDraw.Line(glm::vec3(-1.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));                           // In front
		debugDraw.Box(AABB(glm::vec3(-0.25f), glm::vec3(0.25f)), glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), DebugDrawDepth::OnTop);
		SceneRenderOptions options;
		options.DebugShapes = &debugDraw;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().DebugLines == 3 + 12);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));

		const glm::uvec2 hidden = ProjectToPixel(camera, glm::vec3(0.6f, 0.5f, -1.0f), c_Size);
		const glm::uvec2 onTop = ProjectToPixel(camera, glm::vec3(0.6f, -0.5f, -1.0f), c_Size);
		const glm::uvec2 inFront = ProjectToPixel(camera, glm::vec3(0.6f, 0.0f, 1.0f), c_Size);
		CHECK_FALSE(HasColorNear(image, hidden, glm::u8vec4(255, 0, 0, 255)));
		CHECK(GetPixelRGBA8(image, hidden.x, hidden.y) == wall);
		CHECK(HasColorNear(image, onTop, glm::u8vec4(0, 255, 0, 255)));
		CHECK(HasColorNear(image, inFront, glm::u8vec4(0, 0, 255, 255)));
		CHECK(HasColorNear(image, ProjectToPixel(camera, glm::vec3(0.25f, 0.0f, 0.25f), c_Size), glm::u8vec4(255, 255, 0, 255)));

		// Away from the lines the image is unchanged, and an empty list draws nothing.
		CHECK(GetPixelRGBA8(image, 4, 4) == GetPixelRGBA8(plain, 4, 4));
		debugDraw.Clear();
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().DebugLines == 0);
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		CHECK(MaxDifference(image, plain) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Scene gizmos and overlays render into external framebuffers")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity lamp = scene.CreateEntity("Lamp");
		PointLightComponent& light = lamp.AddComponent<PointLightComponent>();
		light.Range = 1.0f;
		light.Color = glm::vec3(1.0f, 0.0f, 1.0f);
		Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), glm::vec3(0.0f, -1.5f, 0.0f), "Cube");
		cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		scene.UpdateWorldTransforms();

		DebugDraw debugDraw;
		DrawSceneGizmos(debugDraw, scene);
		const Entity selection[] = { cube };
		SceneRenderOptions options;
		options.ShowGrid = true;
		options.SelectedEntities = selection;
		options.DebugShapes = &debugDraw;

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f));
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage internal;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), internal));
		// The light's range sphere: its circle facing the camera passes one unit right of the light.
		CHECK(HasColorNear(internal, ProjectToPixel(camera, glm::vec3(1.0f, 0.0f, 0.0f), c_Size), glm::u8vec4(255, 0, 255, 255)));

		nvrhi::TextureDesc desc;
		desc.width = c_Size;
		desc.height = c_Size;
		desc.format = nvrhi::Format::BGRA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = gpu.GetNvrhiDevice()->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = gpu.GetNvrhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		REQUIRE(renderer.Render(scene, camera, framebuffer, options));
		ReadbackImage external;
		REQUIRE(Renderer::ReadTexture(target, external));
		for (size_t index = 0; index + 3 < external.Pixels.size(); index += 4)
			std::swap(external.Pixels[index], external.Pixels[index + 2]); // BGRA to RGBA
		CHECK(MaxDifference(internal, external) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
