#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/SceneGizmos.h"

#include <algorithm>
#include <cstring>
#include <vector>

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

	TEST_CASE("The grid stays visible on a floor at its height")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// A floor exactly in the grid's plane: both have the same depth, up to rounding.
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.1f, 0.1f, 0.1f, 1.0f })), glm::vec3(0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);

		SceneRenderOptions options;
		options.ShowGrid = true;
		options.GridMinorColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		options.GridMajorColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		options.GridAxisXColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		options.GridAxisZColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		auto render = [&](const SceneCamera& camera, bool grid, bool withFloor)
		{
			floor.SetActive(withFloor);
			SceneRenderOptions frameOptions = options;
			frameOptions.ShowGrid = grid;
			REQUIRE(renderer.Render(scene, camera, nullptr, frameOptions));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			return image;
		};

		// The grid over the floor must look like the grid over nothing, blended onto the floor's color: wherever the
		// floor's depth wins a tie, a line would disappear. Grazing and distant views compute the two depths most
		// differently.
		for (const SceneCamera& camera : { TopDownCamera(), LookAt(glm::vec3(0.3f, 4.0f, 5.0f), glm::vec3(0.0f)), LookAt(glm::vec3(0.37f, 0.8f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
			LookAt(glm::vec3(1.3f, 9.0f, 12.0f), glm::vec3(0.0f)) })
		{
			const ReadbackImage floorOnly = render(camera, false, true);
			const ReadbackImage gridOnly = render(camera, true, false);
			const ReadbackImage gridOnFloor = render(camera, true, true);
			const int floorValue = Red(floorOnly, c_Size / 2, c_Size - 1);
			REQUIRE(std::abs(floorValue - 89) <= 1); // 0.1 linear, sRGB-encoded
			uint32_t gridPixels = 0;
			uint32_t mismatches = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					if (Red(floorOnly, x, y) == 0)
						continue; // Beyond the floor
					const int line = Red(gridOnly, x, y);
					gridPixels += line > 0 ? 1u : 0u;
					const int expected = line + (255 - line) * floorValue / 255;
					mismatches += std::abs(Red(gridOnFloor, x, y) - expected) > 3 ? 1u : 0u;
				}
			}
			CHECK(gridPixels > 100);
			CHECK(mismatches == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With TAA the grid on a floor and text on a wall cover the same pixels in every jitter phase")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		// A black floor (what the grid draws over it is its color times its coverage); a blue box and a blue wall hide some grid
		// lines.
		const AssetHandle black = assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f }));
		AddMesh(scene, BuiltinAssets::PlaneMesh, black, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f })), glm::vec3(0.6f, 0.5f, 0.3f), "Box");
		// A wall seen at a grazing angle (a steep depth slope) and text 2 mm in front of it.
		const glm::quat wallRotation = glm::angleAxis(glm::radians(70.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Entity wall = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 0.25f, 1.0f })), glm::vec3(-1.5f, 1.5f, -1.0f), "Wall");
		wall.GetComponent<TransformComponent>().Rotation = wallRotation;
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
		Entity sign = scene.CreateEntity("Sign");
		TextComponent& text = sign.AddComponent<TextComponent>();
		text.Text = "WALL";
		text.FontSize = 0.6f;
		text.ScreenSpace = false;
		text.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
		sign.GetComponent<TransformComponent>().Translation = glm::vec3(-1.5f, 1.5f, -1.0f) + wallRotation * glm::vec3(0.0f, 0.0f, 0.002f);
		sign.GetComponent<TransformComponent>().Rotation = wallRotation;
		const SceneCamera camera = LookAt(glm::vec3(0.4f, 2.5f, 4.0f), glm::vec3(-0.3f, 0.6f, 0.0f));

		SceneRenderOptions options;
		options.ShowGrid = true;
		options.GridMinorColor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
		options.GridMajorColor = options.GridMinorColor;
		options.GridAxisXColor = options.GridMinorColor;
		options.GridAxisZColor = options.GridMinorColor;
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		// The text's glyphs are rasterized over the first frames.
		for (uint32_t frame = 0; frame < 120; frame++)
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			if (renderer.GetStats().PendingTextGlyphs == 0 && renderer.GetStats().PendingAssets == 0)
				break;
		}
		REQUIRE(renderer.GetStats().PendingTextGlyphs == 0);

		// Per phase: the grid's pixels (red), the text's (green) and the box's and the wall's (blue, no grid over them).
		struct Coverage
		{
			std::vector<bool> Grid;
			std::vector<bool> Text;
			std::vector<bool> Box;
		};
		std::vector<Coverage> phases;
		for (uint32_t phase = 0; phase < SceneRenderer::c_TemporalJitterPhases; phase++)
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			REQUIRE(renderer.GetStats().TemporalAntiAliasing);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			Coverage coverage;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					const glm::u8vec4 color = GetPixelRGBA8(image, x, y);
					coverage.Grid.push_back(color.r > 0);
					coverage.Text.push_back(color.g > 0);
					coverage.Box.push_back(color.b > 0 && color.r == 0);
				}
			}
			phases.push_back(std::move(coverage));
		}

		const Coverage& first = phases[0];
		CHECK(std::count(first.Grid.begin(), first.Grid.end(), true) > 200);
		CHECK(std::count(first.Text.begin(), first.Text.end(), true) > 30);
		// Whether a pixel is within one pixel of the box in either of two phases.
		const auto nearBox = [](const Coverage& a, const Coverage& b, uint32_t x, uint32_t y)
		{
			for (uint32_t ny = y > 0 ? y - 1 : 0; ny <= std::min(y + 1, c_Size - 1); ny++)
			{
				for (uint32_t nx = x > 0 ? x - 1 : 0; nx <= std::min(x + 1, c_Size - 1); nx++)
				{
					if (a.Box[ny * c_Size + nx] || b.Box[ny * c_Size + nx])
						return true;
				}
			}
			return false;
		};
		for (size_t phase = 1; phase < phases.size(); phase++)
		{
			CAPTURE(phase);
			// The text, steady; the grid too, but where the box's or the wall's silhouette hides it: that edge follows the jittered
			// depth.
			uint32_t textMoved = 0;
			for (size_t pixel = 0; pixel < first.Text.size(); pixel++)
				textMoved += phases[phase].Text[pixel] != first.Text[pixel] ? 1u : 0u;
			CHECK(textMoved == 0);
			uint32_t moved = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					if (phases[phase].Grid[y * c_Size + x] != first.Grid[y * c_Size + x] && !nearBox(phases[phase], first, x, y))
						moved++;
				}
			}
			CHECK(moved == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With TAA the selection outline comes from the selected instances drawn without the jitter, on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(mode == GPUDrivenMode::Auto);
			Scene scene;
			PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
			postProcess.AntiAliasing = true;
			postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
			// A selected box partly hidden behind another; the outline goes around what is visible of it.
			Entity selected = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(-0.4f, 0.0f, 0.0f), "Selected");
			selected.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.4f, 0.7f, 0.1f));
			Entity front = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), glm::vec3(0.6f, -0.3f, 1.0f), "Front");
			front.GetComponent<TransformComponent>().Scale = glm::vec3(0.6f);
			SceneRendererSpecification specification;
			specification.TextureStreaming = false;
			specification.GPUDriven = mode;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.5f, 4.0f), glm::vec3(0.0f));
			const Entity selection[] = { selected };
			SceneRenderOptions options;
			options.SelectedEntities = selection;
			options.SelectionColor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
			options.OutlineWidth = 2;

			// Outline pixels, and those of the box in front (where the hidden part's edge follows the jittered depth).
			std::vector<bool> first;
			std::vector<bool> firstFront;
			for (uint32_t frame = 0; frame < 2 * SceneRenderer::c_TemporalJitterPhases; frame++)
			{
				CAPTURE(frame);
				REQUIRE(renderer.Render(scene, camera, nullptr, options));
				REQUIRE(renderer.GetStats().TemporalAntiAliasing);
				CHECK(renderer.GetStats().GPUDriven == (mode == GPUDrivenMode::Auto));
				CHECK(renderer.GetStats().OutlinedEntities == 1);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
				std::vector<bool> outline;
				std::vector<bool> nearFront;
				for (uint32_t y = 0; y < c_Size; y++)
				{
					for (uint32_t x = 0; x < c_Size; x++)
					{
						const glm::u8vec4 color = GetPixelRGBA8(image, x, y);
						outline.push_back(color == glm::u8vec4(255, 0, 255, 255));
						nearFront.push_back(color.g > 0 && color.b == 0);
					}
				}
				if (frame == 0)
				{
					first = outline;
					firstFront = nearFront;
					CHECK(std::count(first.begin(), first.end(), true) > 40);
					continue;
				}
				uint32_t moved = 0;
				for (uint32_t y = 0; y < c_Size; y++)
				{
					for (uint32_t x = 0; x < c_Size; x++)
					{
						if (outline[y * c_Size + x] == first[y * c_Size + x])
							continue;
						bool besideFront = false;
						for (uint32_t ny = y > 0 ? y - 1 : 0; ny <= std::min(y + 1, c_Size - 1); ny++)
						{
							for (uint32_t nx = x > 0 ? x - 1 : 0; nx <= std::min(x + 1, c_Size - 1); nx++)
								besideFront = besideFront || nearFront[ny * c_Size + nx] || firstFront[ny * c_Size + nx];
						}
						moved += besideFront ? 0u : 1u;
					}
				}
				CHECK(moved == 0);
			}
		}
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

	TEST_CASE("A renderer without the entity-ID buffer draws the same image, picks nothing and leaves out outlines and tints")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity left = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(-1.0f, 0.0f, 0.0f), "Left");
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), glm::vec3(1.0f, 0.0f, 0.0f), "Right");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));

		SceneRenderer withIDs;
		withIDs.SetViewportSize(c_Size, c_Size);
		const ReadbackImage expected = RenderToImage(withIDs, scene, camera);
		REQUIRE(withIDs.GetEntityIDTexture() != nullptr);

		SceneRendererSpecification specification;
		specification.EntityIDs = false;
		SceneRenderer withoutIDs(specification);
		withoutIDs.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(withoutIDs, scene, camera);
		CHECK(withoutIDs.GetEntityIDTexture() == nullptr);
		CHECK(MaxDifference(image, expected) == 0);
		CHECK_FALSE(withoutIDs.GetEntityAt(scene, c_Size / 4, c_Size / 2));
		CHECK(withoutIDs.ReadEntityIDAsync(c_Size / 4, c_Size / 2) == nullptr);
		CHECK(withIDs.GetEntityAt(scene, ProjectToPixel(camera, glm::vec3(-1.0f, 0.0f, 0.0f), c_Size).x, c_Size / 2) == left);

		// A selection and tints are left out; the frame is rendered all the same.
		const Entity selection[] = { left };
		const SceneEntityTint tints[] = { SceneEntityTint { left, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) } };
		SceneRenderOptions options;
		options.SelectedEntities = selection;
		options.EntityTints = tints;
		REQUIRE(withoutIDs.Render(scene, camera, nullptr, options));
		CHECK(withoutIDs.GetStats().OutlinedEntities == 0);
		CHECK(withoutIDs.GetStats().TintedEntities == 0);
		ReadbackImage untouched;
		REQUIRE(Renderer::ReadTexture(withoutIDs.GetOutputTexture(), untouched));
		CHECK(MaxDifference(untouched, expected) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Entity tints change exactly the pixels of the listed entities")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::vec3(-0.3f, -1.0f, -0.4f), 2.0f);
		// Lit cubes (so their pixels differ) in front of a lit wall, which is no listed entity.
		Entity left = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(MaterialProperties()), glm::vec3(-1.2f, 0.0f, 0.0f), "Left");
		Entity middle = AddMesh(scene, BuiltinAssets::SphereMesh, assets.AddMaterial(MaterialProperties()), glm::vec3(0.0f, 0.0f, 0.0f), "Middle");
		Entity right = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(MaterialProperties()), glm::vec3(1.2f, 0.0f, 0.0f), "Right");
		Entity wall = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.2f, 0.3f, 0.4f, 1.0f })), glm::vec3(0.0f, 0.0f, -2.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.4f, 0.8f, 4.0f), glm::vec3(0.0f));
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		REQUIRE(ids.BytesPerPixel == sizeof(uint32_t));
		const auto idAt = [&ids](uint32_t x, uint32_t y)
		{
			uint32_t id = 0;
			std::memcpy(&id, ids.Pixels.data() + (static_cast<size_t>(y) * ids.Width + x) * sizeof(uint32_t), sizeof(uint32_t));
			return id;
		};
		const uint32_t leftID = static_cast<uint32_t>(left.GetHandle()) + 1;
		const uint32_t rightID = static_cast<uint32_t>(right.GetHandle()) + 1;

		// Half-transparent on the left cube (listed twice: the first color counts), opaque on the right one, plus an entity of
		// another scene, which is ignored.
		Scene other;
		const glm::vec4 leftColor(1.0f, 0.8f, 0.2f, 0.5f);
		const glm::vec4 rightColor(0.9f, 0.3f, 0.3f, 1.0f);
		const SceneEntityTint tints[] = { { left, leftColor }, { right, rightColor }, { other.CreateEntity("Foreign"), rightColor },
			{ left, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) } };
		SceneRenderOptions options;
		options.EntityTints = tints;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().TintedEntities == 2);
		ReadbackImage tinted;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), tinted));
		ReadbackImage tintedIDs;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), tintedIDs));
		REQUIRE(tintedIDs.Pixels == ids.Pixels); // The same frame, the same entities where they were

		// Blending in 8 bits, as the target does: the tint's color and alpha are quantized first.
		const glm::u8vec4 leftPacked = glm::u8vec4(glm::round(glm::clamp(leftColor, 0.0f, 1.0f) * 255.0f));
		const float leftAlpha = static_cast<float>(leftPacked.a) / 255.0f;
		const glm::u8vec4 rightPacked = glm::u8vec4(glm::round(rightColor * 255.0f));
		uint32_t leftPixels = 0;
		uint32_t rightPixels = 0;
		uint32_t otherPixels = 0;
		uint32_t changedLeftPixels = 0;
		uint32_t mismatches = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const uint32_t id = idAt(x, y);
				const glm::u8vec4 before = GetPixelRGBA8(plain, x, y);
				const glm::u8vec4 after = GetPixelRGBA8(tinted, x, y);
				if (id == leftID)
				{
					leftPixels++;
					changedLeftPixels += after != before ? 1u : 0u;
					for (int channel = 0; channel < 3; channel++)
					{
						const float expected = static_cast<float>(leftPacked[channel]) * leftAlpha + static_cast<float>(before[channel]) * (1.0f - leftAlpha);
						mismatches += std::abs(static_cast<float>(after[channel]) - expected) > 1.0f ? 1u : 0u;
					}
				}
				else if (id == rightID)
				{
					rightPixels++;
					mismatches += glm::u8vec3(after) != glm::u8vec3(rightPacked) ? 1u : 0u;
				}
				else
				{
					// Every other pixel - the sphere, the wall, the background - is exactly as before.
					otherPixels++;
					mismatches += after != before ? 1u : 0u;
				}
			}
		}
		CHECK(leftPixels > 100);
		CHECK(rightPixels > 100);
		CHECK(otherPixels > 100);
		CHECK(changedLeftPixels == leftPixels);
		CHECK(mismatches == 0);
		CHECK(idAt(c_Size / 2, c_Size / 2) == static_cast<uint32_t>(middle.GetHandle()) + 1);

		// Without tints (or only foreign ones) the image is the plain one.
		const SceneEntityTint foreign[] = { { other.CreateEntity("Foreign 2"), rightColor } };
		options.EntityTints = foreign;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().TintedEntities == 0);
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), tinted));
		CHECK(MaxDifference(tinted, plain) == 0);
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

	TEST_CASE("Debug lines reaching behind the camera are clipped where they leave the view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const glm::u8vec4 green(0, 255, 0, 255);
		DebugDraw debugDraw;
		SceneRenderOptions options;
		options.DebugShapes = &debugDraw;
		auto render = [&](const SceneCamera& camera)
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			return image;
		};

		// Perspective: from 10 units ahead to 10 units behind the camera. The visible part runs from the far end to the
		// edge of the image.
		const SceneCamera perspective = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		debugDraw.Line(glm::vec3(0.5f, -0.3f, -10.0f), glm::vec3(0.5f, -0.3f, 10.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
		ReadbackImage image = render(perspective);
		for (float z : { -9.0f, -5.0f, -2.0f, -1.0f })
		{
			CAPTURE(z);
			CHECK(HasColorNear(image, ProjectToPixel(perspective, glm::vec3(0.5f, -0.3f, z), c_Size), green));
		}

		// Orthographic: the part beyond the near plane (behind the camera) is cut off, the rest is drawn.
		debugDraw.Clear();
		debugDraw.Line(glm::vec3(-2.0f, 0.5f, -5.0f), glm::vec3(2.0f, 0.5f, 15.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
		const SceneCamera orthographic = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), 6.0f);
		image = render(orthographic);
		auto pointAt = [](float z) { return glm::vec3(-2.0f + 4.0f * (z + 5.0f) / 20.0f, 0.5f, z); };
		for (float z : { -4.0f, 0.0f, 4.0f })
		{
			CAPTURE(z);
			CHECK(HasColorNear(image, ProjectToPixel(orthographic, pointAt(z), c_Size), green));
		}
		for (float z : { 7.0f, 10.0f, 14.0f })
		{
			CAPTURE(z);
			CHECK_FALSE(HasColorNear(image, ProjectToPixel(orthographic, pointAt(z), c_Size), green));
		}
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
