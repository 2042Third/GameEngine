#include "Include/Common.slang"

#include "Include/AtmospherePass.slang"

// The sky seen from the viewer (Include/Atmosphere.glsl's sky-view table), per lux of sun of the sun's color: along each
// direction, the sun's light scattered toward the viewer once (Rayleigh and Mie phase functions, the sun's transmittance
// from the transmittance table, zero in the planet's shadow) and that of all higher orders (the multiple-scattering table),
// dimmed by what lies between, and where the ray meets the ground the ground's diffuse reflection of the sun. Steps grow
// with the square of their index, small near the viewer where the air is densest. Computed when the sun's direction, the
// viewer's height band or the atmosphere settings change; the background and the environment cube read it.

[vk::image_format("rgba16f")] RWTexture2D<float4> o_Table : register(u0);

static const uint c_Steps = 40u;

void st_Main()
{
	int2 texel = int2(st_GlobalInvocationID.xy);
	if (any((texel >= int2(ST_SKY_VIEW_SIZE))))
		return;
	float3 direction = SkyViewUVToDirection((float2(texel) + 0.5) / ST_SKY_VIEW_SIZE);
	float radius = u_Atmosphere.Viewer.x;
	float mu = direction.y; // The viewer's zenith is the world's +Y
	float3 toSun = u_Atmosphere.SunDirection.xyz;
	AtmosphereMedium medium = GetMedium();

	bool ground = RayMeetsGround(radius, mu);
	float length = ground ? DistanceToGround(radius, mu) : DistanceToAtmosphereTop(radius, mu);
	float cosTheta = dot(direction, toSun);
	float rayleighPhase = RayleighPhase(cosTheta);
	float miePhase = MiePhase(cosTheta);
	float3 transmittance = float3(1.0);
	float3 light = float3(0.0);
	float previous = 0.0;
	for (uint index = 0u; index < c_Steps; index++)
	{
		float next = length * float((index + 1u) * (index + 1u)) / float(c_Steps * c_Steps);
		float step = next - previous;
		float t = 0.5 * (previous + next);
		previous = next;
		AccumulateAtmosphereSegment(medium, radius, mu, direction, toSun, rayleighPhase, miePhase, t, step, transmittance, light);
	}
	if (ground)
	{
		float3 groundPosition = float3(0.0, radius, 0.0) + direction * length;
		float sunMu = dot(normalize(groundPosition), toSun);
		light += transmittance * GetSunTransmittance(c_AtmosphereBottomRadius, sunMu) * max(sunMu, 0.0) * u_Atmosphere.GroundAlbedo.rgb / PI;
	}
	// Finite and within half floats, like every sky.
	o_Table[texel] = float4(clamp(light * u_Atmosphere.SunColor.rgb, float3(0.0), float3(65000.0)), 1.0);
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
