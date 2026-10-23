#ifndef STRATA_REFLECTION_PROBES_GLSL
#define STRATA_REFLECTION_PROBES_GLSL

// Reflection probes (ReflectionProbeComponent, SceneRenderer's probes; DeveloperGuide "Reflections"): the frame's list of
// probes in view, strongest first (priority, then the smaller box), each naming the slot of the renderer's cube array its
// capture was converted into, a prefiltered cube with one level per roughness step and its irradiance as spherical-harmonic
// coefficients. Layouts mirror RenderData::ReflectionProbeBuffer (SceneRenderData.h); keep them in sync.

#include "Include/Scene.glsl"
#include "Include/SphericalHarmonics.glsl"

#define ST_MAX_REFLECTION_PROBES 16u
#define ST_REFLECTION_PROBE_SH 9u
#define ST_REFLECTION_PROBE_MIPS 6.0

struct ReflectionProbeData
{
	vec4 CenterBlend; // xyz: the box's camera-relative center (where it was captured), w: its blend distance (meters)
	vec4 AxisX;       // The box's unit axes in world space, w: its half extent along each (meters)
	vec4 AxisY;
	vec4 AxisZ;
	vec4 Parameters;  // x: intensity, y: 1 for box-projected lookups, z: its slot of the cube array
};

layout(set = 0, binding = ST_SRV(10), std430) readonly buffer ReflectionProbeBuffer
{
	// Per slot, the irradiance over pi as coefficients of the real spherical harmonics of bands 0 to 2 (EvaluateProbeIrradiance)
	vec4 u_ProbeIrradiance[ST_MAX_REFLECTION_PROBES * ST_REFLECTION_PROBE_SH];
	uvec4 u_ProbeCounts; // x: probes in the list
	ReflectionProbeData u_ReflectionProbes[ST_MAX_REFLECTION_PROBES];
};

#ifdef ST_REFLECTION_PROBE_SAMPLING
// The probes' prefiltered cubes, a cube per slot (the cube array needs the device's imageCubeArray feature, which the
// engine requires), sampled through u_LinearClampSampler.
layout(set = 0, binding = ST_SRV(9)) uniform textureCubeArray u_ReflectionProbeCubes;

// The irradiance over pi a surface facing `normal` receives from a slot's capture (what the sky's irradiance map holds).
vec3 EvaluateProbeIrradiance(uint slot, vec3 normal)
{
	float basis[9];
	ProbeSHBasis(normal, basis);
	vec3 irradiance = vec3(0.0);
	for (uint index = 0u; index < ST_REFLECTION_PROBE_SH; index++)
		irradiance += u_ProbeIrradiance[slot * ST_REFLECTION_PROBE_SH + index].rgb * basis[index];
	return max(irradiance, vec3(0.0));
}

// A camera-relative position in the box's own frame (meters along its axes, from its center).
vec3 ToProbeBox(ReflectionProbeData probe, vec3 relativePosition)
{
	vec3 offset = relativePosition - probe.CenterBlend.xyz;
	return vec3(dot(offset, probe.AxisX.xyz), dot(offset, probe.AxisY.xyz), dot(offset, probe.AxisZ.xyz));
}

// How much of a probe applies at a position in its box: 1 deeper than its blend distance inside, falling to 0 at its faces
// (0 outside).
float GetProbeWeight(ReflectionProbeData probe, vec3 local)
{
	vec3 extents = vec3(probe.AxisX.w, probe.AxisY.w, probe.AxisZ.w);
	vec3 inside = extents - abs(local);
	float depth = min(inside.x, min(inside.y, inside.z));
	if (depth <= 0.0)
		return 0.0;
	return probe.CenterBlend.w > 0.0 ? clamp(depth / probe.CenterBlend.w, 0.0, 1.0) : 1.0;
}

// The direction to look a reflection up in a probe's cube: with box projection, where the reflected ray from the position
// leaves the box, seen from the box's center (where the capture was taken); else the reflected direction itself.
vec3 GetProbeLookup(ReflectionProbeData probe, vec3 local, vec3 reflected)
{
	if (probe.Parameters.y < 0.5)
		return reflected;
	vec3 direction = vec3(dot(reflected, probe.AxisX.xyz), dot(reflected, probe.AxisY.xyz), dot(reflected, probe.AxisZ.xyz));
	vec3 extents = vec3(probe.AxisX.w, probe.AxisY.w, probe.AxisZ.w);
	// The far face along each axis the ray moves along (a direction without a component never reaches that axis's faces).
	vec3 safe = mix(direction, vec3(1e-6), lessThan(abs(direction), vec3(1e-6)));
	vec3 toFaces = (sign(safe) * extents - local) / safe;
	float distanceToExit = max(min(toFaces.x, min(toFaces.y, toFaces.z)), 0.0);
	vec3 hit = local + direction * distanceToExit;
	return hit.x * probe.AxisX.xyz + hit.y * probe.AxisY.xyz + hit.z * probe.AxisZ.xyz;
}

// The probes' light at a surface: the irradiance over pi and the prefiltered radiance of the two strongest probes whose
// boxes hold the position, the stronger over the weaker by its weight, and their combined weight (what the sky's light
// fills in is 1 minus it). Intensities are applied.
struct ProbeLight
{
	vec3 Irradiance;
	vec3 Radiance;
	float Weight;
};

ProbeLight EvaluateReflectionProbes(vec3 relativePosition, vec3 normal, vec3 reflected, float roughness)
{
	ProbeLight light;
	light.Irradiance = vec3(0.0);
	light.Radiance = vec3(0.0);
	light.Weight = 0.0;
	uint count = min(u_ProbeCounts.x, ST_MAX_REFLECTION_PROBES);
	uint found = 0u;
	for (uint index = 0u; index < count && found < 2u; index++)
	{
		ReflectionProbeData probe = u_ReflectionProbes[index];
		vec3 local = ToProbeBox(probe, relativePosition);
		float weight = GetProbeWeight(probe, local);
		if (weight <= 0.0)
			continue;
		// The stronger probe takes its weight, the weaker one its weight of what is left.
		float share = weight * (1.0 - light.Weight);
		float slot = probe.Parameters.z;
		vec3 lookup = GetProbeLookup(probe, local, reflected);
		vec3 radiance = textureLod(samplerCubeArray(u_ReflectionProbeCubes, u_LinearClampSampler), vec4(lookup, slot),
			roughness * (ST_REFLECTION_PROBE_MIPS - 1.0)).rgb;
		vec3 irradiance = EvaluateProbeIrradiance(uint(slot + 0.5), normal);
		light.Irradiance += irradiance * probe.Parameters.x * share;
		light.Radiance += radiance * probe.Parameters.x * share;
		light.Weight += share;
		found++;
	}
	return light;
}
#endif

#endif
