#ifndef STRATA_REFLECTION_PROBES_SLANG
#define STRATA_REFLECTION_PROBES_SLANG

// Reflection probes (ReflectionProbeComponent, SceneRenderer's probes; DeveloperGuide "Reflections"): the frame's list of
// probes in view, strongest first (priority, then the smaller box), each naming the slot of the renderer's cube array its
// capture was converted into, a prefiltered cube with one level per roughness step and its irradiance as spherical-harmonic
// coefficients. Layouts mirror RenderData::ReflectionProbeBuffer (SceneRenderData.h); keep them in sync.

#include "Include/Scene.slang"
#include "Include/SphericalHarmonics.slang"

#define ST_MAX_REFLECTION_PROBES 16u
#define ST_REFLECTION_PROBE_SH 9u
#define ST_REFLECTION_PROBE_MIPS 6.0

struct ReflectionProbeData
{
	float4 CenterBlend; // xyz: the box's camera-relative center (where it was captured), w: its blend distance (meters)
	float4 AxisX;       // The box's unit axes in world space, w: its half extent along each (meters)
	float4 AxisY;
	float4 AxisZ;
	float4 Parameters;  // x: intensity, y: 1 for box-projected lookups, z: its slot of the cube array
};

struct ReflectionProbeBuffer
{
	// Per slot, the irradiance over pi as coefficients of the real spherical harmonics of bands 0 to 2 (EvaluateProbeIrradiance)
	float4 u_ProbeIrradiance[ST_MAX_REFLECTION_PROBES * ST_REFLECTION_PROBE_SH];
	uint4 u_ProbeCounts; // x: probes in the list
	ReflectionProbeData u_ReflectionProbes[ST_MAX_REFLECTION_PROBES];
};
StructuredBuffer<ReflectionProbeBuffer> st_ReflectionProbeBuffer : register(t10);
#define u_ProbeIrradiance st_ReflectionProbeBuffer[0].u_ProbeIrradiance
#define u_ProbeCounts st_ReflectionProbeBuffer[0].u_ProbeCounts
#define u_ReflectionProbes st_ReflectionProbeBuffer[0].u_ReflectionProbes

#ifdef ST_REFLECTION_PROBE_SAMPLING
// The probes' prefiltered cubes, a cube per slot (the cube array needs the device's imageCubeArray feature, which the
// engine requires), sampled through u_LinearClampSampler.
TextureCubeArray<float4> u_ReflectionProbeCubes : register(t9);

// The irradiance over pi a surface facing `normal` receives from a slot's capture (what the sky's irradiance map holds).
float3 EvaluateProbeIrradiance(uint slot, float3 normal)
{
	float basis[9];
	ProbeSHBasis(normal, basis);
	float3 irradiance = float3(0.0);
	for (uint index = 0u; index < ST_REFLECTION_PROBE_SH; index++)
		irradiance += u_ProbeIrradiance[slot * ST_REFLECTION_PROBE_SH + index].rgb * basis[index];
	return max(irradiance, float3(0.0));
}

// A camera-relative position in the box's own frame (meters along its axes, from its center).
float3 ToProbeBox(ReflectionProbeData probe, float3 relativePosition)
{
	float3 offset = relativePosition - probe.CenterBlend.xyz;
	return float3(dot(offset, probe.AxisX.xyz), dot(offset, probe.AxisY.xyz), dot(offset, probe.AxisZ.xyz));
}

// How much of a probe applies at a position in its box: 1 deeper than its blend distance inside, falling to 0 at its faces
// (0 outside).
float GetProbeWeight(ReflectionProbeData probe, float3 local)
{
	float3 extents = float3(probe.AxisX.w, probe.AxisY.w, probe.AxisZ.w);
	float3 inside = extents - abs(local);
	float depth = min(inside.x, min(inside.y, inside.z));
	if (depth <= 0.0)
		return 0.0;
	return probe.CenterBlend.w > 0.0 ? clamp(depth / probe.CenterBlend.w, 0.0, 1.0) : 1.0;
}

// The direction to look a reflection up in a probe's cube: with box projection, where the reflected ray from the position
// leaves the box, seen from the box's center (where the capture was taken); else the reflected direction itself.
float3 GetProbeLookup(ReflectionProbeData probe, float3 local, float3 reflected)
{
	if (probe.Parameters.y < 0.5)
		return reflected;
	float3 direction = float3(dot(reflected, probe.AxisX.xyz), dot(reflected, probe.AxisY.xyz), dot(reflected, probe.AxisZ.xyz));
	float3 extents = float3(probe.AxisX.w, probe.AxisY.w, probe.AxisZ.w);
	// The far face along each axis the ray moves along (a direction without a component never reaches that axis's faces).
	float3 safe = select((abs(direction) < float3(1e-6)), float3(1e-6), direction);
	float3 toFaces = (sign(safe) * extents - local) / safe;
	float distanceToExit = max(min(toFaces.x, min(toFaces.y, toFaces.z)), 0.0);
	float3 hit = local + direction * distanceToExit;
	return hit.x * probe.AxisX.xyz + hit.y * probe.AxisY.xyz + hit.z * probe.AxisZ.xyz;
}

// The probes' light at a surface: the irradiance over pi and the prefiltered radiance of the two strongest probes whose
// boxes hold the position, the stronger over the weaker by its weight, and their combined weight (what the sky's light
// fills in is 1 minus it). Intensities are applied.
struct ProbeLight
{
	float3 Irradiance;
	float3 Radiance;
	float Weight;
};

ProbeLight EvaluateReflectionProbes(float3 relativePosition, float3 normal, float3 reflected, float roughness)
{
	ProbeLight light;
	light.Irradiance = float3(0.0);
	light.Radiance = float3(0.0);
	light.Weight = 0.0;
	uint count = min(u_ProbeCounts.x, ST_MAX_REFLECTION_PROBES);
	uint found = 0u;
	for (uint index = 0u; index < count && found < 2u; index++)
	{
		ReflectionProbeData probe = u_ReflectionProbes[index];
		float3 local = ToProbeBox(probe, relativePosition);
		float weight = GetProbeWeight(probe, local);
		if (weight <= 0.0)
			continue;
		// The stronger probe takes its weight, the weaker one its weight of what is left.
		float share = weight * (1.0 - light.Weight);
		float slot = probe.Parameters.z;
		float3 lookup = GetProbeLookup(probe, local, reflected);
		float3 radiance = u_ReflectionProbeCubes.SampleLevel(u_LinearClampSampler, float4(lookup, slot), roughness * (ST_REFLECTION_PROBE_MIPS - 1.0)).rgb;
		float3 irradiance = EvaluateProbeIrradiance(uint(slot + 0.5), normal);
		light.Irradiance += irradiance * probe.Parameters.x * share;
		light.Radiance += radiance * probe.Parameters.x * share;
		light.Weight += share;
		found++;
	}
	return light;
}
#endif

#endif
