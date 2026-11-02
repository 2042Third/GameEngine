#ifndef STRATA_INDIRECT_LIGHT_SLANG
#define STRATA_INDIRECT_LIGHT_SLANG

// The forward pass's indirect light (Scene/Forward.frag.slang's ShadeLitSurface; Docs/DeveloperGuide.md, "Lighting"): the
// inputs both gathers read, filled once per pixel (GetIndirectInputs in Forward.frag.slang), the diffuse gather, the specular
// gather and the compose step that keeps Strata 0.7.0's four forms bit for bit. Included by Scene/Forward.frag.slang only,
// after Include/PBR.slang and Include/ReflectionProbes.slang with ST_REFLECTION_PROBE_SAMPLING.

// The forms of the base's indirect light, by what lights the pixel.
#define ST_INDIRECT_PROBES  0u // Reflection probes; the sky's share (or the constant ambient's) fills what they leave
#define ST_INDIRECT_SCREEN  1u // Screen-space reflections without probes; the sky's intensity scales inside the gathers
#define ST_INDIRECT_SKY     2u // The sky alone; its intensity (EnvironmentParams.x) scales the composed sum
#define ST_INDIRECT_AMBIENT 3u // The constant ambient alone: diffuse only

// What the gathers share. Streams add fields at the end (wave 8's plan: shared and additive).
struct IndirectInputs
{
	uint Form;                    // ST_INDIRECT_*
	bool Environment;             // EnvironmentParams.w > 0: the sky's image light; else the constant ambient
	float3 SpecularColor;         // The split sum's reflectance, f0 dfg.x + dfg.y, compensated for multiple scattering
	float3 Reflected;             // The view reflected about the shading normal
	float3 GeometricNormal;       // The primitive's normal, facing the viewer
	float DiffuseOcclusion;       // Material occlusion x screen-space occlusion
	float3 DiffuseOcclusionColor; // With the multi-bounce of AOParams.y
	float SpecularOcclusion;      // From DiffuseOcclusion (Lagarde), with the horizon fade; 1 in the ambient form
	ProbeLight Probes;            // The reflection probes at the surface (Weight 0 without probes)
	float4 ScreenReflection;      // rgb the reflected luminance x confidence, unexposed; a the confidence (0: none)
	float Sky;                    // 1 - Probes.Weight: the sky's (or the constant ambient's) share
	float3 SkyIrradiance;         // The sky's irradiance at the shading normal (0 without the environment)
	float3 SkyRadiance;           // The sky's prefiltered radiance along Reflected at the surface's roughness (0 without)
	float DiffuseWeight;          // The share of its diffuse light the surface scatters (glass: 1 - transmission)
};

// The share of its diffuse light a surface scatters: transmissive surfaces pass the rest through (the glass variant). The other
// variants scatter all of it, with the arithmetic they had before transmission existed.
float3 ScatterDiffuse(float3 diffuse, float diffuseWeight)
{
#ifdef ST_FORWARD_GLASS
	return diffuse * diffuseWeight;
#else
	return diffuse;
#endif
}

// The base's diffuse indirect light, scattered and occluded (radiance-cascades' region).
float3 GatherIndirectDiffuse(IndirectInputs inputs, SurfaceData surface)
{
	float3 diffuse;
	if (inputs.Form == ST_INDIRECT_PROBES)
	{
		// The probes' captures hold the scene's luminance (their intensities applied); the sky's share is scaled here.
		float3 irradiance = inputs.Probes.Irradiance + (inputs.Environment ? inputs.SkyIrradiance * u_Frame.EnvironmentParams.x * inputs.Sky : float3(0.0));
		diffuse = irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor);
		// The constant ambient's share as the constant ambient lights alone (diffuse only), so that a probe fading out meets it
		// without a step at its box's faces.
		if (!inputs.Environment)
			diffuse += u_Frame.AmbientColor.rgb * inputs.Sky * surface.Albedo * (1.0 - surface.Metallic);
	}
	else if (inputs.Form == ST_INDIRECT_SCREEN)
	{
		// Without the sky the constant ambient lights the diffuse part, as it does without reflections.
		diffuse = inputs.Environment ? inputs.SkyIrradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor) * u_Frame.EnvironmentParams.x
			: u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic);
	}
	else if (inputs.Form == ST_INDIRECT_SKY)
	{
		diffuse = inputs.SkyIrradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - inputs.SpecularColor);
	}
	else
	{
		// Constant ambient without an environment map: diffuse only, modulated by material occlusion.
		diffuse = u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic);
	}
	return ScatterDiffuse(diffuse, inputs.DiffuseWeight) * inputs.DiffuseOcclusionColor;
}

// The base's specular indirect light, by its reflectance and occlusion (reflections' region). Screen-space reflections replace
// the probes' and the sky's light by their confidence (the scene's luminance as the previous frame showed it, which no
// intensity of the sky scales).
float3 GatherIndirectSpecular(IndirectInputs inputs)
{
	float3 radiance;
	if (inputs.Form == ST_INDIRECT_PROBES)
	{
		radiance = inputs.Probes.Radiance + inputs.SkyRadiance * (inputs.Environment ? u_Frame.EnvironmentParams.x * inputs.Sky : 0.0);
		if (inputs.ScreenReflection.a > 0.0)
			radiance = radiance * (1.0 - inputs.ScreenReflection.a) + inputs.ScreenReflection.rgb;
	}
	else if (inputs.Form == ST_INDIRECT_SCREEN)
	{
		radiance = inputs.SkyRadiance * (inputs.Environment ? u_Frame.EnvironmentParams.x : 0.0) * (1.0 - inputs.ScreenReflection.a) + inputs.ScreenReflection.rgb;
	}
	else if (inputs.Form == ST_INDIRECT_SKY)
	{
		radiance = inputs.SkyRadiance;
	}
	else
	{
		return float3(0.0); // The constant ambient lights the diffuse part only
	}
	return radiance * inputs.SpecularColor * inputs.SpecularOcclusion;
}

// The base's indirect light from the two gathers, each form's scale where 0.7.0 applied it (the sky's intensity on the sum of
// the sky-only form; the clear coat follows in Forward.frag.slang).
float3 ComposeIndirect(IndirectInputs inputs, float3 diffuse, float3 specular)
{
	if (inputs.Form == ST_INDIRECT_AMBIENT)
		return diffuse;
	if (inputs.Form == ST_INDIRECT_SKY)
		return (diffuse + specular) * u_Frame.EnvironmentParams.x;
	return diffuse + specular;
}

#endif
