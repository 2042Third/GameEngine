#include "Include/Common.slang"

#include "Include/GlobalIllumination.slang"

// The dynamic occupancy layer of moving objects (Docs/DeveloperGuide.md, "Global illumination"; R5 of the radiance cascades'
// research): a group of threads per brick of ST_GI_MOVER_BRICK_VOXELS^3 voxels that the frame's moving boxes reach, a thread
// per voxel. The idea of a coverage per voxel for moving boxes is radiance-cascades-lab's (Daniel Quevedo, MIT;
// src/rc/shaders/voxelize_boxes.comp, which counts 4 x 4 x 4 point samples; here the share is analytic, so a face that moves
// along its normal changes its voxels continuously, not in four steps a voxel).
//
// The layer is written into the level's own textures, so every march reads max(static, dynamic) with the reads it had, and
// what the bricks held is kept in a backup and put back before the next frame's voxelization:
// - u_Pass.Restore 1: every voxel of the brick that lay in the level's window takes back the material, emission and normal the
//   backup holds (the bricks are the frame before's, with the texels and slots they were written with).
// - u_Pass.Restore 0: every voxel of the brick in the window stores what it holds in the backup, then takes the box that
//   gives it the most occupancy, where that is more than it holds:
//   - Material: the box's albedo and an occupancy from its coverage (GetMoverOccupancy): the box's share of the voxel's
//     surroundings, per axis of the box and multiplied, under a tent two voxels wide. A box thinner than two and a half
//     voxels counts as that thick, which fills the voxel at its middle (what is thinner than a voxel is a whole voxel in the
//     static voxels too). Alpha-masked boxes count ST_GI_MASKED_OCCUPANCY of it.
//   - Normal: where a face of the box passes through the voxel (or lies on its boundary), the faces' outward normals by how
//     near they pass, w 1 (0.5 double-sided; -1, a dark two-sided surface, where the box is thinner than the voxel along the
//     face's axis): one layer of surface voxels around the box, which injection lights (GI/VoxelInject.comp starts the sun's
//     walk a voxel off them or on them, outside what the box itself absorbs). Deeper voxels are interior: no normal, no light.
//   - Emission: the box's on its surface voxels.
//   A voxel that held a surface of its own below the box's occupancy (the layer in front of a floor's face, a cut-out) turns
//   into the box's by how far that has passed its own, so it does not change in a step.

// RenderData::VoxelMover
struct VoxelMover
{
	float4 Center; // xyz: camera-relative (m)
	float4 AxisX;  // xyz: the half extent along the box's first axis
	float4 AxisY;
	float4 AxisZ;
	uint Albedo;   // Linear diffuse albedo, RGB8
	uint Emission; // RGB9E5, nits
	uint Flags;    // ST_VOXEL_MOVER_*
	uint Padding;
};

// RenderData::VoxelMoverBrick
struct VoxelMoverBrick
{
	int4 Local;   // xyz: the brick's first voxel relative to the level's window; w: its slot in the backup
	int4 Texel;   // xyz: that voxel's texel; w: the level
	uint4 Movers; // xy: the boxes that reach it, a bit each
};

// RenderData::VoxelMoverPassParameters (push constants, b2).
struct VoxelMoverPassParameters
{
	uint Level;
	uint FirstBrick;
	uint BrickCount;
	uint Restore;
};

#define ST_GI_MOVER_BRICK_VOXELS 8        // c_GIMoverBrickVoxels
#define ST_VOXEL_MOVER_ALPHA_MASKED 1u    // c_VoxelMoverAlphaMasked
#define ST_VOXEL_MOVER_DOUBLE_SIDED 2u    // c_VoxelMoverDoubleSided
// A face counts as passing through a voxel up to this share of a voxel beyond it: the layer of surface voxels never has a gap
// where a face lies on the grid.
#define ST_GI_MOVER_SURFACE_SLACK (1.0 / 64.0)
// The occupancy of a voxel a box covers whole at its surface: one over ST_GI_OCCUPANCY_GAIN, so what a march's sample absorbs,
// saturate(occupancy * gain), is the coverage itself, and a face that moves through a voxel darkens it evenly all the way
// (stored as the coverage, the voxel was opaque from two fifths of the way on: a step per voxel crossed). Voxels more than a
// voxel inside rise to 1 over the next voxel inward, the solid core the forward gather's guard reads (ST_GI_GATHER_GUARD_CORE).
#define ST_GI_MOVER_SURFACE_OCCUPANCY (1.0 / ST_GI_OCCUPANCY_GAIN)
// The box is weighed this share of a voxel smaller on every side: weighed whole, the soft edge and the marches' absorption, which
// saturates, made it too large (the ground beside a 0.5 m box lost 12.0% of its light for the 9.8% of its sky the box hides;
// 7.2% with half a voxel; 10.1% with this, and the bounce of its sunlit face read 1.01 of its static voxels': GPU.SceneRenderer.
// GlobalIlluminationRoom's moving props).
#define ST_GI_MOVER_INSET 0.25

[[vk::push_constant]] ConstantBuffer<VoxelMoverPassParameters> u_Pass;

StructuredBuffer<VoxelMoverBrick> u_Bricks : register(t0);
StructuredBuffer<VoxelMover> u_Movers : register(t1);
[vk::image_format("rgba8")] RWTexture3D<float4> o_Material : register(u0);
[vk::image_format("r32ui")] RWTexture3D<uint> o_Emission : register(u1);
[vk::image_format("rgba8snorm")] RWTexture3D<float4> o_Normal : register(u2);
// Three words per voxel of a brick: the material's bytes, the emission and the normal's bytes.
RWByteAddressBuffer o_Backup : register(u3);

uint PackUnormBytes(float4 value)
{
	uint4 bytes = uint4(saturate(value) * 255.0 + 0.5);
	return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

// RGBA8_SNORM as the texture holds it (-127 to 127), moved to 0-254.
uint PackSnormBytes(float4 value)
{
	uint4 bytes = uint4(clamp(value, -1.0, 1.0) * 127.0 + 127.5);
	return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

float4 UnpackSnormBytes(uint value)
{
	return (float4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float(value >> 24)) - 127.0) / 127.0;
}

// The integral of the tent 1 - |u| over [-1, 1] up to `u` (0 to 1).
float GetTentIntegral(float u)
{
	float clamped = clamp(u, -1.0, 1.0);
	return clamped < 0.0 ? 0.5 * (clamped + 1.0) * (clamped + 1.0) : 1.0 - 0.5 * (1.0 - clamped) * (1.0 - clamped);
}

// The occupancy a box gives the voxel centered at camera-relative `position` (0: none), and in `normal` the surface it puts
// there (xyz the outward normal, w 1, or -1 where the box is thinner than the voxel; all zero: none, the voxel lies outside the
// box or deeper inside than a face reaches).
float GetMoverOccupancy(VoxelMover mover, float3 position, float voxel, out float4 normal)
{
	normal = float4(0.0, 0.0, 0.0, 0.0);
	float3 axes[3] = { mover.AxisX.xyz, mover.AxisY.xyz, mover.AxisZ.xyz };
	float3 offset = position - mover.Center.xyz;
	float coverage = 1.0;
	float inside = 1.0;
	float3 summed = float3(0.0, 0.0, 0.0);
	bool touched = true;
	bool surface = false;
	bool thin = false;
	[unroll]
	for (uint axis = 0u; axis < 3u; axis++)
	{
		float extent = length(axes[axis]);
		// A flat box (a plane's bounds) has no length along one axis: the other two give its direction.
		float3 unit = extent > 1e-6 ? axes[axis] / extent : cross(axes[(axis + 1u) % 3u], axes[(axis + 2u) % 3u]);
		float unitLength = length(unit);
		if (unitLength < 1e-12)
			return 0.0;
		unit /= unitLength;
		// The voxel's extent along the axis, and the box's half extent, at least so much that the weights below leave a voxel at
		// its middle full (thinner counts as that thick).
		float width = voxel * (abs(unit.x) + abs(unit.y) + abs(unit.z));
		float halfExtent = max(extent, (1.0 + ST_GI_MOVER_INSET) * width);
		float along = dot(offset, unit);
		// The box's share of the voxel's surroundings under a tent two voxels wide (a voxel's occupancy then starts and ends
		// without a kink as a face passes: weighed over the voxel alone, a probe's light bent once per voxel crossed), the box
		// taken ST_GI_MOVER_INSET of a voxel smaller.
		float weighed = halfExtent - ST_GI_MOVER_INSET * width;
		coverage *= GetTentIntegral((weighed - along) / width) - GetTentIntegral((-weighed - along) / width);
		// How far the voxel's center lies inside the nearer face of the pair (negative: beyond it), in voxels.
		float depth = (halfExtent - abs(along)) / width;
		inside = min(inside, saturate(depth - 1.0));
		touched = touched && depth >= -0.5 - ST_GI_MOVER_SURFACE_SLACK;
		if (depth <= 0.5 + ST_GI_MOVER_SURFACE_SLACK)
		{
			surface = true;
			thin = thin || 2.0 * extent < width;
		}
		summed += unit * (sign(along) * saturate(1.0 - max(depth, 0.0)));
	}
	float occupancy = ST_GI_MOVER_SURFACE_OCCUPANCY * coverage + (1.0 - ST_GI_MOVER_SURFACE_OCCUPANCY) * inside;
	if (coverage <= 0.0 || !surface || !touched)
		return occupancy;
	float summedLength = length(summed);
	if (thin || summedLength < 1e-4)
		normal = float4(0.0, 0.0, 0.0, -1.0);
	else
		normal = float4(summed / summedLength, 1.0);
	return occupancy;
}

[numthreads(ST_GI_MOVER_BRICK_VOXELS, ST_GI_MOVER_BRICK_VOXELS, ST_GI_MOVER_BRICK_VOXELS)]
void main(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID)
{
	if (groupID.x >= u_Pass.BrickCount)
		return;
	VoxelMoverBrick brick = u_Bricks[u_Pass.FirstBrick + groupID.x];
	int3 dimensions = int3(u_GI.Dimensions.xyz);
	int3 local = brick.Local.xyz + int3(threadID);
	if (any(local < int3(0, 0, 0)) || any(local >= dimensions))
		return; // Outside the window the brick's level had: another voxel's texel
	int3 texel = (brick.Texel.xyz + int3(threadID)) % dimensions;
	uint address = ((uint(brick.Local.w) * ST_GI_MOVER_BRICK_VOXELS + threadID.z) * ST_GI_MOVER_BRICK_VOXELS + threadID.y) * ST_GI_MOVER_BRICK_VOXELS + threadID.x;
	address *= 12u;

	if (u_Pass.Restore != 0u)
	{
		o_Material[texel] = UnpackUnorm4x8(o_Backup.Load(address));
		o_Emission[texel] = o_Backup.Load(address + 4u);
		o_Normal[texel] = UnpackSnormBytes(o_Backup.Load(address + 8u));
		return;
	}

	float4 material = o_Material[texel];
	uint emission = o_Emission[texel];
	float4 normal = o_Normal[texel];
	o_Backup.Store(address, PackUnormBytes(material));
	o_Backup.Store(address + 4u, emission);
	o_Backup.Store(address + 8u, PackSnormBytes(normal));

	// The box that covers most of the voxel.
	GIVoxelLevel level = u_GI.Levels[u_Pass.Level];
	float voxel = level.Origin.w;
	float3 position = level.Origin.xyz + (float3(local) + 0.5) * voxel;
	float occupancy = 0.0;
	float4 moverNormal = float4(0.0, 0.0, 0.0, 0.0);
	float3 moverAlbedo = float3(0.0, 0.0, 0.0);
	uint moverEmission = 0u;
	for (uint word = 0u; word < 2u; word++)
	{
		uint bits = brick.Movers[word];
		while (bits != 0u)
		{
			uint index = word * 32u + firstbitlow(bits);
			bits &= bits - 1u;
			VoxelMover mover = u_Movers[index];
			float4 candidateNormal;
			float candidate = GetMoverOccupancy(mover, position, voxel, candidateNormal);
			if ((mover.Flags & ST_VOXEL_MOVER_ALPHA_MASKED) != 0u)
				candidate *= ST_GI_MASKED_OCCUPANCY;
			if (candidate > occupancy)
			{
				occupancy = candidate;
				moverAlbedo = UnpackUnorm4x8(mover.Albedo).rgb;
				moverEmission = mover.Emission;
				moverNormal = candidateNormal;
				if (candidateNormal.w > 0.0 && (mover.Flags & ST_VOXEL_MOVER_DOUBLE_SIDED) != 0u)
					moverNormal.w = 0.5;
			}
		}
	}
	// The static voxel stays where it is at least as full (a floor's solid voxels under a box, a wall the box passes into).
	if (occupancy <= material.a + 0.5 / 255.0)
		return;

	bool moverSurface = moverNormal.w != 0.0;
	float4 written = float4(moverAlbedo, occupancy);
	uint writtenEmission = moverSurface ? moverEmission : 0u;
	float4 writtenNormal = moverNormal;
	if (normal.w > 0.25)
	{
		// The voxel holds a lit surface of its own that is less than full (the layer in front of a face on the grid, a cut-out):
		// it turns into the box's by how far the coverage has passed its occupancy, the normals blended where they do not face
		// against each other (a floor's layer under a box's bottom keeps the floor's until the box covers half of it).
		float share = saturate((occupancy - material.a) / max(ST_GI_MOVER_SURFACE_OCCUPANCY - material.a, 1e-3));
		bool blend = moverNormal.w > 0.25 && dot(normal.xyz, moverNormal.xyz) >= 0.0;
		if (blend)
		{
			written.rgb = lerp(material.rgb, moverAlbedo, share);
			writtenNormal = lerp(normal, moverNormal, share);
		}
		if (share < 0.5)
		{
			writtenEmission = emission;
			if (!blend)
			{
				written.rgb = material.rgb;
				writtenNormal = normal;
			}
		}
	}
	o_Material[texel] = written;
	o_Emission[texel] = writtenEmission;
	o_Normal[texel] = writtenNormal;
}
