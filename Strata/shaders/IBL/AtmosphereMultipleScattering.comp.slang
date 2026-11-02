#include "Include/Common.slang"

#include "Include/AtmospherePass.slang"

// The light of the second and higher scattering orders at each height for each height of the sun, per lux of sun, taken as
// arriving equally from every direction (the phase functions are averaged out after the first scattering). At a point, the
// light scattered once toward it (with an even phase) is gathered from a sphere of directions, the ground's reflection of
// the sun included: the second order. The same gathering of a light that is 1 everywhere gives the share f of light that
// scattering returns to the point; every further order is f times the one before, so all of them sum to the second order
// over (1 - f). Computed after the transmittance table, when the atmosphere settings change.

[vk::image_format("rgba16f")] RWTexture2D<float4> o_Table : register(u0);

static const uint c_DirectionRings = 8u;   // Rings of equal solid angle from straight up to straight down...
static const uint c_DirectionSectors = 8u; // ...each split into sectors around the vertical
static const uint c_Steps = 20u;

void st_Main()
{
	int2 texel = int2(st_GlobalInvocationID.xy);
	if (any((texel >= int2(ST_MULTIPLE_SCATTERING_SIZE))))
		return;
	float2 radiusMu = MultipleScatteringUVToRadiusMu((float2(texel) + 0.5) / ST_MULTIPLE_SCATTERING_SIZE);
	float radius = radiusMu.x;
	float3 toSun = float3(sqrt(max(1.0 - radiusMu.y * radiusMu.y, 0.0)), radiusMu.y, 0.0);
	AtmosphereMedium medium = GetMedium();
	float3 albedo = u_Atmosphere.GroundAlbedo.rgb;

	float3 secondOrder = float3(0.0);
	float3 transfer = float3(0.0);
	const float isotropic = 1.0 / (4.0 * PI);
	const float directionWeight = 1.0 / float(c_DirectionRings * c_DirectionSectors); // Each direction's share of the sphere
	for (uint ring = 0u; ring < c_DirectionRings; ring++)
	{
		float mu = 1.0 - 2.0 * (float(ring) + 0.5) / float(c_DirectionRings);
		float horizontal = sqrt(max(1.0 - mu * mu, 0.0));
		for (uint sector = 0u; sector < c_DirectionSectors; sector++)
		{
			float azimuth = TWO_PI * (float(sector) + 0.5) / float(c_DirectionSectors);
			float3 direction = float3(horizontal * cos(azimuth), mu, horizontal * sin(azimuth));
			bool ground = RayMeetsGround(radius, mu);
			float length = ground ? DistanceToGround(radius, mu) : DistanceToAtmosphereTop(radius, mu);
			float step = length / float(c_Steps);
			float3 transmittance = float3(1.0);
			float3 single = float3(0.0);
			float3 gathered = float3(0.0);
			for (uint index = 0u; index < c_Steps; index++)
			{
				float t = (float(index) + 0.5) * step;
				float pointRadius = RadiusAlongRay(radius, mu, t);
				float height = pointRadius - c_AtmosphereBottomRadius;
				float3 scattering = medium.RayleighScattering * exp(-max(height, 0.0) / c_RayleighScaleHeight)
					+ medium.MieScattering * exp(-max(height, 0.0) / c_MieScaleHeight);
				float3 extinction = GetAtmosphereExtinction(medium, height);
				// The point's position and its sun's cosine to its own zenith.
				float3 position = float3(0.0, radius, 0.0) + direction * t;
				float sunMu = dot(position, toSun) / pointRadius;
				// Integrated over the step: what scatters in it, dimmed by what lies before it on the step.
				float3 stepTransmittance = exp(-extinction * step);
				float3 inStep = max(extinction, float3(1.0e-9));
				float3 weight = transmittance * (1.0 - stepTransmittance) / inStep;
				single += weight * scattering * GetSunTransmittance(pointRadius, sunMu) * isotropic;
				gathered += weight * scattering;
				transmittance *= stepTransmittance;
			}
			if (ground)
			{
				// The ground reflects the sun diffusely toward the point.
				float3 groundPosition = float3(0.0, radius, 0.0) + direction * length;
				float3 normal = normalize(groundPosition);
				float sunMu = dot(normal, toSun);
				single += transmittance * GetSunTransmittance(c_AtmosphereBottomRadius, sunMu) * max(sunMu, 0.0) * albedo / PI;
			}
			secondOrder += single * directionWeight;
			transfer += gathered * directionWeight;
		}
	}
	float3 multiple = secondOrder / max(1.0 - transfer, float3(1.0e-3));
	o_Table[texel] = float4(multiple, 1.0);
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
