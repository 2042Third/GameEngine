#pragma once

#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>
#include <glm/ext/vector_int3_sized.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The layout of global illumination: world-space radiance cascades traced through a voxel clipmap that follows the camera
// (Docs/DeveloperGuide.md, "Global illumination"). What a quality level makes (voxel levels, probe cascades, their memory)
// and the clipmap's arithmetic every part shares: windows snapped to whole steps, toroidal texels, scrolling slabs, the
// voxel coordinates of a point and the weight with which a sample reads a level. CPU only; the shaders mirror what they need
// (Include/GlobalIllumination.slang).
namespace Strata
{

	// Global illumination's tuning (SceneRendererSpecification::GlobalIllumination; the layout is the quality's GlobalIllumination
	// row, and the GI's views at Off show what High gives): the triangles the voxel clipmap voxelizes per frame (at least one
	// region a frame) and on a frame that rebuilds or jumps a window; 0 for no limit (renderers of single images, whose first frame
	// is then complete).
	struct GlobalIlluminationSettings
	{
		uint32_t VoxelizeTrianglesPerFrame = RenderData::c_GIVoxelizeTrianglesPerFrame;
		uint32_t VoxelizeTrianglesOnCut = RenderData::c_GIVoxelizeTrianglesOnCut;
	};

	// A level of the voxel clipmap: every level has the same dimensions, each twice the voxel size of the one before.
	struct VoxelLevelLayout
	{
		float VoxelSize = 0.0f;     // Meters: the first level's voxel size times 2^level
		glm::uvec3 Dimensions{ 0 }; // Voxels per axis
	};

	// A cascade of probes: all cascades cover the same field around the camera; each has half the probes per axis of the one
	// before, four times the directions (cascade 0 as many as cascade 1) and an interval four times as long.
	struct ProbeCascadeLayout
	{
		float Spacing = 0.0f;           // Meters: the first cascade's spacing times 2^k
		glm::uvec3 Grid{ 0 };           // Probes per axis: the first cascade's grid / 2^k
		uint32_t DirectionsPerAxis = 0; // The side of its octahedral tile (its square directions per probe): 8 for cascades 0 and 1, then 4 * 2^k
		float IntervalStart = 0.0f;     // Meters along the ray
		float IntervalEnd = 0.0f;       // Infinity for the top cascade: it marches until it leaves the clipmap
		// Meters of occlusion-only march before IntervalStart: the whole way from the probe for every cascade but the first, so
		// a wall between a child's interval and its parents' is always crossed by a marched side.
		float LeadIn = 0.0f;
		// Meters its rays march past IntervalEnd, gathering light (one spacing; 0 for the top cascade): a parent's interval starts
		// from the parent, up to 1.3 parent spacings beside the child, so a surface just past the child's interval end lies
		// before the parents' starts (in their occlusion-only lead-in). The overlap takes it on the child's own line: the sun
		// room's floor under a ceiling probe whose near parents sit in the ceiling. Its light ends the ray, so nothing counts
		// twice.
		float Overlap = 0.0f;
		// The finest voxel level its samples read: 0 for cascades 0 and 1, 1 above (no coarser: a 0.2 m wall in level 2's 0.4 m
		// voxels lets a sealed hall's top-cascade rays through; the top cascade has few rays, so the finer steps cost little).
		uint32_t MinimumLevel = 0;
		// Each direction is the mean of SubRays x SubRays rays spread over its texel (CascadeMarch.comp): 2 for the cascades between
		// cascade 0 and the top, whose texels are as wide as a room's sunlit patch seen across the room; 1 for cascade 0 (64
		// directions within a metre) and the top (outside, 1024).
		uint32_t SubRays = 1;
	};

	// The bytes of each resource a layout makes, as the renderer makes them.
	struct GlobalIlluminationMemory
	{
		uint64_t VoxelLevels = 0; // Material, emission, normal and radiance per voxel (20 bytes)
		uint64_t Scratch = 0;     // A voxelization batch's scratch (32 bytes per voxel)
		uint64_t Atlases = 0;     // The cascades' rays (RGBA16F)
		uint64_t Weights = 0;     // Each probe's eight parents' weights (two RGBA8), of every cascade but the top
		uint64_t Validity = 0;    // RGBA8 per probe of every cascade
		uint64_t Field = 0;       // Six RGBA16F faces per probe of the first cascade
		uint64_t Tiles = 0;       // The large triangles' tiles of a batch (8 bytes each)
		uint64_t WorkItems = 0;   // The frame's work items (16 bytes) and triangle groups (8 bytes)
		uint64_t Regions = 0;     // The frame's voxel regions (48 bytes each) and the GI's counters

		uint64_t GetTotal() const;
	};

	struct GlobalIlluminationLayout
	{
		std::vector<VoxelLevelLayout> Levels;
		std::vector<ProbeCascadeLayout> Cascades;
		// Voxels a voxelization batch's regions hold together: the scratch's size (one 64^3 batch at High; one level at Low,
		// whose levels are smaller).
		uint32_t BatchVoxels = 0;
		glm::vec3 FieldExtent{ 0.0f };     // Meters the probe cascades cover
		float FieldSnap = 0.0f;            // The field moves in whole steps of the top cascade's spacing
		glm::vec2 FieldFullExtent{ 0.0f }; // Meters around the camera where the field has full weight: horizontally, vertically
		float FieldFade = 0.0f;            // Meters over which it fades to the fallback beyond that

		GlobalIlluminationMemory GetMemory() const;
		uint64_t GetMemoryBytes() const { return GetMemory().GetTotal(); }
		uint64_t GetRaysPerFrame() const;
	};

	// A box of voxels in world voxel indices (or relative to a window or an anchor where named): [Minimum, Maximum).
	struct VoxelBox
	{
		glm::i64vec3 Minimum{ 0 };
		glm::i64vec3 Maximum{ 0 };

		bool IsEmpty() const { return Maximum.x <= Minimum.x || Maximum.y <= Minimum.y || Maximum.z <= Minimum.z; }
		uint64_t GetVoxelCount() const;
		bool Contains(const VoxelBox& other) const;
		bool Intersects(const VoxelBox& other) const;
		VoxelBox Intersection(const VoxelBox& other) const;
		bool operator==(const VoxelBox& other) const = default;
	};

	namespace GlobalIllumination
	{
		// The layout of a quality level (Off: none).
		std::optional<GlobalIlluminationLayout> ComputeLayout(GlobalIlluminationLevel level);

		// Octahedral direction of texel (u, v) of an a x a tile: the engine's OctahedralDecode (Include/Scene.slang) of the
		// texel's center mapped to [-1, 1]^2. Its solid angle: the octahedron's Jacobian 1 / |p|^3 at the texel's center (p the
		// point on the octahedron, |p.x| + |p.y| + |p.z| = 1), normalized so a tile's add up to 4 pi.
		glm::vec3 GetDirection(uint32_t u, uint32_t v, uint32_t a);
		float GetSolidAngle(uint32_t u, uint32_t v, uint32_t a);
		// The field's face axes (+X, -X, +Y, -Y, +Z, -Z), and C_f: the sum over an a x a tile's directions of max(0, d . axis) times
		// their solid angles. The reduce divides by it instead of pi, so a uniform radiance gives every face exactly that radiance
		// (16 directions sum to 0.959 pi for the X and Y faces and 1.115 pi for the Z faces: OctahedralDecode folds along z).
		glm::vec3 GetFaceAxis(uint32_t face);
		// What GetSolidAngle multiplies the octahedron's Jacobian 1 / |p|^3 by for an a x a tile (its texels add up to 4 pi).
		float GetSolidAngleNormalization(uint32_t a);
		float GetFaceCosineSum(uint32_t face, uint32_t a);
		// The probe field's minimum corner around a camera: the nearest whole step of the layout's FieldSnap to the camera less
		// half the field's extent, per axis, so the camera stays within half a step of the field's center.
		glm::dvec3 GetFieldMinimum(const GlobalIlluminationLayout& layout, const glm::dvec3& cameraPosition);

		// The world voxel index of a position: floor(position / voxelSize).
		glm::i64vec3 GetVoxelIndex(const glm::dvec3& position, float voxelSize);
		// The minimum corner of a level's window around a camera: centered on the camera's voxel, snapped down to a multiple of
		// c_GIScrollVoxels, so the camera lies between Dimensions / 2 and Dimensions / 2 + c_GIScrollVoxels voxels from it.
		glm::i64vec3 GetWindowMinimum(const VoxelLevelLayout& level, const glm::dvec3& cameraPosition);
		VoxelBox GetWindow(const VoxelLevelLayout& level, const glm::i64vec3& minimum);
		// Where a world voxel index is stored in a level's textures: its non-negative remainder by the dimensions.
		glm::uvec3 GetTexel(const glm::i64vec3& voxel, const glm::uvec3& dimensions);
		// Appends the pieces of a box: split where its texels wrap (each piece contiguous in the textures) and cut into pieces
		// of at most c_GIMaxRegionVoxels a side.
		void SplitRegion(const VoxelBox& box, const glm::uvec3& dimensions, std::vector<VoxelBox>& outPieces);
		// The slabs a window's move from oldMinimum to newMinimum brings in: disjoint boxes whose union is the new window minus
		// the old one, split as SplitRegion. False, appending nothing, when the move is at least the dimensions on an axis: the
		// window holds nothing of the old one and is rebuilt whole.
		bool GetScrollSlabs(const glm::i64vec3& oldMinimum, const glm::i64vec3& newMinimum, const glm::uvec3& dimensions, std::vector<VoxelBox>& outSlabs);
		// A box grown outward to whole bricks of c_GIRegionBrick voxels.
		VoxelBox SnapToBricks(const VoxelBox& box);
		// The anchor of a window: its minimum snapped down to a multiple of c_GIAnchorVoxels.
		glm::i64vec3 GetAnchorVoxel(const glm::i64vec3& windowMinimum);

		// The voxel coordinate of a mesh position, as Include/Voxelize.slang computes it: the instance's world rows applied to it
		// with their translation taken relative to the anchor's world position first (anchor: the anchor voxel times the voxel
		// size, as a float), divided by the voxel size, and snapped onto a voxel boundary within c_GISnapVoxels of it (faces meant
		// to meet do). Its floor is the voxel index relative to the anchor voxel. Independent of the camera: what voxelizes
		// relative to the same anchor gives the same bits.
		glm::vec3 GetVoxelCoordinate(const RenderData::AffineTransform& world, const glm::vec3& position, const glm::vec3& anchor, float voxelSize);

		// The weight with which a sample at a camera-relative point reads a level rather than the coarser ones: 1 within the
		// window's half extent less (c_GIScrollVoxels + c_GILevelGuardVoxels + c_GILevelBlendVoxels) voxels of the camera on every
		// axis, falling to 0 over c_GILevelBlendVoxels voxels. By the distance from the camera, not from the snapped window, so
		// it changes continuously as the camera moves (Include/GlobalIllumination.slang's GetLevelWeight).
		float GetLevelWeight(const VoxelLevelLayout& level, const glm::vec3& relative);

		// A material's voxel colors (RenderData::GIMaterial): linear RGB in [0, 1] as 8 bits per channel (red in the low byte),
		// and a color in nits as shared-exponent RGB9E5 (EXT_texture_shared_exponent; values clamped to [0, 65408], what
		// Include/Voxelize.slang unpacks).
		uint32_t PackAlbedo(const glm::vec3& albedo);
		uint32_t PackRGB9E5(const glm::vec3& color);
		glm::vec3 UnpackRGB9E5(uint32_t value);
	}

}
