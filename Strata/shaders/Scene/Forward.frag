#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Scene.glsl"
#include "Include/MaterialTextures.glsl"
#include "Include/PBR.glsl"
#include "Include/Shadows.glsl"
#define ST_LOCAL_SHADOW_SAMPLING
#include "Include/LocalShadows.glsl"
#include "Include/Clusters.glsl"
#include "Include/Fog.glsl"
#define ST_REFLECTION_PROBE_SAMPLING
#include "Include/ReflectionProbes.glsl"

// Forward shading into the HDR target, compiled in these variants (see Strata/CMakeLists.txt):
// - Scene/Forward.frag shades opaque and alpha-masked surfaces after the depth prepass with an EQUAL depth test. The
//   prepass already discarded masked texels, so this variant forces early depth testing and discards only what the
//   prepass's level-of-detail cross-fade mask left out (IsLODFadeDrawn, the same test).
// - Scene/ForwardTransparent.frag (ST_FORWARD_TRANSPARENT) shades alpha-blended surfaces. They are not in the
//   prepass, so the screen-space ambient occlusion (which belongs to the opaque surface behind them) is not applied.
// - Scene/ForwardGlass.frag (ST_FORWARD_TRANSPARENT, ST_FORWARD_GLASS) shades transmissive surfaces, drawn before the blended
//   ones without blending: what lies behind them comes from the scene-color copy (SRV 19), and they write their own depth,
//   entity ids (without ST_NO_ENTITY_IDS: ForwardGlassNoEntityIDs.frag) and, with ST_VELOCITY (ForwardGlassVelocity.frag,
//   ForwardGlassNoEntityIDsVelocity.frag, after Scene/MeshVelocity.vert), their motion, into the attachments after the
//   color, as the prepass does for opaque surfaces. Their HDR alpha is 1: the TAA does not take them for blended surfaces.
// Every variant shades a clear coat where the material has one (a branch on its flag, the same for a whole draw).

#ifndef ST_FORWARD_TRANSPARENT
layout(early_fragment_tests) in;
#endif

layout(location = 0) in vec3 v_RelativePosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent; // w: bitangent sign (already flipped for mirrored instances)
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_InstanceIndex;
layout(location = 5) flat in uint v_LODFade;
#ifdef ST_VELOCITY
layout(location = 6) in vec4 v_CurrentClip;
layout(location = 7) in vec4 v_PreviousClip;
#endif

layout(location = 0) out vec4 o_Color;
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
layout(location = 1) out uint o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
layout(location = 1) out vec2 o_Velocity;
#elif defined(ST_VELOCITY)
layout(location = 2) out vec2 o_Velocity;
#endif

// The frame's HDR image before its transparent surfaces (SceneRenderer::CopySceneColor), pre-exposed, at half resolution with
// mips that blur it further each level.
layout(set = 0, binding = ST_SRV(19)) uniform texture2D u_SceneColor;
#endif

#ifndef ST_FORWARD_TRANSPARENT
// The frame's screen-space reflections (ReflectionParams.x, Effects/ScreenSpaceReflections.comp), at half resolution: a the
// confidence, rgb the reflected luminance, pre-exposed and multiplied by the confidence (sampled bilinearly, a reflection's
// edge then blends with the light it replaces, not with black). Of the opaque surfaces the prepass drew: transparent surfaces
// have none.
layout(set = 0, binding = ST_SRV(18)) uniform texture2D u_ScreenReflections;
#endif

// The share of its diffuse light a surface scatters: transmissive surfaces pass the rest through (the glass variant). The other
// variants scatter all of it, with the arithmetic they had before transmission existed.
vec3 ScatterDiffuse(vec3 diffuse, float diffuseWeight)
{
#ifdef ST_FORWARD_GLASS
	return diffuse * diffuseWeight;
#else
	return diffuse;
#endif
}

// The light a lit surface reflects from one light: the base's, under its clear coat when it has one (the coat reflects some of
// the light before it reaches the base).
vec3 LightSurface(SurfaceData surface, ClearcoatData coat, float diffuseWeight, vec3 lightDirection, vec3 radiance)
{
#ifdef ST_FORWARD_GLASS
	vec3 light = EvaluateLightWeighted(surface, lightDirection, radiance, diffuseWeight);
#else
	vec3 light = EvaluateLight(surface, lightDirection, radiance);
#endif
	if (coat.Strength > 0.0)
	{
		float baseWeight;
		vec3 coatLight = EvaluateClearcoat(coat, surface.View, lightDirection, radiance, baseWeight);
		light = light * baseWeight + coatLight;
	}
	return light;
}

// The specular light arriving along `reflected` at a surface of `roughness` (the clear coat's): the reflection probes' whose
// boxes hold it, and the sky's (prefiltered) where they leave room, or without a sky the constant ambient as a uniform sky.
vec3 SampleSpecularLight(vec3 relativePosition, vec3 normal, vec3 reflected, float roughness, bool environment)
{
	ProbeLight probes;
	probes.Radiance = vec3(0.0);
	probes.Weight = 0.0;
	if (u_ProbeCounts.x > 0u)
		probes = EvaluateReflectionProbes(relativePosition, normal, reflected, roughness);
	vec3 sky = environment ? textureLod(samplerCube(u_PrefilteredMap, u_LinearClampSampler), RotateEnvironmentDirection(reflected),
		roughness * u_Frame.EnvironmentParams.z).rgb * u_Frame.EnvironmentParams.x : u_Frame.AmbientColor.rgb;
	return probes.Radiance + sky * (1.0 - probes.Weight);
}

// The lit surface's color: the sun with its shadow, the punctual lights of the pixel's cluster (those with shadows through
// their views, Include/LocalShadows.glsl), the reflection probes' and the sky's image-based light (or the constant ambient)
// and the emission, under the clear coat where the material has one. `diffuseWeight` is the share of the diffuse light the
// surface scatters (1 - its transmission in the glass variant; 1 elsewhere).
vec3 ShadeLitSurface(vec3 albedo, float baseAlpha, vec3 emissive, vec4 metallicRoughness, vec3 shadingNormal, float occlusion, MaterialData material,
	ClearcoatData coat, float diffuseWeight)
{
	SurfaceData surface;
	surface.Position = v_RelativePosition;
	surface.Normal = shadingNormal;
	surface.View = GetViewVector(v_RelativePosition);
	surface.Albedo = albedo;
	surface.Alpha = baseAlpha;
	surface.Metallic = clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0);
	surface.Roughness = clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0);
	surface.Occlusion = occlusion;
	surface.Emissive = emissive;

	float viewDepth = -(u_Frame.View * vec4(v_RelativePosition, 1.0)).z;
	vec3 color = vec3(0.0);
	if (u_Frame.DirectionalLightDirection.w > 0.0)
	{
		float shadow = 1.0;
		if (u_Frame.ShadowParams.w > 0.0)
		{
			vec3 geometricNormal = normalize(v_Normal) * (gl_FrontFacing ? 1.0 : -1.0);
			shadow = SampleDirectionalShadow(v_RelativePosition, geometricNormal, viewDepth, gl_FragCoord.xy);
		}
		color += LightSurface(surface, coat, diffuseWeight, -u_Frame.DirectionalLightDirection.xyz, u_Frame.DirectionalLightColor.rgb) * shadow;
	}

	// Punctual lights reaching this pixel's cluster.
	if (u_Frame.LightCounts.x > 0u)
	{
		uint clusterBase = GetClusterIndex(gl_FragCoord.xy, viewDepth) * GetClusterStride();
		uint clusterLights = min(u_ClusterLights[clusterBase], u_Frame.ClusterGrid.w);
		for (uint index = 0u; index < clusterLights; index++)
		{
			LightData light = u_Lights[u_ClusterLights[clusterBase + 1u + index]];
			vec3 toLight = light.PositionRange.xyz - surface.Position;
			float distanceSquared = dot(toLight, toLight);
			float range = light.PositionRange.w;
			if (distanceSquared >= range * range)
				continue;

			float attenuation = DistanceAttenuation(distanceSquared, light.Color.w);
			if (uint(light.DirectionType.w) == ST_LIGHT_SPOT)
				attenuation *= SpotAttenuation(-toLight, light.DirectionType.xyz, light.SpotAngles.x, light.SpotAngles.y);
			if (attenuation <= 0.0)
				continue;
			// A shadowed light's slot plus one (SceneRenderer's local shadows).
			uint shadowSlot = uint(light.SpotAngles.z + 0.5);
			if (shadowSlot > 0u)
			{
				vec3 geometricNormal = normalize(v_Normal) * (gl_FrontFacing ? 1.0 : -1.0);
				attenuation *= SampleLocalShadow(shadowSlot - 1u, surface.Position, geometricNormal, gl_FragCoord.xy);
			}
			color += LightSurface(surface, coat, diffuseWeight, normalize(toLight), light.Color.rgb * attenuation);
		}
	}

	// Screen-space occlusion darkens indirect light only (direct light has shadows).
#ifdef ST_FORWARD_TRANSPARENT
	float screenOcclusion = 1.0;
#else
	float screenOcclusion = u_Frame.AOParams.x > 0.0 ? texelFetch(u_AmbientOcclusion, ivec2(gl_FragCoord.xy), 0).r : 1.0;
#endif
	float diffuseOcclusion = surface.Occlusion * screenOcclusion;
	vec3 reflected = reflect(-surface.View, surface.Normal);
	// Reflection probes whose boxes hold the surface light it first (Include/ReflectionProbes.glsl); the sky, or the constant
	// ambient without an environment, fills in what they leave.
	ProbeLight probes;
	probes.Irradiance = vec3(0.0);
	probes.Radiance = vec3(0.0);
	probes.Weight = 0.0;
	if (u_ProbeCounts.x > 0u)
		probes = EvaluateReflectionProbes(surface.Position, surface.Normal, reflected, surface.Roughness);
	// Screen-space reflections replace the probes' and the sky's specular light by their confidence (the scene's luminance as
	// the previous frame showed it, which no intensity of the sky scales).
	vec4 screenReflection = vec4(0.0);
#ifndef ST_FORWARD_TRANSPARENT
	if (u_Frame.ReflectionParams.x > 0.0)
	{
		screenReflection = textureLod(sampler2D(u_ScreenReflections, u_LinearClampSampler), gl_FragCoord.xy * u_Frame.ViewportSize.zw, 0.0);
		screenReflection.rgb /= u_Frame.TemporalParams.y;
	}
#endif
	bool environment = u_Frame.EnvironmentParams.w > 0.0;
	// The base's image-based light (or constant ambient), which the clear coat covers below.
	vec3 indirect;
	if (environment || probes.Weight > 0.0 || screenReflection.a > 0.0)
	{
		// Image-based lighting: diffuse irradiance plus split-sum specular (prefiltered radiance x environment BRDF).
		float NoV = max(dot(surface.Normal, surface.View), 1e-4);
		vec3 f0 = mix(vec3(0.04), surface.Albedo, surface.Metallic);
		vec2 dfg = textureLod(sampler2D(u_BRDFLut, u_LinearClampSampler), vec2(NoV, surface.Roughness), 0.0).rg;
		vec3 specularColor = f0 * dfg.x + dfg.y;
		vec3 irradiance = vec3(0.0);
		vec3 radiance = vec3(0.0);
		if (environment)
		{
			irradiance = textureLod(samplerCube(u_IrradianceMap, u_LinearClampSampler), RotateEnvironmentDirection(surface.Normal), 0.0).rgb;
			radiance = textureLod(samplerCube(u_PrefilteredMap, u_LinearClampSampler), RotateEnvironmentDirection(reflected),
				surface.Roughness * u_Frame.EnvironmentParams.z).rgb;
		}
		// Specular occlusion from ambient occlusion (Lagarde): tighter for smooth surfaces and grazing views.
		float specularOcclusion = clamp(pow(NoV + diffuseOcclusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0);
		if (probes.Weight > 0.0)
		{
			// The probes' captures hold the scene's luminance (their intensities applied); the sky's share is scaled here.
			float sky = 1.0 - probes.Weight;
			irradiance = probes.Irradiance + (environment ? irradiance * u_Frame.EnvironmentParams.x * sky : vec3(0.0));
			radiance = probes.Radiance + radiance * (environment ? u_Frame.EnvironmentParams.x * sky : 0.0);
			if (screenReflection.a > 0.0)
				radiance = radiance * (1.0 - screenReflection.a) + screenReflection.rgb;
			vec3 diffuse = irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - specularColor);
			// The constant ambient's share as the constant ambient lights alone (below: diffuse only, no specular share), so
			// that a probe fading out meets it without a step at its box's faces.
			if (!environment)
				diffuse += u_Frame.AmbientColor.rgb * sky * surface.Albedo * (1.0 - surface.Metallic);
			indirect = ScatterDiffuse(diffuse, diffuseWeight) * diffuseOcclusion + radiance * specularColor * specularOcclusion;
		}
		else if (screenReflection.a > 0.0)
		{
			// Without the sky the constant ambient lights the diffuse part, as it does without reflections (below).
			vec3 diffuse = environment ? irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - specularColor) * u_Frame.EnvironmentParams.x
				: u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic);
			vec3 specular = radiance * (environment ? u_Frame.EnvironmentParams.x : 0.0) * (1.0 - screenReflection.a) + screenReflection.rgb;
			indirect = ScatterDiffuse(diffuse, diffuseWeight) * diffuseOcclusion + specular * specularColor * specularOcclusion;
		}
		else
		{
			vec3 diffuse = irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - specularColor);
			indirect = (ScatterDiffuse(diffuse, diffuseWeight) * diffuseOcclusion + radiance * specularColor * specularOcclusion) * u_Frame.EnvironmentParams.x;
		}
	}
	else
	{
		// Constant ambient without an environment map: diffuse only, modulated by material occlusion.
		indirect = ScatterDiffuse(u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic), diffuseWeight) * diffuseOcclusion;
	}

	vec3 emitted = surface.Emissive;
	if (coat.Strength > 0.0)
	{
		// The coat reflects the probes', the sky's or the ambient light by its prefiltered Fresnel (split sum, as the base), and
		// the light under it, the base's and the emission, loses what it reflects.
		float coatNoV = max(dot(coat.Normal, surface.View), 1e-4);
		vec2 coatDFG = textureLod(sampler2D(u_BRDFLut, u_LinearClampSampler), vec2(coatNoV, coat.Roughness), 0.0).rg;
		float coatReflectance = coat.Strength * (c_ClearcoatF0 * coatDFG.x + coatDFG.y);
		vec3 coatReflected = reflect(-surface.View, coat.Normal);
		vec3 coatRadiance = SampleSpecularLight(surface.Position, coat.Normal, coatReflected, coat.Roughness, environment);
		float coatOcclusion = clamp(pow(coatNoV + diffuseOcclusion, exp2(-16.0 * coat.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0);
		indirect = indirect * (1.0 - coatReflectance) + coatRadiance * coatReflectance * coatOcclusion;
		emitted *= 1.0 - ClearcoatFresnel(coat, coatNoV);
	}
	color += indirect;
	color += emitted;
	return color;
}

#ifdef ST_FORWARD_GLASS
// How much the instance scales lengths, on average over its axes (a material's Thickness is in the mesh's units).
float GetInstanceScale(InstanceData instance)
{
	return (length(TransformDirection(instance, vec3(1.0, 0.0, 0.0))) + length(TransformDirection(instance, vec3(0.0, 1.0, 0.0)))
		+ length(TransformDirection(instance, vec3(0.0, 0.0, 1.0)))) / 3.0;
}

// The light a transmissive surface passes through from behind it: the scene-color copy where the ray refracted by the surface
// (IOR) leaves its volume after Thickness (behind the pixel itself for a thin wall), blurred by the roughness (a coarser
// level), absorbed in the volume on the way (Beer's law) and tinted by the base color, less what the surface reflects. It is
// not fogged again: the copy holds what lies behind with the fog in front of it.
vec3 TransmitLight(InstanceData instance, MaterialData material, vec3 normal, vec3 view, vec3 albedo, float metallic, float roughness, float transmission)
{
	vec3 refracted = refract(-view, normal, 1.0 / material.IOR);
	float thickness = material.Thickness * GetInstanceScale(instance);
	vec3 exitPoint = v_RelativePosition + refracted * thickness;
	vec4 clip = u_Frame.ViewProjection * vec4(exitPoint, 1.0);
	vec2 uv = clip.w > 1e-6 ? NDCToUV(clip.xy / clip.w) : gl_FragCoord.xy * u_Frame.ViewportSize.zw;
	float level = roughness * float(textureQueryLevels(u_SceneColor) - 1);
	vec3 behind = textureLod(sampler2D(u_SceneColor, u_LinearClampSampler), uv, level).rgb / u_Frame.TemporalParams.y;

	float NoV = max(dot(normal, view), 1e-4);
	vec2 dfg = textureLod(sampler2D(u_BRDFLut, u_LinearClampSampler), vec2(NoV, roughness), 0.0).rg;
	vec3 reflectance = mix(vec3(0.04), albedo, metallic) * dfg.x + dfg.y;
	vec3 absorbed = exp(-material.Absorption.rgb * thickness);
	return behind * absorbed * albedo * (1.0 - metallic) * (1.0 - reflectance) * transmission;
}
#endif

void main()
{
	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	vec2 uv = TransformUV(material, v_TexCoord);

	// The surface is shaded before anything is discarded: a quad whose other pixels discarded has no implicit derivatives
	// (texture levels of detail, the shadow's filter) left (Architecture.Shaders).
	vec4 baseColor = material.BaseColor * SampleMaterialTexture(material.BaseColorMap, uv);
	vec3 emissive = material.Emissive.rgb * SampleMaterialTexture(material.EmissiveMap, uv).rgb;
	vec3 color = baseColor.rgb + emissive;
#ifdef ST_FORWARD_GLASS
	vec3 transmitted = vec3(0.0);
#endif
	if ((material.Flags & ST_MATERIAL_UNLIT) == 0u)
	{
		vec4 metallicRoughness = SampleMaterialTexture(material.MetallicRoughnessMap, uv);
		vec3 shadingNormal = GetShadingNormal(material, uv, v_Normal, v_Tangent, gl_FrontFacing);
		float occlusion = mix(1.0, SampleMaterialTexture(material.OcclusionMap, uv).r, material.OcclusionStrength);
		ClearcoatData coat;
		coat.Strength = 0.0;
		coat.Roughness = 0.0;
		coat.Normal = shadingNormal;
		if ((material.Flags & ST_MATERIAL_CLEARCOAT) != 0u)
		{
			coat.Strength = clamp(material.Clearcoat * SampleMaterialTexture(material.ClearcoatMap, uv).r, 0.0, 1.0);
			coat.Roughness = clamp(material.ClearcoatRoughness * SampleMaterialTexture(material.ClearcoatRoughnessMap, uv).g, 0.0, 1.0);
			coat.Normal = GetMappedNormal(material.ClearcoatNormalMap, 1.0, uv, v_Normal, v_Tangent, gl_FrontFacing);
		}
		float diffuseWeight = 1.0;
#ifdef ST_FORWARD_GLASS
		float transmission = clamp(material.Transmission * SampleMaterialTexture(material.TransmissionMap, uv).r, 0.0, 1.0);
		diffuseWeight = 1.0 - transmission;
		float metallic = clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0);
		float roughness = clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0);
		transmitted = TransmitLight(instance, material, shadingNormal, GetViewVector(v_RelativePosition), baseColor.rgb, metallic, roughness, transmission);
		if (coat.Strength > 0.0)
			transmitted *= 1.0 - ClearcoatFresnel(coat, dot(coat.Normal, GetViewVector(v_RelativePosition)));
#endif
		color = ShadeLitSurface(baseColor.rgb, baseColor.a, emissive, metallicRoughness, shadingNormal, occlusion, material, coat, diffuseWeight);
	}

	// Every variant fogs alike, so a transparent surface looks like an opaque one at its depth.
	if (u_Frame.FogParams.x > 0.0)
		color = ApplyHeightFog(color, v_RelativePosition);
#ifdef ST_FORWARD_GLASS
	color += transmitted;
#endif

	// The prepass's mask: of a fading level, only the pixels whose depth it wrote.
	if (!IsLODFadeDrawn(v_LODFade, gl_FragCoord.xy))
		discard;
#ifdef ST_FORWARD_TRANSPARENT
	if ((material.Flags & ST_MATERIAL_ALPHA_MASK) != 0u && baseColor.a < material.AlphaCutoff)
		discard;
#endif
#if defined(ST_FORWARD_TRANSPARENT) && !defined(ST_FORWARD_GLASS)
	float alpha = (material.Flags & ST_MATERIAL_ALPHA_BLEND) != 0u ? baseColor.a : 1.0;
#else
	const float alpha = 1.0;
#endif
	// Pre-exposed (TemporalParams.y, a power of two: exact unless a value overflows or becomes subnormal).
	o_Color = vec4(SanitizeHDR(color * u_Frame.TemporalParams.y), alpha);
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
	o_EntityID = instance.EntityID;
#endif
#ifdef ST_VELOCITY
	o_Velocity = GetClipMotion(v_CurrentClip, v_PreviousClip);
#endif
#endif
}
