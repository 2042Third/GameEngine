#include "Include/Common.slang"

#include "Include/GIMarch.slang"
#include "Include/SphericalHarmonics.slang"

// Cascade 0 and the irradiance field (Docs/DeveloperGuide.md, "Global illumination"): a group per cascade-0 probe, a thread per
// direction of its tile (cascade 1's directions: the layout gives cascade 0 as many). Each thread marches its own ray over cascade
// 0's interval (GI/CascadeMarch.comp's march), continues it into cascade 1's merged ray of the same direction (the probe's
// weights over its eight parents, as CascadeMarch merges) and adds the result, times the direction's solid angle and its cosine
// to each face's axis, to the probe's ambient cube; the group sums the faces. Six faces (+X, -X, +Y, -Y, +Z, -Z) at
// (3x + f % 3, 2y + f / 3, z): rgb irradiance over pi (the radiance of a white Lambertian surface facing the face's axis), a the
// share of the sky the face sees, each normalized by its directions' cosine sum (C_f: a uniform radiance gives every face exactly
// that radiance). Every direction is gated by its own ray: a separate reduce of a 16-direction cascade 0 gated four of cascade
// 1's directions at once by one coarse ray (a wall beside the probe hid light its other directions reach, about a fifth of a
// room's bounce). A dead probe writes zero.
// The probe's visibility for the forward gather (DDGI's depth moments, Majercik et al. 2019; ideas only): each ray's distance
// to its first hit over cascade 0's march (the absorbed shares' mean, what passes counted at the march's end) as its mean and
// variance (meters), filtered over neighbouring rays, a texel of an ST_GI_FIELD_DEPTH_TEXELS octahedral tile per ray at
// (probe.xy * texels + texel, probe.z): the interior is the rays' own 8 x 8 directions, the border repeats it across the
// octahedron's edges (GetDepthTileSource), and the gather's bilinear read blends neighbouring texels as a mixture. Fixed
// directions, nothing over time: a still scene gives the same bits every frame. A dead probe writes zero (it is never read).
// The probe's irradiance as drawn (the scene passes read it; re-injection keeps the faces): the same rays projected on the real
// spherical harmonics of bands 0-2 (Include/SphericalHarmonics.slang's order), each band times the cosine lobe's factor over pi
// (1, 2/3, 1/4), a coefficient at (3x + c % 3, 3y + c / 3, z) (rgb irradiance over pi, a the sky share): at a normal the sum
// of the coefficients times the basis is the radiance of a white Lambertian surface facing it. Band 0's texel holds the
// coefficient times its basis function already, the rays' mean (the read gives it the weight one): the coefficient is 3.545
// times the mean and left the format's range where the mean does not. The 64 directions' quadrature is not exact beyond band
// 1: what a uniform radiance (the rays' mean) would leave in a coefficient is taken out, so a uniform radiance gives every
// normal exactly that radiance. The faces blend axis lobes for a normal between the axes, which take directions behind its
// plane: beside a white sphere under a uniform sky, probes a few centimeters off it read its rim 7% dark
// (Include/GIField.slang).
// What follows the march is `precise` (every product, sum and quotient rounded as written): the same rays must give the same
// bits in every dispatch, because a camera's field is compared with a fresh renderer's to the byte. Left free, one driver
// (NVIDIA 621.1) ran these sums in two ways from dispatch to dispatch, a unit in the last place apart: a face of one probe and
// the harmonics of some 180. `precise` does not reach into a called function, a dot or a square root: what feeds the sums
// declares it where it is computed (GetTileSolidAngle, MergeParents, ProbeSHBasisPrecise). The march and the octahedral mapping,
// shared with GI/CascadeMarch.comp, are as they were: what they give never differed.

Texture3D<float4> u_Validity : register(t57);
Texture3D<float4> u_WeightsA : register(t58);
Texture3D<float4> u_WeightsB : register(t59);
Texture3D<float4> u_ParentAtlas : register(t60);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Field : register(u0);
// Two halves a texel (PackHalf2x16): RG16F is an extended storage format, which devices may lack.
[vk::image_format("r32ui")] RWTexture3D<uint> o_Depth : register(u1);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Irradiance : register(u2);

#define ST_GI_REDUCE_GROUP 64u

groupshared float4 s_Faces[ST_GI_REDUCE_GROUP][6];
// Each direction's distance moments: its mean and mean square (ST_GI_FIELD_DIRECTIONS directions, the layout's 8 x 8).
groupshared float2 s_Distances[ST_GI_FIELD_DIRECTIONS];
// Each direction itself, for the filter (computed once per group, not per texel and ray).
groupshared float3 s_Directions[ST_GI_FIELD_DIRECTIONS];
// Each direction's light (rgb, a the sky share) times its solid angle, its solid angle and its harmonics, for the coefficients'
// sums. The direction's own thread evaluates the basis, once: the group's other threads wait while the nine that sum run their
// loop, so what it does per direction is paid as the whole group's time.
groupshared float4 s_Rays[ST_GI_FIELD_DIRECTIONS];
groupshared float s_SolidAngles[ST_GI_FIELD_DIRECTIONS];
groupshared float s_Basis[ST_GI_FIELD_DIRECTIONS][9];
// The depth tile's interior texels as written (PackHalf2x16), for its border to repeat.
groupshared uint s_Depths[(ST_GI_FIELD_DEPTH_TEXELS - 2) * (ST_GI_FIELD_DEPTH_TEXELS - 2)];

// A tile texel's solid angle (GlobalIllumination::GetSolidAngle): the octahedron's Jacobian 1 / |p|^3 at its center times the
// tile's normalization.
float GetTileSolidAngle(uint2 texel, uint a, float normalization)
{
	float2 encoded = (float2(texel) + 0.5) / float(a) * 2.0 - 1.0;
	float3 p = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-p.z, 0.0);
	p.x += p.x >= 0.0 ? -t : t;
	p.y += p.y >= 0.0 ? -t : t;
	precise float squared = (p.x * p.x + p.y * p.y) + p.z * p.z;
	precise float length3 = squared * sqrt(squared);
	precise float solidAngle = normalization / length3;
	return solidAngle;
}

// A texel of the depth tile's one-texel border by its index: the top row, the bottom row, then the left and the right column
// between them (4 * ST_GI_FIELD_DEPTH_TEXELS - 4 texels).
uint2 GetDepthTileBorderTexel(uint index)
{
	uint size = ST_GI_FIELD_DEPTH_TEXELS;
	if (index < size)
		return uint2(index, 0u);
	if (index < 2u * size)
		return uint2(index - size, size - 1u);
	uint side = index - 2u * size;
	return side < size - 2u ? uint2(0u, side + 1u) : uint2(size - 1u, side - (size - 2u) + 1u);
}

// Cascade 1's merged light along cascade 0's texel: the parents' rays of the texel's directions (one, or the mean of the 2 x 2 a
// finer parent tile holds), weighted.
float4 MergeParents(uint3 probe, uint2 texel, uint a)
{
	float4 weightsA = u_WeightsA.Load(int4(int3(probe), 0));
	float4 weightsB = u_WeightsB.Load(int4(int3(probe), 0));
	int3 base = int3(floor((float3(probe) - 0.5) * 0.5));
	uint parentDirections = u_Pass.ParentGrid.w;
	uint children = max(parentDirections / a, 1u);
	precise float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	precise float total = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		float weight = corner < 4u ? weightsA[corner] : weightsB[corner - 4u];
		if (weight <= 0.0)
			continue;
		int3 parent = base + int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int2 first = parent.xy * int(parentDirections) + int2(texel * children);
		precise float4 rays = float4(0.0, 0.0, 0.0, 0.0);
		for (uint child = 0u; child < children * children; child++)
			rays += u_ParentAtlas.Load(int4(first + int2(int(child % children), int(child / children)), parent.z, 0));
		sum += weight * rays / float(children * children);
		total += weight;
	}
	precise float4 merged = total < 1e-4 ? float4(0.0, 0.0, 0.0, 0.0) : sum / total;
	return merged;
}

[numthreads(ST_GI_REDUCE_GROUP, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint index : SV_GroupIndex)
{
	uint3 probe = groupID;
	precise float4 faces[6];
	[unroll]
	for (uint face = 0u; face < 6u; face++)
		faces[face] = float4(0.0, 0.0, 0.0, 0.0);
	float4 validity = u_Validity.Load(int4(int3(probe), 0));
	uint a = u_Pass.DirectionsPerAxis;
	bool live = all(probe < u_Pass.Grid.xyz) && validity.r > 0.0;
	// What passes the march counts at its end: farther than any point the gather asks about (a cell's diagonal, sqrt(3)
	// spacings, plus the view bias).
	float missDistance = u_Pass.Interval.z;
	if (index < ST_GI_FIELD_DIRECTIONS)
	{
		s_Distances[index] = float2(missDistance, missDistance * missDistance);
		s_Rays[index] = float4(0.0, 0.0, 0.0, 0.0);
		s_SolidAngles[index] = 0.0;
	}
	if (live)
	{
		// From the probe as ProbeValidity.comp moved it off a surface it sat on.
		float3 origin = GetProbePosition(probe, u_Pass.Interval.x) + DecodeProbeMove(validity, u_Pass.Interval.x);
		bool hasParents = u_Pass.ParentGrid.w > 0u;
		[loop]
		for (uint direction = index; direction < a * a; direction += ST_GI_REDUCE_GROUP)
		{
			uint2 texel = uint2(direction % a, direction / a);
			float3 d = GetTileDirection(texel, a);
			bool left;
			float3 hit;
			float4 ray = MarchRay(origin, d, u_Pass.Interval.y, u_Pass.Interval.z, false, u_Pass.MinimumLevel, left, hit);
			if (direction < ST_GI_FIELD_DIRECTIONS)
			{
				s_Directions[direction] = d;
				float passed = max(1.0 - hit.z, 0.0);
				s_Distances[direction] = (hit.xy + passed * float2(missDistance, missDistance * missDistance)) / max(hit.z + passed, 1e-6);
			}
			float4 rest = ray.a > 0.0 && hasParents ? MergeParents(probe, texel, a) : float4(0.0, 0.0, 0.0, 0.0);
			float solidAngle = GetTileSolidAngle(texel, a, u_GI.SolidAngleNorms.x);
			precise float4 value = float4(ray.rgb + ray.a * rest.rgb, ray.a * rest.a) * solidAngle;
			if (direction < ST_GI_FIELD_DIRECTIONS)
			{
				s_Rays[direction] = value;
				s_SolidAngles[direction] = solidAngle;
				float basis[9];
				ProbeSHBasisPrecise(d, basis);
				[unroll]
				for (uint coefficient = 0u; coefficient < 9u; coefficient++)
					s_Basis[direction][coefficient] = basis[coefficient];
			}
			faces[0] += value * max(d.x, 0.0);
			faces[1] += value * max(-d.x, 0.0);
			faces[2] += value * max(d.y, 0.0);
			faces[3] += value * max(-d.y, 0.0);
			faces[4] += value * max(d.z, 0.0);
			faces[5] += value * max(-d.z, 0.0);
		}
	}
	[unroll]
	for (uint face = 0u; face < 6u; face++)
		s_Faces[index][face] = faces[face];
	GroupMemoryBarrierWithGroupSync();
	if (index < 6u && all(probe < u_Pass.Grid.xyz))
	{
		precise float4 sum = float4(0.0, 0.0, 0.0, 0.0);
		for (uint thread = 0u; thread < ST_GI_REDUCE_GROUP; thread++)
			sum += s_Faces[thread][index];
		precise float4 face = sum * u_GI.FaceWeights[index >> 2][index & 3u];
		o_Field[uint3(probe.x * 3u + index % 3u, probe.y * 2u + index / 3u, probe.z)] = face;
	}
	// The irradiance's coefficients: a thread each, over every direction (none of a dead probe, whose threads wrote no
	// direction: it writes zero).
	if (index >= 6u && index < 15u && all(probe < u_Pass.Grid.xyz))
	{
		uint coefficient = index - 6u;
		uint directions = live ? min(a * a, ST_GI_FIELD_DIRECTIONS) : 0u;
		precise float4 sum = float4(0.0, 0.0, 0.0, 0.0);
		precise float4 integral = float4(0.0, 0.0, 0.0, 0.0);
		precise float uniformResponse = 0.0;
		precise float total = 0.0;
		for (uint direction = 0u; direction < directions; direction++)
		{
			float basis = s_Basis[direction][coefficient];
			sum += basis * s_Rays[direction];
			integral += s_Rays[direction];
			uniformResponse += basis * s_SolidAngles[direction];
			total += s_SolidAngles[direction];
		}
		// Band 0 integrates a uniform radiance exactly; the others take out what the quadrature leaves of it: the basis function's
		// own sum over the directions (its response to a radiance of one, zero in the continuum) times the rays' mean.
		if (coefficient > 0u)
			sum -= uniformResponse / max(total, 1e-6) * integral;
		// Band 0 is stored times its basis function (0.282095: the texel then holds the rays' mean over the sphere, and the read
		// takes it with weight one). The coefficient itself is 3.545 times that mean, and where the voxels hold radiance near
		// their clamp (ST_GI_MAX_RADIANCE: a bright closed room after its bounces) it left RGBA16F's range: the write stored the
		// format's largest value or an infinity, as the driver chose, for every pixel around the probe to read. The other bands
		// stay inside for any radiance up to the clamp (band 1 at most 1.02 times it, band 2 0.38, in the continuum); the clamp
		// below holds what the 64 directions' quadrature may add to that: a coefficient at the format's largest value is a
		// little wrong and finite.
		float lobe = coefficient == 0u ? 0.282095 : coefficient < 4u ? 2.0 / 3.0 : 0.25;
		precise float4 irradiance = clamp(sum * lobe, -65504.0, 65504.0);
		o_Irradiance[uint3(probe.x * 3u + coefficient % 3u, probe.y * 3u + coefficient / 3u, probe.z)] = irradiance;
	}
	// The depth tile. Its interior is the rays' a x a (the layout's 8 x 8: a thread a texel); its border repeats interior texels
	// across the octahedron's edges, so those are copied from what the group computed: filtering the border's 36 texels like the
	// interior's 64 ran the loop over every ray a second time in 36 of the group's threads, the others waiting.
	bool inside = all(probe < u_Pass.Grid.xyz);
	uint interior = ST_GI_FIELD_DEPTH_TEXELS - 2u;
	[loop]
	for (uint texelIndex = index; texelIndex < interior * interior; texelIndex += ST_GI_REDUCE_GROUP)
	{
		uint2 source = uint2(texelIndex % interior, texelIndex / interior);
		float2 depth = float2(0.0, 0.0);
		if (live)
		{
			// The texel's ray, its moments filtered with its neighbours' by the cosine between them to
			// ST_GI_FIELD_DEPTH_SHARPNESS (about half for the next ray, a thirtieth for the one after): a ray that grazes a
			// surface's voxel staircase alone sets no edge.
			float3 axis = GetTileDirection(source, a);
			float3 sum = float3(0.0, 0.0, 0.0);
			uint directions = min(a * a, ST_GI_FIELD_DIRECTIONS);
			for (uint direction = 0u; direction < directions; direction++)
			{
				// max(0, cos)^8 (ST_GI_FIELD_DEPTH_SHARPNESS) by three squarings.
				float weight = max(dot(s_Directions[direction], axis), 0.0);
				weight *= weight;
				weight *= weight;
				weight *= weight;
				sum += weight * float3(s_Distances[direction], 1.0);
			}
			float2 moments = sum.xy / max(sum.z, 1e-6);
			depth = float2(moments.x, max(moments.y - moments.x * moments.x, 0.0));
		}
		uint halves = PackHalf2x16(depth);
		s_Depths[texelIndex] = halves;
		if (inside)
			o_Depth[uint3(probe.xy * ST_GI_FIELD_DEPTH_TEXELS + source + 1u, probe.z)] = halves;
	}
	GroupMemoryBarrierWithGroupSync();
	if (!inside)
		return;
	[loop]
	for (uint border = index; border < 4u * interior + 4u; border += ST_GI_REDUCE_GROUP)
	{
		uint2 texel = GetDepthTileBorderTexel(border);
		uint2 source = GetDepthTileSource(texel) - 1u;
		o_Depth[uint3(probe.xy * ST_GI_FIELD_DEPTH_TEXELS + texel, probe.z)] = s_Depths[source.y * interior + source.x];
	}
}
