#include "Include/Common.slang"

#include "Include/GIMarch.slang"

// One cascade's rays (Docs/DeveloperGuide.md, "Global illumination"), cascades top down from the top to 1 (FieldReduce.comp marches
// cascade 0): a thread per atlas texel (probe.xy * a + direction texel, probe.z), from the probe as ProbeValidity.comp moved it off
// a surface it sat on. Directions are fixed (nothing jitters, nothing blends over frames). The texel is the mean of s x s sub-rays
// spread over it (ProbeCascadeLayout::SubRays): one ray a texel catches a source smaller than the texel whole or not at all (the
// sun room's floor patch, half a texel of cascade 2 seen from the north wall, lit some probes three times over and left their
// neighbours dark). Each ray marches GIMarch.slang's march over [start - lead-in, end + overlap): the lead-in reaches back to the
// probe for every cascade but the first and only absorbs, so a wall between a child's interval and its parents' is crossed by a
// marched side (correction 2); the overlap (ProbeCascadeLayout::Overlap) takes what lies just past the interval's end on the
// child's own line. A dead probe writes zero.
// The miss rule: what a texel's rays leave unabsorbed continues into the next cascade's merged ray (its eight parents by the probe's
// weights, each the mean of the four parent directions this one covers), never into the sky; no visible parent: dark. Only the
// top cascade, marching until it leaves every level, reads the sky. Output: rgb the merged light (scaled luminance), a the merged
// transmittance to the sky.

Texture3D<float4> u_Validity : register(t57);
Texture3D<float4> u_WeightsA : register(t58);
Texture3D<float4> u_WeightsB : register(t59);
Texture3D<float4> u_ParentAtlas : register(t60);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Atlas : register(u0);

float4 Merge(uint3 probe, uint2 texel)
{
	float4 weightsA = u_WeightsA.Load(int4(int3(probe), 0));
	float4 weightsB = u_WeightsB.Load(int4(int3(probe), 0));
	int3 base = int3(floor((float3(probe) - 0.5) * 0.5));
	uint parentDirections = u_Pass.ParentGrid.w;
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	float total = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		float weight = corner < 4u ? weightsA[corner] : weightsB[corner - 4u];
		if (weight <= 0.0)
			continue;
		int3 parent = base + int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int2 first = parent.xy * int(parentDirections) + int2(texel) * 2;
		float4 rays = u_ParentAtlas.Load(int4(first, parent.z, 0)) + u_ParentAtlas.Load(int4(first + int2(1, 0), parent.z, 0))
			+ u_ParentAtlas.Load(int4(first + int2(0, 1), parent.z, 0)) + u_ParentAtlas.Load(int4(first + int2(1, 1), parent.z, 0));
		sum += weight * 0.25 * rays;
		total += weight;
	}
	return total < 1e-4 ? float4(0.0, 0.0, 0.0, 0.0) : sum / total;
}

[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	uint a = u_Pass.DirectionsPerAxis;
	if (any(globalID >= uint3(u_Pass.Grid.xy * a, u_Pass.Grid.z)))
		return;
	uint3 probe = uint3(globalID.xy / a, globalID.z);
	uint2 texel = globalID.xy % a;
	float4 validity = u_Validity.Load(int4(int3(probe), 0));
	if (validity.r <= 0.0)
	{
		o_Atlas[globalID] = float4(0.0, 0.0, 0.0, 0.0);
		return;
	}

	// From the probe as ProbeValidity.comp moved it off a surface it sat on.
	float3 origin = GetProbePosition(probe, u_Pass.Interval.x) + DecodeProbeMove(validity, u_Pass.Interval.x);
	float start = u_Pass.Interval.y;
	float end = u_Pass.Interval.z;
	bool top = end < 0.0;
	uint minimumLevel = u_Pass.MinimumLevel;

	uint subRays = max(u_Pass.SubRays, 1u);
	float4 own = float4(0.0, 0.0, 0.0, 0.0);
	float leftShare = 0.0;
	[loop]
	for (uint sub = 0u; sub < subRays * subRays; sub++)
	{
		// The sub-ray's direction: OctahedralDecode of its point in the texel (GetTileDirection's arithmetic at that point).
		float2 position = (float2(texel) + (float2(sub % subRays, sub / subRays) + 0.5) / float(subRays)) / float(a) * 2.0 - 1.0;
		float3 n = float3(position, 1.0 - abs(position.x) - abs(position.y));
		float fold = max(-n.z, 0.0);
		n.x += n.x >= 0.0 ? -fold : fold;
		n.y += n.y >= 0.0 ? -fold : fold;
		bool left;
		float4 ray = MarchRay(origin, normalize(n), start, end, top, minimumLevel, left);
		own += ray;
		if (left)
			leftShare += ray.a;
	}
	own /= float(subRays * subRays);
	leftShare /= float(subRays * subRays);

	// The top cascade's rays that left every level read the sky (the texel's direction); the others' rest merges the next cascade.
	float4 rest = float4(0.0, 0.0, 0.0, 0.0);
	if (top)
		rest = own.a > 0.0 ? float4(GetSkyRadiance(GetTileDirection(texel, a)), 1.0) * (leftShare / own.a) : float4(0.0, 0.0, 0.0, 0.0);
	else if (own.a > 0.0)
		rest = Merge(probe, texel);
	o_Atlas[globalID] = float4(own.rgb + own.a * rest.rgb, own.a * rest.a);
}
