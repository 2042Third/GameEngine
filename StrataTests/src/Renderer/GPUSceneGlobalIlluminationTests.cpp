#include <doctest/doctest.h>

#include "Renderer/GIScenes.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/VoxelClipmap.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Width = 1280;
	constexpr uint32_t c_Height = 720;
	constexpr float c_Aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);

	// A renderer of the voxels view whose clipmap voxelizes everything a frame lists (no budget). The GI's work does not follow the
	// viewport: comparisons of voxels render small.
	Scope<SceneRenderer> CreateVoxelRenderer(SceneRendererSpecification specification = ExactSpecification(), uint32_t width = c_Width, uint32_t height = c_Height)
	{
		specification.GlobalIllumination.VoxelizeTrianglesPerFrame = 0;
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 0;
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(width, height);
		return renderer;
	}

	SceneRenderOptions VoxelView()
	{
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::Voxels;
		return options;
	}

	// Renders until the GI has nothing left to do: every proxy resident with its mesh, nothing queued (at most 100 frames).
	void RenderUntilSettled(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options = VoxelView())
	{
		for (uint32_t frame = 0; frame < 100; frame++)
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			const SceneRendererStats& stats = renderer.GetStats();
			REQUIRE(stats.GlobalIllumination);
			if (stats.GlobalIlluminationSettled && stats.GIWaitingProxies == 0 && stats.PendingAssets == 0)
				return;
		}
		FAIL("the GI did not settle within 100 frames");
	}

	// A voxel level as the GPU holds it.
	struct VoxelLevel
	{
		glm::uvec3 Dimensions { 0 };
		std::vector<uint8_t> Material; // RGBA8: linear albedo, occupancy
		std::vector<uint8_t> Emission; // R32_UINT: RGB9E5
		std::vector<uint8_t> Normal;   // RGBA8_SNORM

		size_t GetIndex(const glm::i64vec3& voxel) const
		{
			const glm::uvec3 texel = GlobalIllumination::GetTexel(voxel, Dimensions);
			return static_cast<size_t>(texel.x) + Dimensions.x * (static_cast<size_t>(texel.y) + static_cast<size_t>(Dimensions.y) * texel.z);
		}
		glm::u8vec4 GetMaterial(const glm::i64vec3& voxel) const
		{
			const size_t index = GetIndex(voxel) * 4;
			return glm::u8vec4(Material[index], Material[index + 1], Material[index + 2], Material[index + 3]);
		}
		glm::vec4 GetNormal(const glm::i64vec3& voxel) const
		{
			const size_t index = GetIndex(voxel) * 4;
			glm::vec4 normal;
			for (int channel = 0; channel < 4; channel++)
				normal[channel] = std::max(static_cast<float>(static_cast<int8_t>(Normal[index + channel])) / 127.0f, -1.0f);
			return normal;
		}
		uint32_t GetEmission(const glm::i64vec3& voxel) const
		{
			uint32_t value = 0;
			std::memcpy(&value, Emission.data() + GetIndex(voxel) * 4, sizeof(value));
			return value;
		}
		bool operator==(const VoxelLevel& other) const = default;
	};

	VoxelLevel ReadLevel(const SceneRenderer& renderer, uint32_t level)
	{
		VoxelLevel data;
		const GlobalIlluminationLayout* layout = renderer.GetGlobalIlluminationLayout();
		REQUIRE(layout);
		REQUIRE(level < layout->Levels.size());
		data.Dimensions = layout->Levels[level].Dimensions;
		REQUIRE(renderer.ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Material, data.Material));
		REQUIRE(renderer.ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Emission, data.Emission));
		REQUIRE(renderer.ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Normal, data.Normal));
		return data;
	}

	std::vector<VoxelLevel> ReadLevels(const SceneRenderer& renderer)
	{
		std::vector<VoxelLevel> levels;
		const GlobalIlluminationLayout* layout = renderer.GetGlobalIlluminationLayout();
		REQUIRE(layout);
		for (uint32_t level = 0; level < layout->Levels.size(); level++)
			levels.push_back(ReadLevel(renderer, level));
		return levels;
	}

	// The voxels of a renderer made for the comparison: it renders the path's first camera, then `camera` (its anchors are those of
	// the first, as the compared renderer's), each until settled.
	std::vector<VoxelLevel> ReadFreshVoxels(Scene& scene, const SceneCamera& first, const SceneCamera& camera)
	{
		Scope<SceneRenderer> fresh = CreateVoxelRenderer(ExactSpecification(), 64, 64);
		RenderUntilSettled(*fresh, scene, first);
		RenderUntilSettled(*fresh, scene, camera);
		return ReadLevels(*fresh);
	}

	// How many voxels of two renderers' levels differ (the textures' bytes).
	size_t CountDifferences(const std::vector<VoxelLevel>& a, const std::vector<VoxelLevel>& b)
	{
		REQUIRE(a.size() == b.size());
		size_t differences = 0;
		for (size_t level = 0; level < a.size(); level++)
		{
			REQUIRE(a[level].Material.size() == b[level].Material.size());
			for (size_t index = 0; index < a[level].Material.size(); index += 4)
			{
				differences += std::memcmp(a[level].Material.data() + index, b[level].Material.data() + index, 4) != 0
					|| std::memcmp(a[level].Emission.data() + index, b[level].Emission.data() + index, 4) != 0
					|| std::memcmp(a[level].Normal.data() + index, b[level].Normal.data() + index, 4) != 0 ? 1 : 0;
			}
		}
		return differences;
	}

	SceneCamera YardCamera(const glm::vec3& offset, float aspect = 1.0f)
	{
		return LookAt(glm::vec3(-3.5f, 1.7f, -9.5f) + offset, glm::vec3(2.5f, 1.0f, -0.5f) + offset, aspect);
	}

	bool Contains(const std::vector<glm::i64vec3>& sorted, const glm::i64vec3& voxel)
	{
		return std::binary_search(sorted.begin(), sorted.end(), voxel,
			[](const glm::i64vec3& a, const glm::i64vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
	}

}

TEST_SUITE("GPU.SceneRenderer.GlobalIllumination")
{
	TEST_CASE("A box voxelizes to its cells")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const glm::vec3 red(0.62f, 0.09f, 0.07f);
		const uint32_t packedRed = GlobalIllumination::PackAlbedo(red);
		// On the grid and off it, scaled, and in Low's layout (20 cm voxels).
		struct Placement
		{
			float Offset;
			glm::vec3 Scale;
			GlobalIlluminationLevel Level;
		};
		const Placement placements[] = { { 0.0f, glm::vec3(1.0f), GlobalIlluminationLevel::High }, { 0.037f, glm::vec3(1.0f), GlobalIlluminationLevel::High },
			{ 0.0f, glm::vec3(4.0f, 1.05f, 1.8f), GlobalIlluminationLevel::High }, { 0.0f, glm::vec3(4.0f, 1.05f, 1.8f), GlobalIlluminationLevel::Low } };
		for (const Placement& placement : placements)
		{
			const float offset = placement.Offset;
			CAPTURE(offset);
			CAPTURE(placement.Scale.x);
			CAPTURE(static_cast<int>(placement.Level));
			SceneTestAssets assets;
			Scene scene;
			AddNeutralPostProcess(scene);
			const Ref<Mesh> cube = MeshFactory::CreateCube();
			const AssetHandle mesh = assets.Add(cube, "Cube");
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(red, 1.0f);
			const glm::vec3 position = glm::vec3(1.0f, 0.5f, 2.0f) + offset;
			AddMesh(scene, mesh, assets.AddMaterial(properties), position, "Box").GetComponent<TransformComponent>().Scale = placement.Scale;
			SceneRendererSpecification specification = ExactSpecification();
			specification.GlobalIllumination.Level = placement.Level;
			Scope<SceneRenderer> renderer = CreateVoxelRenderer(specification);
			RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(-1.0f, 1.5f, -1.0f), position, c_Aspect));
			const VoxelClipmap* clipmap = renderer->GetVoxelClipmap();
			REQUIRE(clipmap);
			CHECK(clipmap->GetLayout().Levels.size() == (placement.Level == GlobalIlluminationLevel::High ? 4u : 3u));
			const glm::mat4 world = glm::scale(glm::translate(glm::mat4(1.0f), position), placement.Scale);
			for (uint32_t level = 0; level < 2; level++)
			{
				CAPTURE(level);
				const float voxelSize = clipmap->GetLayout().Levels[level].VoxelSize;
				const std::vector<glm::i64vec3> surface = ReferenceVoxels(*cube, world, clipmap->GetLevel(level).Anchor, voxelSize);
				REQUIRE_FALSE(surface.empty());
				const VoxelLevel voxels = ReadLevel(*renderer, level);
				const glm::i64vec3 low = GlobalIllumination::GetVoxelIndex(glm::dvec3(position - 0.5f * placement.Scale), voxelSize) - int64_t(3);
				const glm::i64vec3 high = GlobalIllumination::GetVoxelIndex(glm::dvec3(position + 0.5f * placement.Scale), voxelSize) + int64_t(3);
				uint32_t wrongOccupancy = 0;
				uint32_t wrongSurface = 0;
				uint32_t wrongInterior = 0;
				uint32_t surfaceCount = 0;
				uint32_t interiorCount = 0;
				for (int64_t z = low.z; z <= high.z; z++)
				{
					for (int64_t y = low.y; y <= high.y; y++)
					{
						for (int64_t x = low.x; x <= high.x; x++)
						{
							const glm::i64vec3 voxel(x, y, z);
							const glm::dvec3 center = (glm::dvec3(voxel) + 0.5) * static_cast<double>(voxelSize) - glm::dvec3(position);
							const bool inside = glm::all(glm::lessThan(glm::abs(center), 0.5 * glm::dvec3(placement.Scale)));
							const bool onSurface = Contains(surface, voxel);
							const glm::u8vec4 material = voxels.GetMaterial(voxel);
							const glm::vec4 normal = voxels.GetNormal(voxel);
							if ((material.a == 255) != (onSurface || inside) || (material.a != 0 && material.a != 255))
								wrongOccupancy++;
							if (onSurface)
							{
								surfaceCount++;
								const bool albedo = std::abs(int(material.r) - int(packedRed & 255u)) <= 1 && std::abs(int(material.g) - int((packedRed >> 8) & 255u)) <= 1
									&& std::abs(int(material.b) - int((packedRed >> 16) & 255u)) <= 1;
								if (!albedo || normal.w != 1.0f)
									wrongSurface++;
							}
							else if (inside)
							{
								interiorCount++;
								if (material.r != 0 || material.g != 0 || material.b != 0 || normal != glm::vec4(0.0f))
									wrongInterior++;
							}
						}
					}
				}
				MESSAGE("level ", level, ": ", surfaceCount, " surface voxels, ", interiorCount, " inside");
				CHECK(wrongOccupancy == 0);
				CHECK(wrongSurface == 0);
				CHECK(wrongInterior == 0);
				CHECK(interiorCount > 0);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A plane is open and faces up, and thin walls are two-sided")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle material = assets.AddMaterial(white);
		// A 4 m plane at a height off the voxel grid, and a 5 cm closed slab (thinner than a level-0 voxel).
		const AssetHandle plane = assets.Add(MeshFactory::CreatePlane(4.0f), "Plane");
		AddMesh(scene, plane, material, glm::vec3(0.0f, 0.37f, 2.0f), "Plane");
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		AddMesh(scene, cube, material, glm::vec3(-2.05f, 1.0f, 2.0f), "Slab").GetComponent<TransformComponent>().Scale = glm::vec3(0.05f, 1.0f, 1.0f);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(0.0f, 1.5f, -2.0f), glm::vec3(0.0f, 0.4f, 2.0f), c_Aspect));
		const VoxelLevel level0 = ReadLevel(*renderer, 0);
		// The plane: one layer of voxels (y 0.3-0.4), normals up within a degree, nothing below or above filled.
		uint32_t layerVoxels = 0;
		uint32_t wrong = 0;
		for (int64_t z = 5; z < 35; z++)
		{
			for (int64_t x = -15; x < 15; x++)
			{
				for (int64_t y = -2; y < 8; y++)
				{
					const glm::u8vec4 occupancy = level0.GetMaterial(glm::i64vec3(x, y, z));
					if (y == 3)
					{
						const glm::vec4 normal = level0.GetNormal(glm::i64vec3(x, y, z));
						layerVoxels += occupancy.a == 255 ? 1 : 0;
						wrong += occupancy.a != 255 || normal.w != 1.0f || normal.y < std::cos(glm::radians(1.0f)) ? 1 : 0;
					}
					else
					{
						wrong += occupancy.a != 0 ? 1 : 0;
					}
				}
			}
		}
		CHECK(layerVoxels == 30 * 30);
		CHECK(wrong == 0);
		// The slab: its voxels hold both of its faces, so their normals cancel: two-sided.
		uint32_t twoSided = 0;
		for (int64_t y = 6; y < 14; y++)
		{
			for (int64_t z = 16; z < 24; z++)
				twoSided += level0.GetNormal(glm::i64vec3(-21, y, z)).w == -1.0f ? 1 : 0;
		}
		CHECK(twoSided == 8 * 8);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Normals face out")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		// A 2 m cube from (0, 0, 2) to (2, 2, 4), on the grid.
		AddMesh(scene, cube, assets.AddMaterial(white), glm::vec3(1.0f, 1.0f, 3.0f), "Box").GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(-1.0f, 1.5f, -1.0f), glm::vec3(1.0f, 1.0f, 3.0f), c_Aspect));
		const VoxelLevel level0 = ReadLevel(*renderer, 0);
		// The face voxels away from the edges (3 voxels in), on both sides of each face: within 10 degrees of the face's normal.
		const float limit = std::cos(glm::radians(10.0f));
		uint32_t checked = 0;
		uint32_t wrong = 0;
		for (int64_t u = 3; u < 17; u++)
		{
			for (int64_t v = 3; v < 17; v++)
			{
				const std::pair<glm::i64vec3, glm::vec3> faces[] = { { { -1, u, 20 + v }, { -1, 0, 0 } }, { { 0, u, 20 + v }, { -1, 0, 0 } },
					{ { 19, u, 20 + v }, { 1, 0, 0 } }, { { 20, u, 20 + v }, { 1, 0, 0 } }, { { u, -1, 20 + v }, { 0, -1, 0 } }, { { u, 20, 20 + v }, { 0, 1, 0 } },
					{ { u, v, 19 }, { 0, 0, -1 } }, { { u, v, 40 }, { 0, 0, 1 } } };
				for (const auto& [voxel, expected] : faces)
				{
					const glm::vec4 normal = level0.GetNormal(voxel);
					checked++;
					wrong += normal.w != 1.0f || glm::dot(glm::normalize(glm::vec3(normal)), expected) < limit ? 1 : 0;
				}
			}
		}
		CHECK(checked > 1000);
		CHECK(wrong == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A textured material's albedo is its map's mean")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// Four colors on a white factor: the voxels take their linear mean.
		const std::vector<uint8_t> texels = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
		MaterialProperties textured;
		textured.BaseColorMap = assets.AddTexture(2, 2, texels, TextureFormat::RGBA8SRGB);
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		AddMesh(scene, cube, assets.AddMaterial(textured), glm::vec3(1.0f, 0.5f, 2.0f), "Box");
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(-1.0f, 1.5f, -1.0f), glm::vec3(1.0f, 0.5f, 2.0f), c_Aspect));
		const VoxelLevel level0 = ReadLevel(*renderer, 0);
		const uint32_t expected = GlobalIllumination::PackAlbedo(glm::vec3(0.5f));
		const glm::u8vec4 albedo = level0.GetMaterial(glm::i64vec3(10, 2, 15)); // On the box's front face
		CHECK(albedo.a == 255);
		CHECK(std::abs(int(albedo.r) - int(expected & 255u)) <= 1);
		CHECK(std::abs(int(albedo.g) - int((expected >> 8) & 255u)) <= 1);
		CHECK(std::abs(int(albedo.b) - int((expected >> 16) & 255u)) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Glow covers its area")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// An 18 cm glowing square in level 1's 20 cm voxels, at places on and across the grid: the emission of its voxels adds up to
		// its emission times its area in voxel faces (0.81) within 20%, however it lies on the grid.
		for (const glm::vec3& position : { glm::vec3(1.1f, 1.1f, 2.05f), glm::vec3(1.2f, 1.13f, 2.05f), glm::vec3(1.2f, 1.2f, 2.05f) })
		{
			CAPTURE(position.x);
			CAPTURE(position.y);
			SceneTestAssets assets;
			Scene scene;
			AddNeutralPostProcess(scene);
			const AssetHandle quad = assets.Add(MeshFactory::CreateQuad(0.18f), "Quad");
			AddMesh(scene, quad, assets.AddMaterial(Emitting(100.0f)), position, "Glow");
			Scope<SceneRenderer> renderer = CreateVoxelRenderer();
			RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(1.0f, 1.0f, -1.0f), position, c_Aspect));
			const VoxelLevel level1 = ReadLevel(*renderer, 1);
			glm::vec3 sum(0.0f);
			for (int64_t y = 2; y < 9; y++)
			{
				for (int64_t x = 2; x < 9; x++)
					sum += GlobalIllumination::UnpackRGB9E5(level1.GetEmission(glm::i64vec3(x, y, 10)));
			}
			const float expected = 100.0f * (0.18f * 0.18f) / (0.2f * 0.2f);
			CHECK(sum.g == doctest::Approx(expected).epsilon(0.2));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A 220 m triangle costs its area")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		const Ref<Mesh> quad = MeshFactory::CreatePlane(220.0f);
		REQUIRE(quad->GetTriangleCount() == 2);
		MaterialProperties ground;
		AddMesh(scene, assets.Add(quad, "Road"), assets.AddMaterial(ground), glm::vec3(3.0f, 0.37f, -5.0f), "Road");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.7f, 0.0f), glm::vec3(0.0f, 0.4f, 10.0f), c_Aspect);
		// The mesh is uploaded and its proxy resident before the frame that voxelizes it all (the GI's counters of that frame).
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer->GetStats().PendingAssets > 0 || renderer->GetStats().DeferredProxies > 0); frame++)
			REQUIRE(renderer->Render(scene, camera, nullptr, SceneRenderOptions {}));
		REQUIRE(renderer->Render(scene, camera, nullptr, VoxelView()));
		CHECK(renderer->GetStats().GlobalIlluminationSettled);
		renderer->ResolveGPUStats(true);
		const SceneRendererGPUStats& counts = renderer->GetGPUStats();
		REQUIRE(counts.HasGICounters);
		REQUIRE(counts.GICountersRender == renderer->GetRenderCount());
		// The CPU's count of the tiles the voxelizer lists: per level, per region of its window, the 8^3 tiles of each triangle's
		// bounds within the region that the triangle overlaps (both triangles are large in every region).
		const VoxelClipmap& clipmap = *renderer->GetVoxelClipmap();
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.37f, -5.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));
		const RenderData::AffineTransform rows = RenderData::AffineTransform::FromMatrix(world);
		uint64_t expected = 0;
		for (uint32_t level = 0; level < clipmap.GetLayout().Levels.size(); level++)
		{
			const VoxelLevelLayout& layout = clipmap.GetLayout().Levels[level];
			const glm::i64vec3 anchor = clipmap.GetLevel(level).Anchor;
			const glm::vec3 anchorPosition = glm::vec3(glm::dvec3(anchor) * static_cast<double>(layout.VoxelSize));
			std::vector<VoxelBox> regions;
			GlobalIllumination::SplitRegion(GlobalIllumination::GetWindow(layout, clipmap.GetLevel(level).Window), layout.Dimensions, regions);
			const std::vector<glm::vec3>& positions = quad->GetPositions();
			const std::vector<uint32_t>& indices = quad->GetIndices();
			for (size_t triangle = 0; triangle < indices.size(); triangle += 3)
			{
				glm::vec3 corners[3];
				for (int corner = 0; corner < 3; corner++)
					corners[corner] = GlobalIllumination::GetVoxelCoordinate(rows, positions[indices[triangle + corner]], anchorPosition, layout.VoxelSize);
				const glm::ivec3 first = glm::ivec3(glm::ceil(glm::min(glm::min(corners[0], corners[1]), corners[2]))) - 1;
				const glm::ivec3 last = glm::ivec3(glm::floor(glm::max(glm::max(corners[0], corners[1]), corners[2])));
				for (const VoxelBox& region : regions)
				{
					const glm::ivec3 minimum = glm::ivec3(region.Minimum - anchor);
					const glm::ivec3 maximum = glm::ivec3(region.Maximum - anchor);
					const glm::ivec3 low = glm::max(first, minimum);
					const glm::ivec3 high = glm::min(last, maximum - 1);
					if (glm::any(glm::lessThan(high, low)))
						continue;
					const glm::ivec3 count = high - low + 1;
					REQUIRE(count.x * count.y * count.z > 64);
					const glm::ivec3 firstTile = (low - minimum) / 8;
					const glm::ivec3 lastTile = (high - minimum) / 8;
					for (int z = firstTile.z; z <= lastTile.z; z++)
					{
						for (int y = firstTile.y; y <= lastTile.y; y++)
						{
							for (int x = firstTile.x; x <= lastTile.x; x++)
							{
								const glm::vec3 center = glm::vec3(minimum + glm::ivec3(x, y, z) * 8) + 4.0f;
								expected += TriangleBoxOverlap(center, glm::vec3(4.0f), corners[0], corners[1], corners[2]) ? 1 : 0;
							}
						}
					}
				}
			}
		}
		CHECK(counts.GITiles == expected);
		CHECK(counts.GITileOverflow == 0);
		if (counts.HasTimes)
		{
			for (const GPUStageTime& stage : counts.StageMs)
			{
				if (stage.Stage == RendererGPUStages::Get().RadianceCascades)
					MESSAGE("GPU.RadianceCascades voxelizing the whole clipmap with the 220 m quad: ", stage.Milliseconds, " ms (Debug)");
			}
		}
		// Its voxels are the plane's layer in every level.
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			const VoxelLevel voxels = ReadLevel(*renderer, level);
			const float voxelSize = clipmap.GetLayout().Levels[level].VoxelSize;
			const int64_t layer = GlobalIllumination::GetVoxelIndex(glm::dvec3(0.0, 0.37, 0.0), voxelSize).y;
			const glm::i64vec3 window = clipmap.GetLevel(level).Window;
			uint32_t wrong = 0;
			for (int64_t z = window.z; z < window.z + 128; z += 7)
			{
				for (int64_t x = window.x; x < window.x + 128; x += 7)
				{
					for (int64_t y = window.y; y < window.y + 64; y++)
						wrong += (voxels.GetMaterial(glm::i64vec3(x, y, z)).a == 255) != (y == layer) ? 1 : 0;
				}
			}
			CHECK(wrong == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Scrolling keeps the voxels")
	{
		// The yard, a camera moving +x 5 cm a frame for 100 frames, then +y and -z: after every frame every level holds what a
		// renderer made for that camera alone holds, bit for bit.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const auto path = [](uint32_t frame)
		{
			glm::vec3 offset(0.05f * static_cast<float>(std::min(frame, 100u)), 0.0f, 0.0f);
			if (frame > 100)
				offset.y = 0.05f * static_cast<float>(std::min(frame, 120u) - 100);
			if (frame > 120)
				offset.z = -0.05f * static_cast<float>(frame - 120);
			return YardCamera(offset);
		};
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 64, 64);
		RenderUntilSettled(*renderer, scene, path(0));
		size_t differing = 0;
		for (uint32_t frame = 1; frame <= 140; frame++)
		{
			REQUIRE(renderer->Render(scene, path(frame), nullptr, VoxelView()));
			CHECK(renderer->GetStats().GlobalIlluminationSettled);
			const size_t differences = CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, path(0), path(frame)));
			if (differences > 0)
			{
				MESSAGE("frame ", frame, ": ", differences, " voxels differ");
				differing++;
			}
		}
		CHECK(differing == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A moved object leaves no trace")
	{
		// A cube moved 1 m: its old voxels empty in the frame it moved; it is out of the voxels while it moves, and in them again,
		// where it rests, c_GIMovingFrames frames later, as a renderer made then holds it.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		Entity box = AddMesh(scene, cube, assets.AddMaterial(white), glm::vec3(1.0f, 0.5f, 2.0f), "Box");
		const SceneCamera camera = LookAt(glm::vec3(-1.0f, 1.5f, -1.0f), glm::vec3(1.5f, 0.5f, 2.0f), c_Aspect);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 64, 64);
		RenderUntilSettled(*renderer, scene, camera);
		CHECK(ReadLevel(*renderer, 0).GetMaterial(glm::i64vec3(10, 5, 20)).a == 255); // Inside the cube

		box.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 0.5f, 2.0f);
		box.MarkModified<TransformComponent>();
		REQUIRE(renderer->Render(scene, camera, nullptr, VoxelView()));
		CHECK(renderer->GetStats().GIMovingProxies == 1);
		const VoxelLevel moving = ReadLevel(*renderer, 0);
		CHECK(moving.GetMaterial(glm::i64vec3(10, 5, 20)).a == 0);
		CHECK(moving.GetMaterial(glm::i64vec3(20, 5, 20)).a == 0);
		for (uint32_t frame = 1; frame <= RenderData::c_GIMovingFrames; frame++)
			REQUIRE(renderer->Render(scene, camera, nullptr, VoxelView()));
		CHECK(renderer->GetStats().GIMovingProxies == 0);
		CHECK(ReadLevel(*renderer, 0).GetMaterial(glm::i64vec3(20, 5, 20)).a == 255);
		CHECK(CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, camera, camera)) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A jump rebuilds in its frame, and the budget spreads a rebuild")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera home = YardCamera(glm::vec3(0.0f));
		const SceneCamera away = YardCamera(glm::vec3(500.0f, 0.0f, 0.0f));
		{
			// The default budgets: a jump of 500 m and back takes the cut's budget, which holds the yard: complete in its frame.
			Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(ExactSpecification());
			renderer->SetViewportSize(64, 64);
			RenderUntilSettled(*renderer, scene, home);
			REQUIRE(renderer->Render(scene, away, nullptr, VoxelView()));
			CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
			REQUIRE(renderer->Render(scene, home, nullptr, VoxelView()));
			CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
			CHECK(renderer->GetStats().GIQueuedRegions == 0);
			CHECK(CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, home, home)) == 0);
		}
		{
			// A rebuild's budget of 2,000 triangles: pending over several frames, the regions left falling, then the same voxels.
			SceneRendererSpecification specification = ExactSpecification();
			specification.GlobalIllumination.VoxelizeTrianglesOnCut = 2000;
			specification.GlobalIllumination.VoxelizeTrianglesPerFrame = 2000;
			Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
			renderer->SetViewportSize(64, 64);
			// The scene's assets first, so the GI's first frame finds every proxy.
			for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer->GetStats().PendingAssets > 0 || renderer->GetStats().DeferredProxies > 0); frame++)
				REQUIRE(renderer->Render(scene, home, nullptr, SceneRenderOptions {}));
			uint32_t pendingFrames = 0;
			uint32_t queued = UINT32_MAX;
			for (uint32_t frame = 0; frame < 200; frame++)
			{
				REQUIRE(renderer->Render(scene, home, nullptr, VoxelView()));
				const SceneRendererStats& stats = renderer->GetStats();
				CHECK(stats.GIQueuedRegions <= queued);
				queued = stats.GIQueuedRegions;
				if (stats.GlobalIlluminationSettled)
					break;
				pendingFrames += stats.GlobalIlluminationPending ? 1 : 0;
			}
			MESSAGE("a rebuild at 2,000 triangles a frame was pending for ", pendingFrames, " frames");
			CHECK(pendingFrames > 2);
			CHECK(renderer->GetStats().GlobalIlluminationSettled);
			CHECK(CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, home, home)) == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A fast turn, a history reset and waiting proxies keep the voxels")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardCamera(glm::vec3(0.0f));
		{
			// The yard's mesh renderers reach the scene's GPU copy (shared by its renderers) at the default pace first.
			Scope<SceneRenderer> loader = CreateScope<SceneRenderer>(ExactSpecification());
			loader->SetViewportSize(64, 64);
			RenderUntilSettled(*loader, scene, camera);
		}
		// From here on one new mesh renderer reaches it a frame.
		SceneRendererSpecification specification = ExactSpecification();
		specification.NewInstanceBytesPerFrame = 1;
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(64, 64);
		RenderUntilSettled(*renderer, scene, camera);
		// A turn of 90 degrees in one frame (a camera cut for the other passes): no voxel work, not pending.
		SceneCamera turned = LookAt(glm::vec3(-3.5f, 1.7f, -9.5f), glm::vec3(-3.5f, 1.7f, -9.5f) + glm::vec3(1.0f, -0.1f, 0.0f));
		REQUIRE(renderer->Render(scene, turned, nullptr, VoxelView()));
		CHECK(renderer->GetStats().GIVoxelizedTriangles == 0);
		CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
		// A reset of the history (a view switch, a quality change) leaves the voxels alone.
		renderer->ResetHistory();
		REQUIRE(renderer->Render(scene, turned, nullptr, VoxelView()));
		CHECK(renderer->GetStats().GIVoxelizedTriangles == 0);
		CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);

		// Mesh renderers that reach the scene's GPU copy over several frames: waiting, never pending; voxelized as they arrive.
		for (int index = 0; index < 8; index++)
		{
			Entity added = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(1.0f, 0.5f, -6.0f), "Arriving");
			added.GetComponent<TransformComponent>().Scale = glm::vec3(0.3f);
		}
		// The GPU copy's budget for new mesh renderers is a device frame's: each frame here is one.
		bool waited = false;
		for (uint32_t frame = 0; frame < 30; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			REQUIRE(renderer->Render(scene, turned, nullptr, VoxelView()));
			gpu.GetDevice().EndFrame();
			waited = waited || renderer->GetStats().GIWaitingProxies > 0;
			CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
			if (frame > 2 && renderer->GetStats().GIWaitingProxies == 0 && renderer->GetStats().DeferredProxies == 0)
				break;
		}
		CHECK(waited);
		CHECK(renderer->GetStats().GIWaitingProxies == 0);
		RenderUntilSettled(*renderer, scene, turned);
		CHECK(ReadLevel(*renderer, 0).GetMaterial(glm::i64vec3(10, 5, -60)).a == 255);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("20 km out")
	{
		// The yard 20 km from the origin (and its camera): the voxels of the origin's yard, but for the floats' rounding there, which
		// moves the yard's faces up to a millimeter: a face on a voxel boundary marks the voxels on both sides at the origin and one
		// of them 20 km out. At most 5% of occupied voxels differ, each next to (or diagonally beside) an occupied voxel of the other.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const glm::vec3 offset(20000.0f, 0.0f, 20000.0f);
		std::vector<VoxelLevel> levels[2];
		std::vector<glm::i64vec3> windows[2];
		const glm::i64vec3 dimensions(128, 64, 128);
		for (int far = 0; far < 2; far++)
		{
			SceneTestAssets assets;
			Scene scene;
			BuildYard(scene, assets);
			if (far)
			{
				auto view = scene.GetRegistry().view<TransformComponent>();
				for (entt::entity entity : view)
				{
					Entity moved(entity, &scene);
					moved.GetComponent<TransformComponent>().Translation += offset;
					moved.MarkModified<TransformComponent>();
				}
			}
			Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 64, 64);
			RenderUntilSettled(*renderer, scene, YardCamera(far ? offset : glm::vec3(0.0f)));
			levels[far] = ReadLevels(*renderer);
			for (uint32_t level = 0; level < 4; level++)
				windows[far].push_back(renderer->GetVoxelClipmap()->GetLevel(level).Window);
		}
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			const float voxelSize = 0.1f * static_cast<float>(1u << level);
			const glm::i64vec3 shift = glm::i64vec3(glm::round(glm::dvec3(offset) / static_cast<double>(voxelSize)));
			// Where both windows hold the voxel and its neighbors (the far camera's window may lie a voxel off: floats round there).
			const glm::i64vec3 low = glm::max(windows[0][level], windows[1][level] - shift) + int64_t(2);
			const glm::i64vec3 high = glm::min(windows[0][level], windows[1][level] - shift) + dimensions - int64_t(2);
			uint32_t occupied = 0;
			uint32_t differing = 0;
			uint32_t isolated = 0;
			const auto occupiedAt = [&](int which, const glm::i64vec3& voxel) { return levels[which][level].GetMaterial(voxel + (which ? shift : glm::i64vec3(0))).a > 0; };
			for (int64_t z = low.z; z < high.z; z++)
			{
				for (int64_t y = low.y; y < high.y; y++)
				{
					for (int64_t x = low.x; x < high.x; x++)
					{
						const glm::i64vec3 voxel(x, y, z);
						const bool near = occupiedAt(0, voxel);
						const bool far = occupiedAt(1, voxel);
						occupied += near ? 1 : 0;
						if (near == far)
							continue;
						differing++;
						// Next to an occupied voxel of the other.
						bool neighbor = false;
						for (int64_t dz = -1; dz <= 1; dz++)
						{
							for (int64_t dy = -1; dy <= 1; dy++)
							{
								for (int64_t dx = -1; dx <= 1; dx++)
									neighbor = neighbor || occupiedAt(near ? 1 : 0, voxel + glm::i64vec3(dx, dy, dz));
							}
						}
						isolated += neighbor ? 0 : 1;
					}
				}
			}
			MESSAGE("level ", level, ": ", differing, " of ", occupied, " occupied voxels differ");
			CHECK(differing <= occupied / 20);
			CHECK(isolated == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Nothing is made without GI, and turning the views off releases it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardCamera(glm::vec3(0.0f), c_Aspect);
		// A renderer that never shows a GI view: no GI, no stage, nothing in its frame constants.
		Scope<SceneRenderer> plain = CreateScope<SceneRenderer>(ExactSpecification());
		plain->SetViewportSize(c_Width, c_Height);
		for (uint32_t frame = 0; frame < 8; frame++)
			REQUIRE(plain->Render(scene, camera, nullptr, SceneRenderOptions {}));
		CHECK_FALSE(plain->GetStats().GlobalIllumination);
		CHECK(plain->GetStats().GlobalIlluminationSettled);
		CHECK(plain->GetGlobalIlluminationMemoryBytes() == 0);
		CHECK(plain->GetGlobalIlluminationLayout() == nullptr);
		plain->ResolveGPUStats(true);
		for (const GPUStageTime& stage : plain->GetGPUStats().StageMs)
			CHECK(stage.Stage != RendererGPUStages::Get().RadianceCascades);
		ReadbackImage plainImage;
		REQUIRE(Renderer::ReadTexture(plain->GetOutputTexture(), plainImage));

		// One that showed the voxels for 4 frames, then none: released, and its image is the plain one's.
		Scope<SceneRenderer> shown = CreateScope<SceneRenderer>(ExactSpecification());
		shown->SetViewportSize(c_Width, c_Height);
		for (uint32_t frame = 0; frame < 4; frame++)
			REQUIRE(shown->Render(scene, camera, nullptr, VoxelView()));
		CHECK(shown->GetStats().GlobalIllumination);
		CHECK(shown->GetGlobalIlluminationMemoryBytes() > 0);
		for (uint32_t frame = 0; frame < 4; frame++)
			REQUIRE(shown->Render(scene, camera, nullptr, SceneRenderOptions {}));
		CHECK_FALSE(shown->GetStats().GlobalIllumination);
		CHECK(shown->GetGlobalIlluminationMemoryBytes() == 0);
		ReadbackImage shownImage;
		REQUIRE(Renderer::ReadTexture(shown->GetOutputTexture(), shownImage));
		CHECK(shownImage.Pixels == plainImage.Pixels);
		// Shown again: made again, from the start.
		RenderUntilSettled(*shown, scene, camera);
		CHECK(shown->GetGlobalIlluminationMemoryBytes() > 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.GPUDriven")
{
	TEST_CASE("The GI's voxels")
	{
		// The voxels read the render scene's GPU copy and the geometry pool, never culling: the CPU and GPU paths (occlusion
		// culling on) give the same voxels.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets, YardOptions { .GrassCards = 100 });
		std::vector<VoxelLevel> levels[2];
		for (int path = 0; path < 2; path++)
		{
			SceneRendererSpecification specification = ExactSpecification();
			specification.GPUDriven = path == 0 ? GPUDrivenMode::Off : GPUDrivenMode::Auto;
			specification.OcclusionCulling = true;
			Scope<SceneRenderer> renderer = CreateVoxelRenderer(specification, 256, 256);
			for (uint32_t frame = 0; frame < 4; frame++)
				REQUIRE(renderer->Render(scene, YardCamera(glm::vec3(0.0f)), nullptr, VoxelView()));
			RenderUntilSettled(*renderer, scene, YardCamera(glm::vec3(0.0f)));
			CHECK(renderer->GetStats().GPUDriven == (path == 1));
			levels[path] = ReadLevels(*renderer);
		}
		CHECK(CountDifferences(levels[0], levels[1]) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
