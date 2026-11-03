#ifndef STRATA_VOXELIZE_SLANG
#define STRATA_VOXELIZE_SLANG

// The voxelizer of global illumination (GI/VoxelTriangles.comp, VoxelTiles.comp, VoxelFill.comp, VoxelFinalize.comp; the frame's
// work from VoxelClipmap): triangles of the geometry pool, through their instances' world rows, into a batch's scratch with
// buffer atomics only, so the result is independent of thread order. The triangle-box test, the winding steps and the
// emission coverage are ported from radiance-cascades-lab (Daniel Quevedo, MIT; src/rc/shaders/voxelize_mesh.comp and
// fill_solid.comp; ThirdPartyNotices.md, "Code derived from other projects").

#include "Include/Common.slang"
#include "Include/GeometryTables.slang"
#include "Include/GlobalIllumination.slang"
#include "Include/Scene.slang"

// RenderData::VoxelRegion, VoxelWorkItem, VoxelBatch (SceneRenderData.h).
struct VoxelRegion
{
	int4 Minimum; // xyz: the first voxel, relative to the level's anchor voxel; w: the level
	int4 Maximum; // xyz: one past the last voxel; w: the first voxel in the batch's scratch
	int4 Texel;   // xyz: the texel of Minimum; w: the first column (x, z) among the batch's
};

struct VoxelWorkItem
{
	uint Instance;
	uint MeshSubmeshLOD; // Mesh slot (bits 0-19), submesh (20-27), level of detail (28-31)
	uint Region;
	uint Flags;          // ST_VOXEL_ITEM_*
};

struct VoxelBatch
{
	uint Item;
	uint FirstTriangle;
};

// RenderData::GIMaterial
struct GIMaterial
{
	uint Albedo;   // Linear diffuse albedo, RGB8
	uint Emission; // RGB9E5, nits
	uint Flags;    // ST_CULL_MATERIAL_* (unused here: the CPU turned them into the items' flags)
	uint Padding;
};

// RenderData::VoxelPassParameters (push constants, b2).
struct VoxelPassParameters
{
	uint Level;
	uint FirstRegion; // The batch's regions in u_Regions
	uint RegionCount;
	uint FirstGroup;  // The batch's triangle groups in u_Groups
	uint Voxels;      // Of the batch's regions together
	uint Columns;
	uint GroupCount;
	uint Padding;
};

[[vk::push_constant]] ConstantBuffer<VoxelPassParameters> u_Pass;

StructuredBuffer<GIMaterial> u_GIMaterials : register(t30);
StructuredBuffer<uint> u_MeshTable : register(t31);
StructuredBuffer<GeometryEntry> u_GeometryEntries : register(t32);
StructuredBuffer<GeometrySubmesh> u_GeometrySubmeshes : register(t33);
StructuredBuffer<GeometryLOD> u_GeometryLODs : register(t34);
ByteAddressBuffer u_Positions : register(t35); // The Static arena's positions, 12 bytes each
ByteAddressBuffer u_Indices : register(t36);
StructuredBuffer<VoxelRegion> u_Regions : register(t37);
StructuredBuffer<VoxelWorkItem> u_Items : register(t38);
StructuredBuffer<VoxelBatch> u_Groups : register(t39);

// The batch's scratch, eight words per voxel of the scratch in blocks of u_GI.Batch.x: keys, winding steps, emission, the normals'
// three sums, the emission's coverage and how many triangles added a normal.
RWByteAddressBuffer u_Scratch : register(u3);
// The large triangles' tiles: x the triangle (its group in the frame times 64 plus its lane), y the tile's first voxel relative
// to the region's minimum, 10 bits per axis.
RWStructuredBuffer<uint2> u_Tiles : register(u4);
// VoxelTiles' dispatch arguments: x the tiles of the batch (at most ST_GI_MAX_TILES).
RWByteAddressBuffer u_TileArguments : register(u5);
// The GI's counters (GIStats): 0 tiles made, 1 triangles that walked their own tiles (the tile list was full).
RWByteAddressBuffer u_Counters : register(u6);

#define ST_GI_MESH_SKINNED_ARENA 0x80000000u // c_CullMeshSkinnedArena: the voxelizer reads the Static arena only
#define ST_SCRATCH_KEYS 0u
#define ST_SCRATCH_WINDING 1u
#define ST_SCRATCH_EMISSION 2u
#define ST_SCRATCH_NORMALS 3u
#define ST_SCRATCH_COVERAGE 6u
#define ST_SCRATCH_NORMAL_COUNT 7u
#define ST_GI_COVERAGE_FIXED_POINT 65536.0 // A whole voxel face's coverage
#define ST_GI_COUNTER_TILES 0u
#define ST_GI_COUNTER_TILE_OVERFLOW 4u

// Voxel classes in a key's bits 30-31: the largest key wins, so opaque surfaces win over alpha-masked ones. Bit 29: a triangle
// reaches into the voxel or touches it from behind (solid); without it every triangle marking the voxel lies on its boundary
// facing into it (the layer in front of a face on the grid: ST_GI_TOUCH_OCCUPANCY). Bit 28: double-sided.
#define ST_VOXEL_CLASS_MASKED 1u
#define ST_VOXEL_CLASS_INTERIOR 2u
#define ST_VOXEL_CLASS_OPAQUE 3u
#define ST_VOXEL_KEY_SOLID (1u << 29)
#define ST_VOXEL_KEY_DOUBLE_SIDED (1u << 28)

uint GetScratchAddress(uint block, uint voxel)
{
	return (block * u_GI.Batch.x + voxel) * 4u;
}

// A region voxel's index in the batch's scratch (local: relative to the region's minimum).
uint GetScratchVoxel(VoxelRegion region, int3 local)
{
	int3 size = region.Maximum.xyz - region.Minimum.xyz;
	return uint(region.Maximum.w + local.x + size.x * (local.y + size.y * local.z));
}

// Shared exponent RGB9E5 (EXT_texture_shared_exponent), the exponent read from the float's bits so the CPU
// (GlobalIllumination::PackRGB9E5) gives the same words; values clamped to [0, 65408].
uint PackRGB9E5(float3 color)
{
	float3 clamped = clamp(select(isnan(color), float3(0.0), color), float3(0.0), float3(65408.0));
	float largest = max(clamped.x, max(clamped.y, clamped.z));
	if (largest < 1.52587891e-5) // Below the smallest step of exponent 0 (2^-16): zero
		return 0u;
	int exponent = max(-16, int((asuint(largest) >> 23) & 255u) - 127) + 16; // floor(log2) + 1 + bias 15
	float denominator = exp2(float(exponent - 24));
	if (floor(largest / denominator + 0.5) >= 512.0)
	{
		exponent++;
		denominator *= 2.0;
	}
	uint3 mantissa = uint3(min(floor(clamped / denominator + 0.5), float3(511.0)));
	return mantissa.x | (mantissa.y << 9) | (mantissa.z << 18) | (uint(exponent) << 27);
}

// The voxel coordinate of a mesh position (GlobalIllumination::GetVoxelCoordinate): the world rows' translation relative to the
// anchor first, so the result depends on the anchor alone, never on the camera; precise, so the CPU's reference gives the same bits.
// Within ST_GI_SNAP_VOXELS of a voxel boundary it snaps onto it: faces meant to lie on the grid, or on each other (a box's bottom on
// a floor's top), meet exactly, whatever the rounding of their instances' rows.
float3 GetVoxelCoordinate(InstanceData instance, float3 position, uint level)
{
	GIVoxelLevel data = u_GI.Levels[level];
	precise float3 coordinate;
	[unroll]
	for (int row = 0; row < 3; row++)
	{
		float4 rows = instance.WorldRows[row];
		precise float translation = rows.w - data.Anchor[row];
		precise float rotated = (rows.x * position.x + rows.y * position.y) + rows.z * position.z;
		precise float value = (rotated + translation) / data.Origin.w;
		float boundary = round(value);
		coordinate[row] = abs(value - boundary) <= ST_GI_SNAP_VOXELS ? boundary : value;
	}
	return coordinate;
}

// --- Akenine-Moller's triangle-box overlap (2001), in radiance-cascades-lab's form ---

bool AxisSeparates(float3 axis, float3 v0, float3 v1, float3 v2, float3 halfSize)
{
	float p0 = dot(v0, axis);
	float p1 = dot(v1, axis);
	float p2 = dot(v2, axis);
	float radius = halfSize.x * abs(axis.x) + halfSize.y * abs(axis.y) + halfSize.z * abs(axis.z);
	return max(max(p0, p1), p2) < -radius || min(min(p0, p1), p2) > radius;
}

// Whether a triangle overlaps a box (center, half size); touching counts.
bool TriangleBoxOverlap(float3 center, float3 halfSize, float3 a, float3 b, float3 c)
{
	float3 v0 = a - center;
	float3 v1 = b - center;
	float3 v2 = c - center;
	float3 e0 = v1 - v0;
	float3 e1 = v2 - v1;
	float3 e2 = v0 - v2;
	float3 axes[3] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };
	[unroll]
	for (int axis = 0; axis < 3; axis++)
	{
		if (AxisSeparates(cross(axes[axis], e0), v0, v1, v2, halfSize) || AxisSeparates(cross(axes[axis], e1), v0, v1, v2, halfSize)
			|| AxisSeparates(cross(axes[axis], e2), v0, v1, v2, halfSize))
			return false;
	}
	if (max(max(v0.x, v1.x), v2.x) < -halfSize.x || min(min(v0.x, v1.x), v2.x) > halfSize.x)
		return false;
	if (max(max(v0.y, v1.y), v2.y) < -halfSize.y || min(min(v0.y, v1.y), v2.y) > halfSize.y)
		return false;
	if (max(max(v0.z, v1.z), v2.z) < -halfSize.z || min(min(v0.z, v1.z), v2.z) > halfSize.z)
		return false;
	float3 normal = cross(e0, e1);
	return abs(dot(normal, v0)) <= halfSize.x * abs(normal.x) + halfSize.y * abs(normal.y) + halfSize.z * abs(normal.z);
}

// --- A triangle of the frame's work ---

struct VoxelTriangle
{
	float3 A;          // Voxel coordinates relative to the level's anchor voxel
	float3 B;
	float3 C;
	VoxelRegion Region;
	uint Flags;        // The item's ST_VOXEL_ITEM_*
	uint Key;          // What its voxels take with InterlockedMax: class, double-sided bit, albedo
	float3 Emission;   // Nits (before the coverage)
	int3 Normal;       // Its unit normal times ST_GI_NORMAL_FIXED_POINT (0 for double-sided items)
	float3 Facing;     // The side its front faces (not normalized; mirrored instances' turned)
};

// Triangle `lane` of the frame's triangle group `group`; false past the item's triangles.
bool LoadVoxelTriangle(uint group, uint lane, out VoxelTriangle triangle)
{
	triangle = (VoxelTriangle)0;
	VoxelBatch batch = u_Groups[group];
	VoxelWorkItem item = u_Items[batch.Item];
	uint index = batch.FirstTriangle + lane;
	uint meshSlot = item.MeshSubmeshLOD & 0xFFFFFu;
	uint submeshIndex = (item.MeshSubmeshLOD >> 20) & 255u;
	uint level = item.MeshSubmeshLOD >> 28;
	uint handle = u_MeshTable[meshSlot];
	if (handle == 0u || (handle & ST_GI_MESH_SKINNED_ARENA) != 0u)
		return false;
	GeometryEntry entry = u_GeometryEntries[handle];
	if (submeshIndex >= entry.SubmeshCount)
		return false;
	GeometrySubmesh submesh = u_GeometrySubmeshes[entry.FirstSubmesh + submeshIndex];
	if (submesh.LODCount == 0u)
		return false;
	GeometryLOD lod = u_GeometryLODs[submesh.FirstLOD + min(level, submesh.LODCount - 1u)];
	if (index * 3u + 2u >= lod.IndexCount)
		return false;
	InstanceData instance = u_Instances[item.Instance];
	triangle.Region = u_Regions[item.Region];
	uint regionLevel = uint(triangle.Region.Minimum.w);
	float3 corners[3];
	[unroll]
	for (uint corner = 0; corner < 3u; corner++)
	{
		uint vertex = u_Indices.Load((entry.FirstIndex + lod.IndexOffset + index * 3u + corner) * 4u);
		float3 position = asfloat(u_Positions.Load3((entry.FirstVertex + lod.BaseVertex + vertex) * 12u));
		corners[corner] = GetVoxelCoordinate(instance, position, regionLevel);
	}
	triangle.A = corners[0];
	triangle.B = corners[1];
	triangle.C = corners[2];
	triangle.Flags = item.Flags;

	GIMaterial material = u_GIMaterials[instance.MaterialIndex];
	bool doubleSided = (item.Flags & ST_VOXEL_ITEM_DOUBLE_SIDED) != 0u;
	uint voxelClass = (item.Flags & ST_VOXEL_ITEM_ALPHA_MASKED) != 0u ? ST_VOXEL_CLASS_MASKED : ST_VOXEL_CLASS_OPAQUE;
	triangle.Key = (voxelClass << 30) | (doubleSided ? ST_VOXEL_KEY_DOUBLE_SIDED : 0u) | (material.Albedo & 0xFFFFFFu);
	triangle.Emission = UnpackRGB9E5(material.Emission);
	// Counter-clockwise front faces (glTF); a mirrored instance's triangles turn the other way.
	float3 normal = cross(triangle.B - triangle.A, triangle.C - triangle.A);
	float normalLength = sqrt(dot(normal, normal));
	if ((item.Flags & ST_VOXEL_ITEM_MIRRORED) != 0u)
		normal = -normal;
	triangle.Normal = doubleSided || !(normalLength > 0.0) ? int3(0, 0, 0) : int3(round(normal / normalLength * ST_GI_NORMAL_FIXED_POINT));
	triangle.Facing = normal;
	return true;
}

// Whether a voxel (anchor-relative index) lies wholly behind a triangle's plane, which then only touches its boundary from behind:
// the bottom of a box resting on a floor touches the floor's voxels around the box's foot along their edges, and a face on the
// grid touches the voxels behind it. Exact for faces on the grid (their facing has two zero components; coordinates snapped).
bool IsVoxelBehind(VoxelTriangle triangle, int3 voxel)
{
	float3 facing = triangle.Facing;
	precise float reach = dot(facing, (float3(voxel) + 0.5) - triangle.A) + 0.5 * (abs(facing.x) + abs(facing.y) + abs(facing.z));
	return reach <= 0.0;
}

// Whether a voxel (anchor-relative index) lies wholly in front of a triangle's plane, which then only touches its boundary from
// the front: the voxel in front of a face on the grid, marked so the face has a lit layer (touching counts as overlap).
bool IsVoxelInFront(VoxelTriangle triangle, int3 voxel)
{
	float3 facing = triangle.Facing;
	precise float reach = dot(facing, (float3(voxel) + 0.5) - triangle.A) - 0.5 * (abs(facing.x) + abs(facing.y) + abs(facing.z));
	return reach >= 0.0;
}

// The writes of a triangle to a voxel of its region's scratch (local: relative to the region's minimum). A glowing triangle adds
// the share of the voxel's face it covers (fixed point, integer adds: the same in any order); the voxel's emission is the
// brightest of its triangles' times their coverage together, at most 1 (GI/VoxelFinalize.comp). Its normal goes only to voxels
// not wholly behind it: a face on a voxel boundary faces the voxel in front of it, and one touching a voxel from behind (a resting
// box's bottom, the floor's top under it) cannot cancel the faces the voxel shows.
void WriteVoxel(VoxelTriangle triangle, int3 local, uint emission, uint coverage)
{
	uint voxel = GetScratchVoxel(triangle.Region, local);
	uint previous;
	// Double-sided sheets are solid wherever they touch (both of their sides are surfaces).
	bool solid = (triangle.Key & ST_VOXEL_KEY_DOUBLE_SIDED) != 0u || !IsVoxelInFront(triangle, triangle.Region.Minimum.xyz + local);
	u_Scratch.InterlockedMax(GetScratchAddress(ST_SCRATCH_KEYS, voxel), triangle.Key | (solid ? ST_VOXEL_KEY_SOLID : 0u), previous);
	if (emission != 0u)
	{
		u_Scratch.InterlockedMax(GetScratchAddress(ST_SCRATCH_EMISSION, voxel), emission, previous);
		u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_COVERAGE, voxel), coverage, previous);
	}
	if (any(triangle.Normal != int3(0, 0, 0)) && !IsVoxelBehind(triangle, triangle.Region.Minimum.xyz + local))
	{
		// Signed sums as two's complement words: integer adds, the same whatever the order.
		u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_NORMALS + 0u, voxel), uint(triangle.Normal.x), previous);
		u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_NORMALS + 1u, voxel), uint(triangle.Normal.y), previous);
		u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_NORMALS + 2u, voxel), uint(triangle.Normal.z), previous);
		u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_NORMAL_COUNT, voxel), 1u, previous);
	}
}

#endif
