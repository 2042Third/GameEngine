#include <doctest/doctest.h>

#include "Renderer/GIScenes.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/DeferredReleaseQueue.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/VoxelClipmap.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
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
			REQUIRE(RenderGIFrame(renderer, scene, camera, options));
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
		std::vector<VoxelLevel> levels = ReadLevels(*fresh); // Waits for the GPU
		// The renderer's GI resources (116 MiB at High) leave through the deferred release queue, which nothing pumps in a test:
		// free them now, or a path of fresh renderers exhausts the GPU's memory.
		fresh.reset();
		Renderer::GetDeferredReleases().Collect();
		return levels;
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

	// A seeded walk around the yard of `steps` steps after its first camera: it turns at random and mixes moves within a voxel,
	// moves of several scroll steps on all three axes at once (slabs of every face together), moves back, and jumps of 14 to 30 m
	// on one axis (past level 0's window, 12.8 m wide, and at times level 1's, 25.6 m), at least one. It takes its numbers straight
	// from mt19937's output, which the standard fixes (its distributions it does not), so every platform walks the same path. It
	// stays within 60 m of the yard, well within a level's anchor distance (1 km), where voxels do not depend on the camera.
	std::vector<SceneCamera> RandomYardPath(uint32_t seed, uint32_t steps)
	{
		std::mt19937 random(seed);
		// Every number is drawn in its own statement: the order a call's arguments are evaluated in differs between compilers.
		const auto uniform = [&random](float low, float high)
		{
			return low + (high - low) * static_cast<float>(static_cast<double>(random()) / 4294967296.0);
		};
		const auto uniformVector = [&uniform](const glm::vec3& extent)
		{
			glm::vec3 value;
			for (int axis = 0; axis < 3; axis++)
				value[axis] = uniform(-extent[axis], extent[axis]);
			return value;
		};
		const auto cameraAt = [](const glm::vec3& offset, float yaw)
		{
			const glm::vec3 position = glm::vec3(-3.5f, 1.7f, -9.5f) + offset;
			return LookAt(position, position + glm::vec3(glm::sin(yaw), -0.15f, glm::cos(yaw)));
		};
		std::vector<SceneCamera> path = { cameraAt(glm::vec3(0.0f), 0.0f) };
		glm::vec3 offset(0.0f);
		uint32_t jumps = 0;
		for (uint32_t step = 1; step <= steps; step++)
		{
			const uint32_t kind = random() % 8;
			// The last step jumps if none did.
			if (kind == 7 || (step == steps && jumps == 0))
			{
				const uint32_t axis = random() % 3;
				const float direction = random() % 2 == 0 ? 1.0f : -1.0f;
				const float distance = uniform(14.0f, 30.0f);
				offset[axis] += direction * distance;
				jumps++;
			}
			else
			{
				offset += uniformVector(kind < 4 ? glm::vec3(0.35f) : glm::vec3(4.0f, 2.0f, 4.0f));
			}
			offset = glm::clamp(offset, glm::vec3(-60.0f, -3.0f, -60.0f), glm::vec3(60.0f, 30.0f, 60.0f));
			const float yaw = uniform(-glm::pi<float>(), glm::pi<float>());
			path.push_back(cameraAt(offset, yaw));
		}
		MESSAGE("the walk (seed ", seed, ") jumps ", jumps, " times");
		return path;
	}

	bool Contains(const std::vector<glm::i64vec3>& sorted, const glm::i64vec3& voxel)
	{
		return std::binary_search(sorted.begin(), sorted.end(), voxel,
			[](const glm::i64vec3& a, const glm::i64vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
	}

	// The CPU's count of the tiles the voxelizer lists for a mesh when it voxelizes every level's whole window: per level, per
	// region of its window, the 8^3 tiles of each large triangle's bounds within the region that the triangle overlaps. A triangle is
	// large when its whole bounds (not the region's part) hold more than 64 voxels.
	uint64_t CountListedTiles(const Mesh& mesh, const glm::mat4& world, const VoxelClipmap& clipmap)
	{
		const RenderData::AffineTransform rows = RenderData::AffineTransform::FromMatrix(world);
		const std::vector<glm::vec3>& positions = mesh.GetPositions();
		const std::vector<uint32_t>& indices = mesh.GetIndices();
		uint64_t tiles = 0;
		for (uint32_t level = 0; level < clipmap.GetLayout().Levels.size(); level++)
		{
			const VoxelLevelLayout& layout = clipmap.GetLayout().Levels[level];
			const glm::i64vec3 anchor = clipmap.GetLevel(level).Anchor;
			const glm::vec3 anchorPosition = glm::vec3(glm::dvec3(anchor) * static_cast<double>(layout.VoxelSize));
			std::vector<VoxelBox> regions;
			GlobalIllumination::SplitRegion(GlobalIllumination::GetWindow(layout, clipmap.GetLevel(level).Window), layout.Dimensions, regions);
			for (size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
			{
				glm::vec3 corners[3];
				for (int corner = 0; corner < 3; corner++)
					corners[corner] = GlobalIllumination::GetVoxelCoordinate(rows, positions[indices[triangle + corner]], anchorPosition, layout.VoxelSize);
				const glm::ivec3 first = glm::ivec3(glm::ceil(glm::min(glm::min(corners[0], corners[1]), corners[2]))) - 1;
				const glm::ivec3 last = glm::ivec3(glm::floor(glm::max(glm::max(corners[0], corners[1]), corners[2])));
				const glm::i64vec3 fullCount = glm::i64vec3(last - first) + int64_t(1);
				if (fullCount.x * fullCount.y * fullCount.z <= 64)
					continue;
				for (const VoxelBox& region : regions)
				{
					const glm::ivec3 minimum = glm::ivec3(region.Minimum - anchor);
					const glm::ivec3 maximum = glm::ivec3(region.Maximum - anchor);
					const glm::ivec3 low = glm::max(first, minimum);
					const glm::ivec3 high = glm::min(last, maximum - 1);
					if (glm::any(glm::lessThan(high, low)))
						continue;
					const glm::ivec3 firstTile = (low - minimum) / 8;
					const glm::ivec3 lastTile = (high - minimum) / 8;
					for (int z = firstTile.z; z <= lastTile.z; z++)
					{
						for (int y = firstTile.y; y <= lastTile.y; y++)
						{
							for (int x = firstTile.x; x <= lastTile.x; x++)
							{
								const glm::vec3 center = glm::vec3(minimum + glm::ivec3(x, y, z) * 8) + 4.0f;
								tiles += TriangleBoxOverlap(center, glm::vec3(4.0f), corners[0], corners[1], corners[2]) ? 1 : 0;
							}
						}
					}
				}
			}
		}
		return tiles;
	}

	// Renders until the scene's mesh is uploaded and its proxy resident, then the frame that voxelizes it all (no budget) and
	// reads that frame's GI counters.
	SceneRendererGPUStats VoxelizeAllAtOnce(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer.GetStats().PendingAssets > 0 || renderer.GetStats().DeferredProxies > 0); frame++)
			REQUIRE(RenderGIFrame(renderer, scene, camera, SceneRenderOptions {}));
		REQUIRE(RenderGIFrame(renderer, scene, camera, VoxelView()));
		CHECK(renderer.GetStats().GlobalIlluminationSettled);
		renderer.ResolveGPUStats(true);
		const SceneRendererGPUStats& counts = renderer.GetGPUStats();
		REQUIRE(counts.HasGICounters);
		REQUIRE(counts.GICountersRender == renderer.GetRenderCount());
		return counts;
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
		// c_GITouchOccupancy as the RGBA8 texture stores it.
		constexpr uint8_t c_Touch = static_cast<uint8_t>(RenderData::c_GITouchOccupancy * 255.0f + 0.5f);
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
				// The surface voxels, and those some face faces: a face on the grid has none behind it (they are inside the box).
				std::vector<glm::i64vec3> facing;
				std::vector<glm::i64vec3> solid;
				const std::vector<glm::i64vec3> surface = ReferenceVoxels(*cube, world, clipmap->GetLevel(level).Anchor, voxelSize, &facing, &solid);
				REQUIRE_FALSE(surface.empty());
				const VoxelLevel voxels = ReadLevel(*renderer, level);
				const glm::i64vec3 low = GlobalIllumination::GetVoxelIndex(glm::dvec3(position - 0.5f * placement.Scale), voxelSize) - int64_t(3);
				const glm::i64vec3 high = GlobalIllumination::GetVoxelIndex(glm::dvec3(position + 0.5f * placement.Scale), voxelSize) + int64_t(3);
				uint32_t wrongOccupancy = 0;
				uint32_t wrongSurface = 0;
				uint32_t wrongInterior = 0;
				uint32_t surfaceCount = 0;
				uint32_t interiorCount = 0;
				uint32_t behindCount = 0;
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
							// Opaque where a face reaches in or the box holds it; the layer only touched by faces in front of it at
							// c_GITouchOccupancy.
							const uint8_t expected = inside || Contains(solid, voxel) ? 255 : onSurface ? c_Touch : 0;
							if (material.a != expected)
								wrongOccupancy++;
							if (onSurface)
							{
								surfaceCount++;
								const bool albedo = std::abs(int(material.r) - int(packedRed & 255u)) <= 1 && std::abs(int(material.g) - int((packedRed >> 8) & 255u)) <= 1
									&& std::abs(int(material.b) - int((packedRed >> 16) & 255u)) <= 1;
								const bool faced = Contains(facing, voxel);
								behindCount += faced ? 0 : 1;
								if (!albedo || (faced ? normal.w != 1.0f : normal != glm::vec4(0.0f)))
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
				MESSAGE("level ", level, ": ", surfaceCount, " surface voxels (", behindCount, " behind every face touching them), ", interiorCount, " inside");
				// Off the grid no face touches a voxel from behind; on it, the layer inside each face is behind it.
				if (placement.Offset != 0.0f)
					CHECK(behindCount == 0);
				else if (placement.Scale == glm::vec3(1.0f))
					CHECK(behindCount > 0);
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

	TEST_CASE("A box resting on a floor leaves the floor at its foot one-sided")
	{
		// A closed 1 m box on a closed floor, both on level 0's grid (anchored at the origin: the camera's window starts at positive
		// voxels, so the coordinates are exact). The box's bottom lies on the floor's top, facing it, and touches the floor's
		// voxels around its foot only along their edges: those voxels hold the floor's top and the box's side, facing up and
		// out, and must not go two-sided.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle material = assets.AddMaterial(white);
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		AddMesh(scene, cube, material, glm::vec3(10.5f, 3.75f, 12.5f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(8.0f, 0.5f, 8.0f);
		AddMesh(scene, cube, material, glm::vec3(10.5f, 4.5f, 12.5f), "Box");
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(8.0f, 5.5f, 8.0f), glm::vec3(10.5f, 4.0f, 12.5f), c_Aspect));
		REQUIRE(renderer->GetVoxelClipmap()->GetLevel(0).Anchor == glm::i64vec3(0));
		const VoxelLevel level0 = ReadLevel(*renderer, 0);
		// The box covers voxels x 100-109 and z 120-129; the floor's top is at y 40. The ring of floor voxels around the box's foot,
		// corners included, and a patch of open floor.
		std::vector<glm::i64vec3> ring;
		for (int64_t step = 99; step <= 110; step++)
		{
			ring.push_back({ step, 40, 119 });
			ring.push_back({ step, 40, 130 });
		}
		for (int64_t step = 120; step <= 129; step++)
		{
			ring.push_back({ 99, 40, step });
			ring.push_back({ 110, 40, step });
		}
		// They lie outside the box and above the floor: the floor's lit layer (c_GITouchOccupancy), not solid, though the box's
		// bottom touches them along their edges from behind (a box's edge beside a voxel marked it solid, and closed every opening
		// on the grid by a voxel at its edges).
		constexpr uint8_t c_Touch = static_cast<uint8_t>(RenderData::c_GITouchOccupancy * 255.0f + 0.5f);
		uint32_t wrong = 0;
		for (const glm::i64vec3& voxel : ring)
		{
			const glm::vec4 normal = level0.GetNormal(voxel);
			wrong += level0.GetMaterial(voxel).a != c_Touch || normal.w != 1.0f || normal.y <= 0.0f ? 1 : 0;
		}
		CHECK(wrong == 0);
		uint32_t wrongOpen = 0;
		for (int64_t z = 90; z < 100; z++)
		{
			for (int64_t x = 70; x < 80; x++)
			{
				const glm::vec4 normal = level0.GetNormal(glm::i64vec3(x, 40, z));
				wrongOpen += normal.w != 1.0f || normal.y < 0.99f ? 1 : 0;
			}
		}
		CHECK(wrongOpen == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Curved and rough surfaces stay one-sided, a thin wall on the ground two-sided")
	{
		// A voxel is two-sided where its triangles face each other along an axis: both faces of a wall thinner than the voxel. A
		// tessellated sphere's pole (48 triangles leaning 3.75 degrees around it) and a rough heightfield (dozens of triangles a
		// voxel, each leaning up to 30 degrees) lean only a little each way, however many triangles add: one-sided at every
		// level. A 5 cm wall standing on finely tessellated ground stays two-sided at every level, its foot too, where the
		// ground's triangles outnumber the wall's.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle material = assets.AddMaterial(white);
		const AssetHandle sphere = assets.Add(MeshFactory::CreateSphere(3.0f), "Sphere");
		AddMesh(scene, sphere, material, glm::vec3(-5.05f, 2.45f, 6.05f), "Sphere"); // Its pole in the middle of a level-0 voxel
		// A 5 m heightfield of 5 cm cells, every vertex moved up or down by up to 1.5 cm (taken straight from mt19937's output,
		// which the standard fixes).
		const Ref<Mesh> flat = MeshFactory::CreatePlane(5.0f, 100);
		std::vector<glm::vec3> positions = flat->GetPositions();
		std::mt19937 random(7);
		for (glm::vec3& position : positions)
			position.y = 0.03f * static_cast<float>(static_cast<double>(random()) / 4294967296.0) - 0.015f;
		std::string error;
		const Ref<Mesh> roughMesh = Mesh::Create(positions, flat->GetAttributes(), flat->GetIndices(), flat->GetSubmeshes(), &error);
		REQUIRE_MESSAGE(roughMesh, error);
		const AssetHandle rough = assets.Add(roughMesh, "Rough");
		AddMesh(scene, rough, material, glm::vec3(3.0f, 0.37f, 5.0f), "Rough");
		// The wall (x -2.975 to -2.925, within one voxel of every level) on 3 m of ground in 5 cm cells, its top at y 0.
		const AssetHandle ground = assets.Add(MeshFactory::CreatePlane(3.0f, 60), "Ground");
		AddMesh(scene, ground, material, glm::vec3(-3.0f, 0.0f, -3.0f), "Ground");
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		AddMesh(scene, cube, material, glm::vec3(-2.95f, 0.5f, -3.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(0.05f, 1.0f, 2.0f);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		RenderUntilSettled(*renderer, scene, LookAt(glm::vec3(0.0f, 2.5f, 0.0f), glm::vec3(0.0f, 2.0f, 8.0f), c_Aspect));
		const VoxelClipmap* clipmap = renderer->GetVoxelClipmap();
		REQUIRE(clipmap);
		const std::vector<VoxelLevel> levels = ReadLevels(*renderer);
		REQUIRE(levels.size() == 4);
		for (uint32_t level = 0; level < levels.size(); level++)
		{
			CAPTURE(level);
			const float voxel = clipmap->GetLayout().Levels[level].VoxelSize;
			const VoxelBox& valid = clipmap->GetLevel(level).Valid;
			// The surface voxels of a box (world coordinates) inside the level's valid box.
			const auto countSides = [&](const glm::vec3& minimum, const glm::vec3& maximum, uint32_t& oneSided, uint32_t& twoSided)
			{
				oneSided = 0;
				twoSided = 0;
				const glm::i64vec3 first = glm::max(glm::i64vec3(glm::floor(minimum / voxel)), valid.Minimum);
				const glm::i64vec3 last = glm::min(glm::i64vec3(glm::floor(maximum / voxel)), valid.Maximum - int64_t(1));
				for (int64_t z = first.z; z <= last.z; z++)
				{
					for (int64_t y = first.y; y <= last.y; y++)
					{
						for (int64_t x = first.x; x <= last.x; x++)
						{
							const float w = levels[level].GetNormal(glm::i64vec3(x, y, z)).w;
							oneSided += w == 1.0f ? 1 : 0;
							twoSided += w == -1.0f ? 1 : 0;
						}
					}
				}
			};
			uint32_t oneSided = 0;
			uint32_t twoSided = 0;
			countSides(glm::vec3(-8.1f, -0.6f, 3.0f), glm::vec3(-2.0f, 5.5f, 9.1f), oneSided, twoSided);
			MESSAGE("level " << level << ", the sphere: " << oneSided << " one-sided voxels, " << twoSided << " two-sided");
			CHECK(oneSided > 0);
			CHECK(twoSided == 0);
			countSides(glm::vec3(0.5f, 0.355f, 2.5f), glm::vec3(5.5f, 0.385f, 7.5f), oneSided, twoSided);
			MESSAGE("level " << level << ", the heightfield: " << oneSided << " one-sided voxels, " << twoSided << " two-sided");
			CHECK(oneSided > 0);
			CHECK(twoSided == 0);
			// The wall's voxels halfway up and at its foot (the ground's top lies on every level's grid: the foot's voxels hold the
			// wall's faces and the ground's).
			for (const float height : { 0.5f * voxel, 0.55f })
			{
				CAPTURE(height);
				const glm::i64vec3 wall = glm::i64vec3(glm::floor(glm::vec3(-2.95f, height, -3.0f) / voxel));
				REQUIRE(valid.Contains(VoxelBox { wall, wall + int64_t(1) }));
				CHECK(levels[level].GetNormal(wall).w == -1.0f);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The voxels view shows the voxels' light")
	{
		// From stage 2 the view shows each surface's radiance (rgb / a) through the frame's exposure and x / (1 + x), and its
		// albedo times 0.03 where no light arrives (D15): a sunlit floor reads bright, the same floor in a box's shadow dark but not
		// black. The albedo the view showed before shaded both alike.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		white.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		const AssetHandle material = assets.AddMaterial(white);
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		AddMesh(scene, cube, material, glm::vec3(0.0f, -0.4f, 0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
		// Out of the camera's view, its shadow on the floor around (0, 0.1, 3.8).
		AddMesh(scene, cube, material, glm::vec3(-1.2f, 4.0f, 3.0f), "Occluder").GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 0.4f, 2.0f);
		const glm::vec3 toSun = -glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
		AddSun(scene, -toSun, 10.0f, false);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 256, 256);
		SceneRenderOptions options = VoxelView();
		options.PreExposure = 1.0f;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, -1.0f), glm::vec3(0.0f, 0.1f, 4.0f));
		RenderUntilSettled(*renderer, scene, camera, options);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer->GetOutputTexture(), image));
		const auto read = [&](const glm::vec3& point)
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, point, image.Width, image.Height);
			REQUIRE(pixel.x < image.Width);
			REQUIRE(pixel.y < image.Height);
			return GetPixelRGBA8(image, pixel.x, pixel.y);
		};
		const glm::u8vec4 lit = read(glm::vec3(1.8f, 0.1f, 2.0f));
		const glm::u8vec4 shadowed = read(glm::vec3(0.0f, 0.1f, 3.8f));
		// The sunlit floor: 0.8 * 10 lux * cos / pi, about 2.4 nits at a pre-exposure of 1, shows at x / (1 + x), about 0.7 (sRGB
		// 0.86). The shadowed floor: no light (one bounce, none from the sky), 0.8 * 0.03 (sRGB 0.17).
		const auto encode = [](float linear) { return (linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f) * 255.0f; };
		const float radiance = 0.8f * 10.0f * toSun.y / glm::pi<float>();
		const float expectedLit = encode(radiance / (1.0f + radiance));
		const float expectedShadowed = encode(0.8f * 0.03f);
		MESSAGE("lit " << int(lit.g) << " (expected " << expectedLit << "), shadowed " << int(shadowed.g) << " (expected " << expectedShadowed << ")");
		CHECK(std::abs(float(lit.g) - expectedLit) <= 6.0f);
		CHECK(std::abs(float(shadowed.g) - expectedShadowed) <= 6.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A sunlit slope and a ball under a low sun light themselves fully")
	{
		// The sun's walk from a solid surface voxel starts c_GIShadowOffsetVoxels along its normal from the voxel's center. On a
		// slope the voxels the plane crosses stand out of it as a staircase (up to 1.4 voxels at 45 degrees), and a walk toward a
		// sun low over the slope may cross them and read the surface's own shadow. Every voxel of an unshadowed 45-degree slope
		// facing it holds albedo * E * cos / pi, and a ball's lit side nearly so (where the sun is more than grazing). Starting
		// one voxel off at every tilt, the slope held 0.33-0.53 of its light with the sun 20 degrees over it (levels 0-3) and the
		// ball 0.66-0.79; c_GIShadowTiltVoxels gives the slope 0.998-1.000 with no voxel dark, the ball 0.94-0.98 with at most
		// 5.8% dark (at level 0, near the terminator).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		white.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		const AssetHandle material = assets.AddMaterial(white);
		const AssetHandle cube = assets.Add(MeshFactory::CreateCube(), "Cube");
		const glm::quat tilt = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		const glm::vec3 slopeCenter(3.0f, 1.0f, 6.0f);
		Entity slope = AddMesh(scene, cube, material, slopeCenter, "Slope");
		slope.GetComponent<TransformComponent>().Rotation = tilt;
		slope.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f, 1.0f, 10.0f);
		const glm::vec3 ballCenter(-5.0f, 3.0f, 6.0f);
		const AssetHandle sphere = assets.Add(MeshFactory::CreateSphere(2.0f), "Sphere");
		AddMesh(scene, sphere, material, ballCenter, "Ball");
		const glm::vec3 normal = tilt * glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 upSlope = tilt * glm::vec3(1.0f, 0.0f, 0.0f);
		const glm::vec3 across(0.0f, 0.0f, 1.0f);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 64, 64);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, 2.0f, 8.0f));
		Entity sun;
		for (const float above : { 20.0f, 35.0f })
		{
			CAPTURE(above);
			// The sun `above` degrees over the slope's plane, across its fall line; the ball's sun then stands low over the ground.
			const glm::vec3 toSun = glm::normalize(std::cos(glm::radians(above)) * glm::normalize(0.8f * upSlope + 0.6f * across)
				+ std::sin(glm::radians(above)) * normal);
			if (sun)
				scene.DestroyEntity(sun);
			sun = AddSun(scene, -toSun, 10.0f, false);
			RenderUntilSettled(*renderer, scene, camera);
			for (uint32_t level = 0; level < 4; level++)
			{
				CAPTURE(level);
				const VoxelLevel voxels = ReadLevel(*renderer, level);
				std::vector<uint8_t> radiance;
				REQUIRE(renderer->ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Radiance, radiance));
				const VoxelClipmap* clipmap = renderer->GetVoxelClipmap();
				const float voxel = clipmap->GetLayout().Levels[level].VoxelSize;
				const VoxelBox& valid = clipmap->GetLevel(level).Valid;
				double measured[2] = {};
				double expected[2] = {};
				uint32_t counted[2] = {};
				uint32_t dark[2] = {};
				for (int64_t z = valid.Minimum.z; z < valid.Maximum.z; z++)
				{
					for (int64_t y = valid.Minimum.y; y < valid.Maximum.y; y++)
					{
						for (int64_t x = valid.Minimum.x; x < valid.Maximum.x; x++)
						{
							const glm::i64vec3 index(x, y, z);
							const glm::vec4 stored = voxels.GetNormal(index);
							if (stored.w != 1.0f)
								continue;
							const glm::vec3 center = (glm::vec3(index) + 0.5f) * voxel;
							// Only voxels samples read (weight above 0: beyond it a walk takes the next level from its start).
							if (GlobalIllumination::GetLevelWeight(clipmap->GetLayout().Levels[level], center - camera.Position) <= 0.0f)
								continue;
							const glm::vec3 local = glm::inverse(tilt) * (center - slopeCenter);
							// The slope's top away from its edges, or the ball's side the sun reaches more than grazing.
							const glm::vec3 voxelNormal = glm::normalize(glm::vec3(stored));
							int part = -1;
							if (std::abs(local.x) < 4.0f && std::abs(local.z) < 4.0f && local.y > 0.0f && glm::dot(voxelNormal, normal) > 0.95f)
								part = 0;
							else if (glm::length(center - ballCenter) < 2.0f + 2.0f * voxel && glm::dot(voxelNormal, toSun) > 0.3f)
								part = 1;
							if (part < 0)
								continue;
							uint16_t value[4];
							std::memcpy(value, radiance.data() + voxels.GetIndex(index) * 8, sizeof(value));
							// rgb / a: coarser levels hold the finer ones' light times the share they cover.
							const float alpha = glm::unpackHalf1x16(value[3]);
							const float light = alpha > 0.0f ? glm::unpackHalf1x16(value[1]) / alpha / RenderData::c_GIRadianceScale : 0.0f;
							const float reference = (204.0f / 255.0f) * 10.0f * std::max(glm::dot(voxelNormal, toSun), 0.0f) / glm::pi<float>();
							measured[part] += light;
							expected[part] += reference;
							counted[part]++;
							dark[part] += light < 0.5f * reference ? 1u : 0u;
						}
					}
				}
				for (int part = 0; part < 2; part++)
				{
					CAPTURE(part);
					MESSAGE("sun " << above << " degrees over the slope, level " << level << ", " << (part == 0 ? std::string("slope") : std::string("ball")) << ": " << counted[part]
						<< " voxels, " << dark[part] << " in their own shadow, power " << measured[part] / std::max(expected[part], 1e-9));
					REQUIRE(counted[part] > 0u);
					CHECK(dark[part] <= (part == 0 ? 0u : counted[part] * 8u / 100u));
					CHECK(measured[part] >= (part == 0 ? 0.98 : 0.92) * expected[part]);
				}
			}
		}
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
		// The face voxels away from the edges (3 voxels in). Outside each face: within 10 degrees of the face's normal. Inside it
		// (the faces lie on voxel boundaries, so they mark the layer behind them too): occupied, no normal (behind every face).
		const float limit = std::cos(glm::radians(10.0f));
		uint32_t checked = 0;
		uint32_t wrong = 0;
		uint32_t wrongInside = 0;
		for (int64_t u = 3; u < 17; u++)
		{
			for (int64_t v = 3; v < 17; v++)
			{
				const std::pair<glm::i64vec3, glm::vec3> faces[] = { { { -1, u, 20 + v }, { -1, 0, 0 } }, { { 20, u, 20 + v }, { 1, 0, 0 } },
					{ { u, -1, 20 + v }, { 0, -1, 0 } }, { { u, 20, 20 + v }, { 0, 1, 0 } }, { { u, v, 19 }, { 0, 0, -1 } }, { { u, v, 40 }, { 0, 0, 1 } } };
				for (const auto& [voxel, expected] : faces)
				{
					const glm::vec4 normal = level0.GetNormal(voxel);
					checked++;
					wrong += normal.w != 1.0f || glm::dot(glm::normalize(glm::vec3(normal)), expected) < limit ? 1 : 0;
				}
				const glm::i64vec3 inside[] = { { 0, u, 20 + v }, { 19, u, 20 + v }, { u, 0, 20 + v }, { u, 19, 20 + v }, { u, v, 20 }, { u, v, 39 } };
				for (const glm::i64vec3& voxel : inside)
					wrongInside += level0.GetMaterial(voxel).a != 255 || level0.GetNormal(voxel) != glm::vec4(0.0f) ? 1 : 0;
			}
		}
		CHECK(checked > 1000);
		CHECK(wrong == 0);
		CHECK(wrongInside == 0);
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
		// The GI's counters of the frame that voxelizes it all: the tiles the CPU counts (both triangles are large in every level).
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		const SceneRendererGPUStats counts = VoxelizeAllAtOnce(*renderer, scene, camera);
		const VoxelClipmap& clipmap = *renderer->GetVoxelClipmap();
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.37f, -5.0f));
		CHECK(counts.GITiles == CountListedTiles(*quad, world, clipmap));
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

	TEST_CASE("A 145 m oblique triangle takes the tile path")
	{
		// A triangle 145 m across on all three axes (1,450 voxels an axis at level 0, whose product passes 2^31) through the windows:
		// large in every level, so the voxelizer lists its tiles everywhere, as the CPU counts them.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		const std::vector<glm::vec3> positions = { { 72.5f, -72.5f, 0.0f }, { 0.0f, 72.5f, -72.5f }, { -72.5f, 0.0f, 72.5f } };
		Submesh submesh;
		submesh.VertexCount = 3;
		submesh.LODs = { MeshLOD { 0, 3 } };
		const Ref<Mesh> slope = Mesh::Create(positions, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, { submesh });
		REQUIRE(slope);
		MaterialProperties ground;
		const glm::vec3 position(1.0f, 1.0f, 5.0f);
		AddMesh(scene, assets.Add(slope, "Slope"), assets.AddMaterial(ground), position, "Slope");
		Scope<SceneRenderer> renderer = CreateVoxelRenderer();
		const SceneRendererGPUStats counts = VoxelizeAllAtOnce(*renderer, scene, LookAt(glm::vec3(0.0f, 1.7f, 0.0f), position, c_Aspect));
		const uint64_t expected = CountListedTiles(*slope, glm::translate(glm::mat4(1.0f), position), *renderer->GetVoxelClipmap());
		CHECK(expected > 0);
		CHECK(counts.GITiles == expected);
		CHECK(counts.GITileOverflow == 0);
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
			REQUIRE(RenderGIFrame(*renderer, scene, path(frame), VoxelView()));
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
		REQUIRE(RenderGIFrame(*renderer, scene, camera, VoxelView()));
		CHECK(renderer->GetStats().GIMovingProxies == 1);
		const VoxelLevel moving = ReadLevel(*renderer, 0);
		CHECK(moving.GetMaterial(glm::i64vec3(10, 5, 20)).a == 0);
		CHECK(moving.GetMaterial(glm::i64vec3(20, 5, 20)).a == 0);
		for (uint32_t frame = 1; frame <= RenderData::c_GIMovingFrames; frame++)
			REQUIRE(RenderGIFrame(*renderer, scene, camera, VoxelView()));
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
			REQUIRE(RenderGIFrame(*renderer, scene, away, VoxelView()));
			CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
			REQUIRE(RenderGIFrame(*renderer, scene, home, VoxelView()));
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
				REQUIRE(RenderGIFrame(*renderer, scene, home, SceneRenderOptions {}));
			uint32_t pendingFrames = 0;
			uint32_t queued = UINT32_MAX;
			for (uint32_t frame = 0; frame < 200; frame++)
			{
				REQUIRE(RenderGIFrame(*renderer, scene, home, VoxelView()));
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
		REQUIRE(RenderGIFrame(*renderer, scene, turned, VoxelView()));
		CHECK(renderer->GetStats().GIVoxelizedTriangles == 0);
		CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
		// A reset of the history (a view switch, a quality change) leaves the voxels alone.
		renderer->ResetHistory();
		REQUIRE(RenderGIFrame(*renderer, scene, turned, VoxelView()));
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
			REQUIRE(RenderGIFrame(*renderer, scene, turned, VoxelView()));
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

	TEST_CASE("A random camera path keeps the voxels")
	{
		// Any camera path (RandomYardPath): after every step every level holds what a renderer made for that camera alone holds,
		// bit for bit.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const std::vector<SceneCamera> path = RandomYardPath(20261103u, 40);
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(ExactSpecification(), 64, 64);
		RenderUntilSettled(*renderer, scene, path[0]);
		size_t differing = 0;
		for (size_t step = 1; step < path.size(); step++)
		{
			REQUIRE(RenderGIFrame(*renderer, scene, path[step], VoxelView()));
			CHECK_FALSE(renderer->GetStats().GlobalIlluminationPending);
			const size_t differences = CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, path[0], path[step]));
			if (differences > 0)
			{
				MESSAGE("step ", step, ": ", differences, " voxels differ");
				differing++;
			}
		}
		CHECK(differing == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A random camera path under a small budget settles to the same voxels")
	{
		// The same kind of walk at 2,000 triangles a frame: the camera moves on every frame while regions are still queued (slabs,
		// whole windows after jumps), so queued boxes are cut to windows that moved again before they were done. Every 8 steps the
		// camera rests until the clipmap settles: then every level holds what a renderer made for that camera alone holds.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const std::vector<SceneCamera> path = RandomYardPath(7u, 40);
		SceneRendererSpecification specification = ExactSpecification();
		specification.GlobalIllumination.VoxelizeTrianglesPerFrame = 2000;
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 2000;
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(64, 64);
		RenderUntilSettled(*renderer, scene, path[0]);
		uint32_t unsettledFrames = 0;
		size_t differing = 0;
		for (size_t step = 1; step < path.size(); step++)
		{
			REQUIRE(RenderGIFrame(*renderer, scene, path[step], VoxelView()));
			unsettledFrames += renderer->GetStats().GlobalIlluminationSettled ? 0 : 1;
			if (step % 8 != 0 && step + 1 != path.size())
				continue;
			RenderUntilSettled(*renderer, scene, path[step]);
			const size_t differences = CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, path[0], path[step]));
			if (differences > 0)
			{
				MESSAGE("step ", step, ": ", differences, " voxels differ");
				differing++;
			}
		}
		MESSAGE(unsettledFrames, " of the walk's frames left regions queued");
		CHECK(unsettledFrames > 10);
		CHECK(differing == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("20 km out")
	{
		// The yard 20 km from the origin (and its camera): the voxels of the origin's yard, but for the floats' rounding there, which
		// moves the yard's faces up to a millimeter (coordinates within 1/256 voxel of a boundary snap onto it, so faces on the grid
		// stay there). At most 1% of occupied voxels differ, each next to an occupied voxel of the other (6 neighbors).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const glm::vec3 offset(20000.0f, 0.0f, 20000.0f);
		std::vector<VoxelLevel> levels[2];
		std::vector<glm::i64vec3> windows[2];
		const glm::i64vec3 dimensions(128, 64, 128);
		for (int which = 0; which < 2; which++)
		{
			SceneTestAssets assets;
			Scene scene;
			BuildYard(scene, assets);
			if (which == 1)
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
			RenderUntilSettled(*renderer, scene, YardCamera(which == 1 ? offset : glm::vec3(0.0f)));
			levels[which] = ReadLevels(*renderer);
			for (uint32_t level = 0; level < 4; level++)
				windows[which].push_back(renderer->GetVoxelClipmap()->GetLevel(level).Window);
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
						const bool atOrigin = occupiedAt(0, voxel);
						const bool atOffset = occupiedAt(1, voxel);
						occupied += atOrigin ? 1 : 0;
						if (atOrigin == atOffset)
							continue;
						differing++;
						// Next to an occupied voxel of the other.
						bool neighbor = false;
						for (int axis = 0; axis < 3; axis++)
						{
							for (int64_t side = -1; side <= 1; side += 2)
							{
								glm::i64vec3 next = voxel;
								next[axis] += side;
								neighbor = neighbor || occupiedAt(atOrigin ? 1 : 0, next);
							}
						}
						isolated += neighbor ? 0 : 1;
					}
				}
			}
			MESSAGE("level ", level, ": ", differing, " of ", occupied, " occupied voxels differ");
			CHECK(differing <= occupied / 100);
			CHECK(isolated == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Frames whose voxels cannot be recorded are not settled, and report once")
	{
		// The geometry pool's tables cannot be brought up to date (the CPU path, so only the voxelizer reads them): the frames that
		// voxelize the yard record nothing, so none of them is settled, and the error is logged when it appears, not every frame.
		// Once the tables can be uploaded, the voxels are those of a renderer made for the camera alone.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardCamera(glm::vec3(0.0f));
		SceneRendererSpecification specification = ExactSpecification();
		specification.GPUDriven = GPUDrivenMode::Off;
		Scope<SceneRenderer> renderer = CreateVoxelRenderer(specification, 64, 64);
		// Resident first, so the GI's frames have work.
		for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer->GetStats().PendingAssets > 0 || renderer->GetStats().DeferredProxies > 0); frame++)
			REQUIRE(RenderGIFrame(*renderer, scene, camera, SceneRenderOptions {}));
		GeometryPool& pool = *Renderer::GetGeometryPool();
		size_t reports = 0;
		{
			const LogCapture log;
			pool.SetTablesUnavailable(true);
			for (uint32_t frame = 0; frame < 4; frame++)
			{
				REQUIRE(RenderGIFrame(*renderer, scene, camera, VoxelView()));
				CHECK(renderer->GetStats().GlobalIllumination);
				CHECK_FALSE(renderer->GetStats().GlobalIlluminationSettled);
			}
			pool.SetTablesUnavailable(false);
			for (const LogEntry& entry : log.GetEntries())
			{
				if (entry.Message.find("global illumination's voxels wait") != std::string::npos)
					reports++;
			}
		}
		CHECK(reports == 1);
		RenderUntilSettled(*renderer, scene, camera);
		CHECK(CountDifferences(ReadLevels(*renderer), ReadFreshVoxels(scene, camera, camera)) == 0);
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
			REQUIRE(RenderGIFrame(*plain, scene, camera, SceneRenderOptions {}));
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
			REQUIRE(RenderGIFrame(*shown, scene, camera, VoxelView()));
		CHECK(shown->GetStats().GlobalIllumination);
		CHECK(shown->GetGlobalIlluminationMemoryBytes() > 0);
		for (uint32_t frame = 0; frame < 4; frame++)
			REQUIRE(RenderGIFrame(*shown, scene, camera, SceneRenderOptions {}));
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
		// culling on) give the same voxels, and draw the same gi view from them.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets, YardOptions { .GrassCards = 100 });
		std::vector<VoxelLevel> levels[2];
		ReadbackImage images[2];
		for (int path = 0; path < 2; path++)
		{
			SceneRendererSpecification specification = ExactSpecification();
			specification.GPUDriven = path == 0 ? GPUDrivenMode::Off : GPUDrivenMode::Auto;
			specification.OcclusionCulling = true;
			Scope<SceneRenderer> renderer = CreateVoxelRenderer(specification, 256, 256);
			for (uint32_t frame = 0; frame < 4; frame++)
				REQUIRE(RenderGIFrame(*renderer, scene, YardCamera(glm::vec3(0.0f)), VoxelView()));
			RenderUntilSettled(*renderer, scene, YardCamera(glm::vec3(0.0f)));
			CHECK(renderer->GetStats().GPUDriven == (path == 1));
			levels[path] = ReadLevels(*renderer);
			SceneRenderOptions giView;
			giView.LightingView = SceneLightingView::GlobalIllumination;
			for (uint32_t frame = 0; frame < 3; frame++)
				REQUIRE(RenderGIFrame(*renderer, scene, YardCamera(glm::vec3(0.0f)), giView));
			REQUIRE(renderer->GetStats().GlobalIllumination);
			REQUIRE(Renderer::ReadTexture(renderer->GetOutputTexture(), images[path]));
		}
		CHECK(CountDifferences(levels[0], levels[1]) == 0);
		REQUIRE(images[0].Pixels.size() == images[1].Pixels.size());
		CHECK(MaxDifference(images[0], images[1]) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
