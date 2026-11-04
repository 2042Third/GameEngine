#include "Include/Common.slang"

#include "Include/GIMarch.slang"

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

Texture3D<float4> u_Validity : register(t57);
Texture3D<float4> u_WeightsA : register(t58);
Texture3D<float4> u_WeightsB : register(t59);
Texture3D<float4> u_ParentAtlas : register(t60);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Field : register(u0);

#define ST_GI_REDUCE_GROUP 64u

groupshared float4 s_Faces[ST_GI_REDUCE_GROUP][6];

// A tile texel's solid angle (GlobalIllumination::GetSolidAngle): the octahedron's Jacobian 1 / |p|^3 at its center times the
// tile's normalization.
float GetTileSolidAngle(uint2 texel, uint a, float normalization)
{
	float2 encoded = (float2(texel) + 0.5) / float(a) * 2.0 - 1.0;
	float3 p = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-p.z, 0.0);
	p.x += p.x >= 0.0 ? -t : t;
	p.y += p.y >= 0.0 ? -t : t;
	float length3 = dot(p, p) * sqrt(dot(p, p));
	return normalization / length3;
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
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	float total = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		float weight = corner < 4u ? weightsA[corner] : weightsB[corner - 4u];
		if (weight <= 0.0)
			continue;
		int3 parent = base + int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int2 first = parent.xy * int(parentDirections) + int2(texel * children);
		float4 rays = float4(0.0, 0.0, 0.0, 0.0);
		for (uint child = 0u; child < children * children; child++)
			rays += u_ParentAtlas.Load(int4(first + int2(int(child % children), int(child / children)), parent.z, 0));
		sum += weight * rays / float(children * children);
		total += weight;
	}
	return total < 1e-4 ? float4(0.0, 0.0, 0.0, 0.0) : sum / total;
}

[numthreads(ST_GI_REDUCE_GROUP, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint index : SV_GroupIndex)
{
	uint3 probe = groupID;
	float4 faces[6];
	[unroll]
	for (uint face = 0u; face < 6u; face++)
		faces[face] = float4(0.0, 0.0, 0.0, 0.0);
	float4 validity = u_Validity.Load(int4(int3(probe), 0));
	uint a = u_Pass.DirectionsPerAxis;
	if (all(probe < u_Pass.Grid.xyz) && validity.r > 0.0)
	{
		float3 origin = GetProbePosition(probe, u_Pass.Interval.x);
		bool hasParents = u_Pass.ParentGrid.w > 0u;
		[loop]
		for (uint direction = index; direction < a * a; direction += ST_GI_REDUCE_GROUP)
		{
			uint2 texel = uint2(direction % a, direction / a);
			float3 d = GetTileDirection(texel, a);
			bool left;
			float4 ray = MarchRay(origin, d, u_Pass.Interval.y, u_Pass.Interval.z, false, u_Pass.MinimumLevel, left);
			float4 rest = ray.a > 0.0 && hasParents ? MergeParents(probe, texel, a) : float4(0.0, 0.0, 0.0, 0.0);
			float4 value = float4(ray.rgb + ray.a * rest.rgb, ray.a * rest.a) * GetTileSolidAngle(texel, a, u_GI.SolidAngleNorms.x);
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
		float4 sum = float4(0.0, 0.0, 0.0, 0.0);
		for (uint thread = 0u; thread < ST_GI_REDUCE_GROUP; thread++)
			sum += s_Faces[thread][index];
		o_Field[uint3(probe.x * 3u + index % 3u, probe.y * 2u + index / 3u, probe.z)] = sum * u_GI.FaceWeights[index >> 2][index & 3u];
	}
}
