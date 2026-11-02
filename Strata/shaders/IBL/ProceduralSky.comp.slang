#include "Include/Common.slang"

#include "Include/IBL.slang"

// Writes mip 0 of the environment cube from a procedural sky (SkyLightSource::Procedural): above the horizon a
// gradient from the horizon color to the zenith color, below it a quick blend into the ground color, and the sun's
// disk. The cube then goes through the same downsampling, irradiance and prefiltering as an environment map.

[vk::image_format("rgba16f")] RWTexture2DArray<float4> o_Output : register(u0);

// RenderData::ProceduralSkyParameters
struct Parameters
{
	float4 ZenithColor;
	float4 HorizonColor;
	float4 GroundColor;
	float4 SunDirection; // xyz: toward the sun, w: angular radius in radians
	float4 SunRadiance;
	uint4 Size;        // x: face size in texels
};
[[vk::push_constant]] ConstantBuffer<Parameters> u_Parameters;

float3 SkyRadiance(float3 direction, float texelAngle)
{
	float height = clamp(direction.y, -1.0, 1.0);
	float3 color;
	if (height >= 0.0)
	{
		// The horizon color is haze near the horizon: it gives way to the zenith color within about 15 degrees.
		float zenith = 1.0 - pow(1.0 - height, 8.0);
		color = lerp(u_Parameters.HorizonColor.rgb, u_Parameters.ZenithColor.rgb, zenith);
	}
	else
	{
		// Below the horizon the ground takes over within a few degrees.
		float ground = 1.0 - pow(1.0 + height, 16.0);
		color = lerp(u_Parameters.HorizonColor.rgb, u_Parameters.GroundColor.rgb, ground);
	}

	// The sun's disk, its edge smoothed over one texel. The angle comes from the chord between the directions, which
	// stays precise for the small angles involved (acos of a dot product near 1 does not).
	float radius = u_Parameters.SunDirection.w;
	if (radius > 0.0)
	{
		float chord = length(direction - u_Parameters.SunDirection.xyz);
		float angle = 2.0 * asin(min(chord * 0.5, 1.0));
		float coverage = clamp((radius - angle) / texelAngle + 0.5, 0.0, 1.0);
		color = lerp(color, u_Parameters.SunRadiance.rgb, coverage);
	}
	return color;
}

void st_Main()
{
	uint3 id = st_GlobalInvocationID;
	uint size = u_Parameters.Size.x;
	if (id.x >= size || id.y >= size)
		return;
	float3 direction = CubeTexelDirection(id.xy, id.z, size);
	// A texel spans 2 / size of a face at unit distance: its angle at the face's center, smaller toward the corners.
	float texelAngle = 2.0 / float(size);
	float3 color = SkyRadiance(direction, texelAngle);
	// Finite and within half-float range, like the environment maps (EquirectToCube.comp).
	color = clamp(color, float3(0.0), float3(65000.0));
	o_Output[int3(id)] = float4(color, 1.0);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
