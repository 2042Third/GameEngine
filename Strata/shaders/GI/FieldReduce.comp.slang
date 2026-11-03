#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// The irradiance field (Docs/DeveloperGuide.md, "Global illumination"): a thread per cascade-0 probe reduces its light to an
// ambient cube, six faces (+X, -X, +Y, -Y, +Z, -Z) at (3x + f % 3, 2y + f / 3, z): rgb irradiance over pi (the radiance of a white
// Lambertian surface facing the face's axis), a the share of the sky the face sees. Cascade 0's 16 rays hold their own near
// light and transmittance (unmerged); what lies beyond them comes from cascade 1, merged here in each of its 64 directions (the
// probe's weights over its eight parents, as CascadeMarch merges), so the cosine weighs the far light at cascade 1's angular
// resolution: 16 directions alone put a 2 m wall's light at their centers' cosines, up to twice its form factor. Each part is
// normalized by its own directions' cosine sum (C_f: a uniform radiance gives every face exactly that radiance). A dead probe
// writes zero.

Texture3D<float4> u_Validity : register(t57);
Texture3D<float4> u_WeightsA : register(t58);
Texture3D<float4> u_WeightsB : register(t59);
Texture3D<float4> u_Atlas : register(t60);
Texture3D<float4> u_ParentAtlas : register(t61);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Field : register(u0);

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

void AddToFaces(inout float4 faces[6], float3 direction, float4 value)
{
	faces[0] += value * max(direction.x, 0.0);
	faces[1] += value * max(-direction.x, 0.0);
	faces[2] += value * max(direction.y, 0.0);
	faces[3] += value * max(-direction.y, 0.0);
	faces[4] += value * max(direction.z, 0.0);
	faces[5] += value * max(-direction.z, 0.0);
}

[numthreads(4, 4, 4)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	if (any(globalID >= u_Pass.Grid.xyz))
		return;
	float4 near[6];
	float4 far[6];
	[unroll]
	for (uint face = 0u; face < 6u; face++)
	{
		near[face] = float4(0.0, 0.0, 0.0, 0.0);
		far[face] = float4(0.0, 0.0, 0.0, 0.0);
	}
	if (u_Validity.Load(int4(int3(globalID), 0)).r > 0.0)
	{
		// The parents and their weights (ProbeWeights.comp's corners).
		float4 weightsA = u_WeightsA.Load(int4(int3(globalID), 0));
		float4 weightsB = u_WeightsB.Load(int4(int3(globalID), 0));
		int3 base = int3(floor((float3(globalID) - 0.5) * 0.5));
		float total = dot(weightsA, float4(1.0, 1.0, 1.0, 1.0)) + dot(weightsB, float4(1.0, 1.0, 1.0, 1.0));
		uint parentDirections = u_Pass.ParentGrid.w;
		[loop]
		for (uint index = 0u; index < 16u; index++)
		{
			uint2 texel = uint2(index & 3u, index >> 2);
			float4 ray = u_Atlas.Load(int4(int2(globalID.xy * 4u + texel), int(globalID.z), 0));
			AddToFaces(near, GetTileDirection(texel, 4u), float4(ray.rgb, 0.0) * GetTileSolidAngle(texel, 4u, u_GI.SolidAngleNorms.x));
			if (ray.a <= 0.0 || total < 1e-4)
				continue;
			// The four cascade-1 directions this one covers, each merged over the parents.
			[loop]
			for (uint child = 0u; child < 4u; child++)
			{
				uint2 subTexel = texel * 2u + uint2(child & 1u, child >> 1);
				float4 merged = float4(0.0, 0.0, 0.0, 0.0);
				[loop]
				for (uint corner = 0u; corner < 8u; corner++)
				{
					float weight = corner < 4u ? weightsA[corner] : weightsB[corner - 4u];
					if (weight <= 0.0)
						continue;
					int3 parent = base + int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
					merged += weight * u_ParentAtlas.Load(int4(parent.xy * int(parentDirections) + int2(subTexel), parent.z, 0));
				}
				AddToFaces(far, GetTileDirection(subTexel, parentDirections), merged * (ray.a / total * GetTileSolidAngle(subTexel, parentDirections, u_GI.SolidAngleNorms.y)));
			}
		}
	}
	[unroll]
	for (uint face = 0u; face < 6u; face++)
	{
		float4 value = near[face] * u_GI.FaceWeights[face >> 2][face & 3u] + far[face] * u_GI.FaceWeightsFar[face >> 2][face & 3u];
		o_Field[uint3(globalID.x * 3u + face % 3u, globalID.y * 2u + face / 3u, globalID.z)] = value;
	}
}
