#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/MaterialTextures.slang"
#include "Include/PBR.slang"
#include "Include/Shadows.slang"
#define ST_LOCAL_SHADOW_SAMPLING
#include "Include/LocalShadows.slang"
#include "Include/Clusters.slang"
#include "Include/Fog.slang"
#include "Include/AerialPerspective.slang"
#include "Include/AmbientOcclusionFit.slang"
#include "Include/GIField.slang"
#define ST_REFLECTION_PROBE_SAMPLING
#include "Include/ReflectionProbes.slang"
#include "Include/IndirectLight.slang"

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
#define ST_EARLY_FRAGMENT_TESTS [earlydepthstencil]
#endif

static float3 v_RelativePosition;
static float3 v_Normal;
static float4 v_Tangent; // w: bitangent sign (already flipped for mirrored instances)
static float2 v_TexCoord;
static uint v_InstanceIndex;
static uint v_LODFade;
#ifdef ST_VELOCITY
static float4 v_CurrentClip;
static float4 v_PreviousClip;
#endif

static float4 o_Color;
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
static uint o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
static float2 o_Velocity;
#elif defined(ST_VELOCITY)
static float2 o_Velocity;
#endif

// The frame's HDR image before its transparent surfaces (SceneRenderer::CopySceneColor), pre-exposed, at half resolution with
// mips that blur it further each level.
Texture2D<float4> u_SceneColor : register(t19);
#endif

#ifndef ST_FORWARD_TRANSPARENT
// The frame's screen-space reflections (ReflectionParams.x, Effects/ScreenSpaceReflections.comp), at half resolution: a the
// confidence, rgb the reflected luminance, pre-exposed and multiplied by the confidence (sampled bilinearly, a reflection's
// edge then blends with the light it replaces, not with black). Of the opaque surfaces the prepass drew: transparent surfaces
// have none.
Texture2D<float4> u_ScreenReflections : register(t18);
// The sun's screen-space shadow mask (Effects/ScreenShadows.frag): red the square root of the sun's visibility at the pixel.
Texture2D<float4> u_ScreenShadows : register(t28);
#endif

// The frame's lighting debug view (ST_LIGHTING_VIEW_*, SceneRenderOptions::LightingView): the same for every pixel.
uint GetLightingView()
{
	return uint(u_Frame.MaterialParams.y + 0.5);
}

// The views whose values are not light (occlusion, albedo): written unexposed, without fog or what glass transmits.
bool IsLightingValueView(uint view)
{
	return view == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION || view == ST_LIGHTING_VIEW_ALBEDO || view == ST_LIGHTING_VIEW_OPEN_SHARE;
}

// The light a lit surface reflects from one light: the base's, under its clear coat when it has one (the coat reflects some of
// the light before it reaches the base). A reflection probe's capture (MaterialParams.w 1: SceneRenderOptions::ProbeCapture)
// keeps the lights' diffuse light only: their highlights depend on where they are seen from, and looked up from elsewhere they
// showed as bright patches in the wrong places. `sourceAlpha2` widens the highlights for the light's size (Include/PBR.slang).
float3 LightSurface(SurfaceData surface, ClearcoatData coat, float diffuseWeight, float3 lightDirection, float3 radiance, float sourceAlpha2)
{
	float specularWeight = 1.0 - u_Frame.MaterialParams.w;
#ifdef ST_FORWARD_GLASS
	float3 light = EvaluateLightWeighted(surface, lightDirection, radiance, diffuseWeight, specularWeight, sourceAlpha2);
#else
	float3 light = EvaluateLightWeighted(surface, lightDirection, radiance, 1.0, specularWeight, sourceAlpha2);
#endif
	if (coat.Strength > 0.0)
	{
		float baseWeight;
		float3 coatLight = EvaluateClearcoat(coat, surface.View, lightDirection, radiance, sourceAlpha2, baseWeight);
		light = light * baseWeight + coatLight * specularWeight;
	}
	return light;
}

// The specular light arriving along `reflected` at a surface of `roughness` (the clear coat's): the reflection probes' whose
// boxes hold it, and the sky's (prefiltered) where they leave room, or without a sky the constant ambient as a uniform sky.
float3 SampleSpecularLight(float3 relativePosition, float3 normal, float3 reflected, float roughness, bool environment)
{
	ProbeLight probes;
	probes.Radiance = float3(0.0);
	probes.Weight = 0.0;
	if (u_ProbeCounts.x > 0u)
		probes = EvaluateReflectionProbes(relativePosition, normal, reflected, roughness);
	float3 sky = environment ? u_PrefilteredMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(reflected), roughness * u_Frame.EnvironmentParams.z).rgb * u_Frame.EnvironmentParams.x : u_Frame.AmbientColor.rgb;
	return probes.Radiance + sky * (1.0 - probes.Weight);
}

// The indirect light's inputs at the pixel (Include/IndirectLight.slang), filled once.
IndirectInputs GetIndirectInputs(SurfaceData surface, float3 f0, float2 dfg, float NoV, float3 geometricNormal, float2 fragCoord,
	float diffuseOcclusion, float3 diffuseOcclusionColor, float diffuseWeight)
{
	IndirectInputs inputs;
	inputs.DiffuseOcclusion = diffuseOcclusion;
	inputs.DiffuseOcclusionColor = diffuseOcclusionColor;
	inputs.DiffuseWeight = diffuseWeight;
	inputs.GeometricNormal = geometricNormal;
	inputs.Reflected = reflect(-surface.View, surface.Normal);
	// Image-based lighting: diffuse irradiance plus split-sum specular (prefiltered radiance x environment BRDF, compensated
	// for multiple scattering); diffuse gets what specular leaves.
	inputs.SpecularColor = (f0 * dfg.x + dfg.y) * surface.SpecularEnergy;
	// Reflection probes whose boxes hold the surface light it first (Include/ReflectionProbes.slang); the sky, or the constant
	// ambient without an environment, fills in what they leave.
	inputs.Probes.Irradiance = float3(0.0);
	inputs.Probes.Radiance = float3(0.0);
	inputs.Probes.Weight = 0.0;
	if (u_ProbeCounts.x > 0u)
		inputs.Probes = EvaluateReflectionProbes(surface.Position, surface.Normal, inputs.Reflected, surface.Roughness);
	inputs.Sky = 1.0 - inputs.Probes.Weight;
	// Screen-space reflections replace the probes' and the sky's specular light by their confidence.
	inputs.ScreenReflection = float4(0.0);
#ifndef ST_FORWARD_TRANSPARENT
	if (u_Frame.ReflectionParams.x > 0.0)
	{
		inputs.ScreenReflection = u_ScreenReflections.SampleLevel(u_LinearClampSampler, fragCoord * u_Frame.ViewportSize.zw, 0.0);
		inputs.ScreenReflection.rgb /= u_Frame.TemporalParams.y;
	}
#endif
	inputs.Environment = u_Frame.EnvironmentParams.w > 0.0;
	inputs.SkyIrradiance = float3(0.0);
	inputs.SkyRadiance = float3(0.0);
	if (inputs.Environment)
	{
		inputs.SkyIrradiance = u_IrradianceMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(surface.Normal), 0.0).rgb;
		inputs.SkyRadiance = u_PrefilteredMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(inputs.Reflected),
			surface.Roughness * u_Frame.EnvironmentParams.z).rgb;
	}
	// Strata 0.7.0's branch order: probes first, then screen-space reflections, then the sky, else the constant ambient.
	inputs.Form = inputs.Probes.Weight > 0.0 ? ST_INDIRECT_PROBES : inputs.ScreenReflection.a > 0.0 ? ST_INDIRECT_SCREEN
		: inputs.Environment ? ST_INDIRECT_SKY : ST_INDIRECT_AMBIENT;
	// Specular occlusion from ambient occlusion (Lagarde): tighter for smooth surfaces and grazing views; reflections a normal
	// map turns into the surface fade out (GetHorizonFade).
	inputs.SpecularOcclusion = inputs.Form == ST_INDIRECT_AMBIENT ? 1.0
		: clamp(pow(NoV + diffuseOcclusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0)
		* GetHorizonFade(inputs.Reflected, geometricNormal);
	// The same on the material's occlusion alone, for what screen-space reflections found (exactly 1 without an occlusion map).
	inputs.ScreenReflectionOcclusion = clamp(pow(NoV + surface.Occlusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + surface.Occlusion, 0.0, 1.0);
	inputs.FieldWeight = 0.0;
	inputs.FieldIrradiance = float3(0.0);
	inputs.FieldOpen = 1.0;
	return inputs;
}

// The GI's views (SceneLightingView::GlobalIllumination, OpenShare): inside the field its diffuse light on a white surface (absolute
// luminance, under the diffuse occlusion as the join will carry it) or the share of the sky it sees; beyond it, blended over the
// field's fade, today's diffuse light on white (every form of Include/IndirectLight.slang) or 1 (today's light counts all open).
// `field` and `openShare` are SampleIrradianceField's at the surface (ShadeLitSurface samples the field once for these views
// and the join).
float3 ShadeGlobalIlluminationView(uint lightingView, SurfaceData surface, float4 field, float openShare, float3 geometricNormal, float3 f0, float2 dfg,
	float NoV, float diffuseOcclusion, float diffuseWeight)
{
	if (lightingView == ST_LIGHTING_VIEW_OPEN_SHARE)
		return float3(openShare * field.a + (1.0 - field.a));
	SurfaceData white = surface;
	white.Albedo = float3(1.0);
	white.Metallic = 0.0;
	IndirectInputs inputs = GetIndirectInputs(white, f0, dfg, NoV, geometricNormal, st_FragCoord.xy, diffuseOcclusion, float3(diffuseOcclusion), diffuseWeight);
	inputs.SpecularColor = float3(0.0);
	float3 today = ComposeIndirect(inputs, GatherIndirectDiffuse(inputs, white), float3(0.0));
	return field.rgb * (diffuseOcclusion / ST_GI_RADIANCE_SCALE) * field.a + today * (1.0 - field.a);
}

// The lit surface's color: the sun with its shadow, the punctual lights of the pixel's cluster (those with shadows through
// their views, Include/LocalShadows.slang), the reflection probes' and the sky's image-based light (or the constant ambient)
// and the emission, under the clear coat where the material has one. `roughness` is the perceptual roughness widened by
// specular anti-aliasing (main); `diffuseWeight` is the share of the diffuse light the surface scatters (1 - its transmission
// in the glass variant; 1 elsewhere). The lighting debug views Indirect and AmbientOcclusion return only the indirect light and
// the occlusion that darkens it.
float3 ShadeLitSurface(float3 albedo, float baseAlpha, float3 emissive, float4 metallicRoughness, float roughness, float3 shadingNormal, float occlusion,
	MaterialData material, ClearcoatData coat, float diffuseWeight)
{
	SurfaceData surface;
	surface.Position = v_RelativePosition;
	surface.Normal = shadingNormal;
	surface.View = GetViewVector(v_RelativePosition);
	surface.Albedo = albedo;
	surface.Alpha = baseAlpha;
	surface.Metallic = clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0);
	surface.Roughness = roughness;
	surface.Occlusion = occlusion;
	surface.Emissive = emissive;
	// The split sum's BRDF table at the surface: its specular reflectance of image light and the compensation of its lobe for
	// multiple scattering, which direct light takes too.
	float NoV = max(dot(surface.Normal, surface.View), 1e-4);
	// A dielectric reflects head-on what its IOR gives (MaterialGPU::GetDielectricF0: 0.04 at 1.5), a metal its albedo.
	float3 f0 = lerp(float3(material.Absorption.w), surface.Albedo, surface.Metallic);
	surface.F0 = f0;
	float2 dfg = u_BRDFLut.SampleLevel(u_LinearClampSampler, float2(NoV, surface.Roughness), 0.0).rg;
	surface.SpecularEnergy = GetSpecularEnergy(f0, dfg);
	float coatNoV = 1.0;
	float2 coatDFG = float2(1.0, 0.0);
	if (coat.Strength > 0.0)
	{
		coatNoV = max(dot(coat.Normal, surface.View), 1e-4);
		coatDFG = u_BRDFLut.SampleLevel(u_LinearClampSampler, float2(coatNoV, coat.Roughness), 0.0).rg;
		coat.SpecularEnergy = GetSpecularEnergy(float3(c_ClearcoatF0), coatDFG).x;
	}

	float viewDepth = -(mul(u_Frame.View, float4(v_RelativePosition, 1.0))).z;
	float3 color = float3(0.0);
	if (u_Frame.DirectionalLightDirection.w > 0.0)
	{
		float shadow = 1.0;
		if (u_Frame.ShadowParams.w > 0.0)
		{
#ifdef ST_FORWARD_TRANSPARENT
			// Transparent surfaces are not in the prepass: the same filter, per pixel (no blur, no contact shadows).
			float3 geometricNormal = normalize(v_Normal) * (st_FrontFacing ? 1.0 : -1.0);
			shadow = SampleDirectionalShadow(v_RelativePosition, geometricNormal, viewDepth, st_FragCoord.xy);
#else
			float encoded = u_ScreenShadows.Load(int3(int2(st_FragCoord.xy), 0)).r;
			shadow = encoded * encoded;
#endif
		}
		// The sun's highlight takes its disk's size (ReflectionParams.w: RenderData::GetSunLobeAlpha2).
		color += LightSurface(surface, coat, diffuseWeight, -u_Frame.DirectionalLightDirection.xyz, u_Frame.DirectionalLightColor.rgb,
			u_Frame.ReflectionParams.w) * shadow;
	}

	// Punctual lights reaching this pixel's cluster.
	if (u_Frame.LightCounts.x > 0u)
	{
		uint clusterBase = GetClusterIndex(st_FragCoord.xy, viewDepth) * GetClusterStride();
		uint clusterLights = min(u_ClusterLights[clusterBase], u_Frame.ClusterGrid.w);
		for (uint index = 0u; index < clusterLights; index++)
		{
			LightData light = u_Lights[u_ClusterLights[clusterBase + 1u + index]];
			float3 toLight = light.PositionRange.xyz - surface.Position;
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
				float3 geometricNormal = normalize(v_Normal) * (st_FrontFacing ? 1.0 : -1.0);
				attenuation *= SampleLocalShadow(shadowSlot - 1u, surface.Position, geometricNormal, st_FragCoord.xy);
			}
			// The highlight takes the bulb's or flame's angular size (SpotAngles.w: its radius; 0, a point, skips it).
			float sourceAlpha2 = GetSourceLobeAlpha2(light.SpotAngles.w, distanceSquared);
			color += LightSurface(surface, coat, diffuseWeight, normalize(toLight), light.Color.rgb * attenuation, sourceAlpha2);
		}
	}
	uint lightingView = GetLightingView();
	if (lightingView == ST_LIGHTING_VIEW_INDIRECT)
		color = float3(0.0);

	// Screen-space occlusion darkens indirect light only (direct light has shadows).
#ifdef ST_FORWARD_TRANSPARENT
	float screenOcclusion = 1.0;
#else
	float screenOcclusion = u_Frame.AOParams.x > 0.0 ? u_AmbientOcclusion.Load(int3(int2(st_FragCoord.xy), 0)).r : 1.0;
#endif
	float diffuseOcclusion = surface.Occlusion * screenOcclusion;
	if (lightingView == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION)
		return float3(diffuseOcclusion);
	float3 surfaceGeometricNormal = normalize(v_Normal) * (st_FrontFacing ? 1.0 : -1.0);
	// The field, sampled once for the GI views and the join (its gather's loops compiled in once, correction 19), only where one
	// of them reads it: global illumination joined (the scene asked, the field is used; ReflectionParams.y) or a GI view.
	bool globalIlluminationView = lightingView == ST_LIGHTING_VIEW_GLOBAL_ILLUMINATION || lightingView == ST_LIGHTING_VIEW_OPEN_SHARE;
	float4 field = float4(0.0, 0.0, 0.0, 0.0);
	float openShare = 1.0;
	if (u_Frame.ReflectionParams.y > 0.0 || globalIlluminationView)
		field = SampleIrradianceField(u_Samplers[0], surface.Position, surfaceGeometricNormal, surface.Normal, openShare);
	if (globalIlluminationView)
		return ShadeGlobalIlluminationView(lightingView, surface, field, openShare, surfaceGeometricNormal, f0, dfg, NoV, diffuseOcclusion, diffuseWeight);
	// Multi-bounce (AOParams.y): occluded corners keep the light their walls, of the surface's diffuse albedo, reflect into
	// them (Include/AmbientOcclusionFit.slang). Diffuse light only; specular occlusion keeps the visibility.
	float3 diffuseOcclusionColor = u_Frame.AOParams.y > 0.0 ? AOMultiBounce(diffuseOcclusion, surface.Albedo * (1.0 - surface.Metallic))
		: float3(diffuseOcclusion);
	IndirectInputs indirectInputs = GetIndirectInputs(surface, f0, dfg, NoV, surfaceGeometricNormal, st_FragCoord.xy, diffuseOcclusion,
		diffuseOcclusionColor, diffuseWeight);
	// The base's image-based light (or constant ambient), which the clear coat covers below.
	float3 indirect;
	if (u_Frame.ReflectionParams.y > 0.0)
	{
		// Global illumination joined (the scene asked, the field is used; .z the intensity): inside the field its diffuse light
		// replaces today's (box probes, the sky or the constant ambient), blended over the field's fade; specular stays today's.
		// Where the field's weight is 0 this is today's light exactly (x * 1 and + 0).
		indirectInputs.FieldWeight = field.a;
		indirectInputs.FieldIrradiance = field.rgb * (u_Frame.ReflectionParams.z / ST_GI_RADIANCE_SCALE);
		indirectInputs.FieldOpen = openShare;
		indirect = ComposeIndirect(indirectInputs, GatherIndirectDiffuse(indirectInputs, surface) * (1.0 - indirectInputs.FieldWeight),
			GatherIndirectSpecular(indirectInputs)) + GatherIndirectField(indirectInputs, surface) * indirectInputs.FieldWeight;
	}
	else
		indirect = ComposeIndirect(indirectInputs, GatherIndirectDiffuse(indirectInputs, surface), GatherIndirectSpecular(indirectInputs));

	float3 emitted = lightingView == ST_LIGHTING_VIEW_INDIRECT ? float3(0.0) : surface.Emissive;
	if (coat.Strength > 0.0)
	{
		// The coat reflects the probes', the sky's or the ambient light by its prefiltered Fresnel (split sum, as the base), and
		// the light under it, the base's and the emission, loses what it reflects.
		float coatReflectance = coat.Strength * (c_ClearcoatF0 * coatDFG.x + coatDFG.y) * coat.SpecularEnergy;
		float3 coatReflected = reflect(-surface.View, coat.Normal);
		float3 coatRadiance = SampleSpecularLight(surface.Position, coat.Normal, coatReflected, coat.Roughness, indirectInputs.Environment);
		float coatOcclusion = clamp(pow(coatNoV + diffuseOcclusion, exp2(-16.0 * coat.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0)
			* GetHorizonFade(coatReflected, indirectInputs.GeometricNormal);
		indirect = indirect * (1.0 - coatReflectance) + coatRadiance * coatReflectance * coatOcclusion;
		emitted *= 1.0 - ClearcoatFresnel(coat, coatNoV);
	}
	color += indirect;
	color += emitted;
	return color;
}

#ifdef ST_FORWARD_GLASS
// How much the instance scales distances along the surface's world normal `normal` (unit): a material's Thickness is in the
// mesh's units, measured along its normal. Planes `t` apart along a mesh normal are `t * |M^T normal|` apart in the world (M the
// instance's world matrix, whose rows the instance holds), so a box scaled to a thin wall is as thick as the wall.
float GetScaleAlongNormal(InstanceData instance, float3 normal)
{
	return length(normal.x * instance.WorldRows[0].xyz + normal.y * instance.WorldRows[1].xyz + normal.z * instance.WorldRows[2].xyz);
}

// The light a transmissive surface passes through from behind it: the scene-color copy where the ray refracted by the surface
// (IOR) leaves its volume after Thickness (behind the pixel itself for a thin wall), blurred by the roughness (a coarser
// level), absorbed in the volume on the way (Beer's law) and tinted by the base color, less what the surface reflects. It is
// not fogged again: the copy holds what lies behind with the fog in front of it.
float3 TransmitLight(InstanceData instance, MaterialData material, float3 normal, float3 view, float3 albedo, float metallic, float roughness, float transmission)
{
	float3 refracted = refract(-view, normal, 1.0 / material.IOR);
	float thickness = material.Thickness * GetScaleAlongNormal(instance, normalize(v_Normal));
	float3 exitPoint = v_RelativePosition + refracted * thickness;
	float4 clip = mul(u_Frame.ViewProjection, float4(exitPoint, 1.0));
	float2 uv = clip.w > 1e-6 ? NDCToUV(clip.xy / clip.w) : st_FragCoord.xy * u_Frame.ViewportSize.zw;
	float level = roughness * float(TextureLevels(u_SceneColor) - 1);
	float3 behind = u_SceneColor.SampleLevel(u_LinearClampSampler, uv, level).rgb / u_Frame.TemporalParams.y;

	float NoV = max(dot(normal, view), 1e-4);
	float2 dfg = u_BRDFLut.SampleLevel(u_LinearClampSampler, float2(NoV, roughness), 0.0).rg;
	// What the surface reflects, compensated as its specular light is (ShadeLitSurface): the rest passes through.
	float3 f0 = lerp(float3(material.Absorption.w), albedo, metallic);
	float3 reflectance = (f0 * dfg.x + dfg.y) * GetSpecularEnergy(f0, dfg);
	float3 absorbed = exp(-material.Absorption.rgb * thickness);
	return behind * absorbed * albedo * (1.0 - metallic) * (1.0 - reflectance) * transmission;
}
#endif

void st_Main()
{
	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	float2 uv = TransformUV(material, v_TexCoord);

	// The surface is shaded before anything is discarded: a quad whose other pixels discarded has no implicit derivatives
	// (texture levels of detail, the shadow's filter) left (Architecture.Shaders).
	float4 baseColor = material.BaseColor * SampleMaterialTexture(material.BaseColorMap, uv);
	float3 emissive = material.Emissive.rgb * SampleMaterialTexture(material.EmissiveMap, uv).rgb;
	float3 color = baseColor.rgb + emissive;
	// The lighting debug view: Lighting shades every lit surface white without its emission (unlit ones draw as they are).
	uint lightingView = GetLightingView();
	bool valueView = IsLightingValueView(lightingView);
	bool whiteView = lightingView == ST_LIGHTING_VIEW_LIGHTING || lightingView == ST_LIGHTING_VIEW_GLOBAL_ILLUMINATION;
	float3 albedo = whiteView ? float3(1.0) : baseColor.rgb;
	float3 surfaceEmissive = whiteView ? float3(0.0) : emissive;
#ifdef ST_FORWARD_GLASS
	float3 transmitted = float3(0.0);
#endif
	if ((material.Flags & ST_MATERIAL_UNLIT) == 0u)
	{
		float4 metallicRoughness = SampleMaterialTexture(material.MetallicRoughnessMap, uv);
		float3 shadingNormal = GetShadingNormal(material, uv, v_Normal, v_Tangent, st_FrontFacing);
		// Specular anti-aliasing (MaterialParams.z): the lobe widened by the shading normal's change across the pixel. Derivatives
		// only of the primitive's interpolants and textures, in a branch on its material (the same for a whole quad), before any
		// discard: both draw paths shade alike.
		float roughness = WidenRoughness(clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0), ddx(shadingNormal), ddy(shadingNormal),
			u_Frame.MaterialParams.z);
		float occlusion = lerp(1.0, SampleMaterialTexture(material.OcclusionMap, uv).r, material.OcclusionStrength);
		ClearcoatData coat;
		coat.Strength = 0.0;
		coat.Roughness = 0.0;
		coat.Normal = shadingNormal;
		coat.SpecularEnergy = 1.0;
		if ((material.Flags & ST_MATERIAL_CLEARCOAT) != 0u)
		{
			coat.Strength = clamp(material.Clearcoat * SampleMaterialTexture(material.ClearcoatMap, uv).r, 0.0, 1.0);
			coat.Roughness = clamp(material.ClearcoatRoughness * SampleMaterialTexture(material.ClearcoatRoughnessMap, uv).g, 0.0, 1.0);
			coat.Normal = GetMappedNormal(material.ClearcoatNormalMap, 1.0, uv, v_Normal, v_Tangent, st_FrontFacing);
			coat.Roughness = WidenRoughness(coat.Roughness, ddx(coat.Normal), ddy(coat.Normal), u_Frame.MaterialParams.z);
		}
		float diffuseWeight = 1.0;
#ifdef ST_FORWARD_GLASS
		float transmission = clamp(material.Transmission * SampleMaterialTexture(material.TransmissionMap, uv).r, 0.0, 1.0);
		diffuseWeight = 1.0 - transmission;
		float metallic = clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0);
		transmitted = TransmitLight(instance, material, shadingNormal, GetViewVector(v_RelativePosition), albedo, metallic, roughness, transmission);
		if (coat.Strength > 0.0)
			transmitted *= 1.0 - ClearcoatFresnel(coat, dot(coat.Normal, GetViewVector(v_RelativePosition)));
#endif
		color = ShadeLitSurface(albedo, baseColor.a, surfaceEmissive, metallicRoughness, roughness, shadingNormal, occlusion, material, coat,
			diffuseWeight);
	}
	else if (lightingView == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION || lightingView == ST_LIGHTING_VIEW_OPEN_SHARE)
	{
		color = float3(1.0); // Nothing occludes the light of an unlit surface
	}
	else if (lightingView == ST_LIGHTING_VIEW_INDIRECT || lightingView == ST_LIGHTING_VIEW_GLOBAL_ILLUMINATION)
	{
		color = float3(0.0); // Its emission is not indirect light
	}
	if (lightingView == ST_LIGHTING_VIEW_ALBEDO)
		color = baseColor.rgb;

	// Every variant hazes and fogs alike, so a transparent surface looks like an opaque one at its depth: the air's haze
	// first (aerial perspective), then the height fog, nearer the camera, over it. The value views show the surface's own
	// values.
	if (u_Frame.FogParams.z > 0.0 && !valueView)
		color = ApplyAerialPerspective(color, v_RelativePosition, st_FragCoord.xy);
	if (u_Frame.FogParams.x > 0.0 && !valueView)
		color = ApplyHeightFog(color, v_RelativePosition);
#ifdef ST_FORWARD_GLASS
	if (!valueView)
		color += transmitted;
#endif

	// The prepass's mask: of a fading level, only the pixels whose depth it wrote.
	if (!IsLODFadeDrawn(v_LODFade, st_FragCoord.xy))
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
	// Pre-exposed (TemporalParams.y, a power of two: exact unless a value overflows or becomes subnormal), but for the value
	// views: a sunlit scene's pre-exposure (2^-19) would make an albedo of 0.03 subnormal in half floats.
	o_Color = float4(SanitizeHDR(valueView ? color : color * u_Frame.TemporalParams.y), alpha);
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
	o_EntityID = instance.EntityID;
#endif
#ifdef ST_VELOCITY
	o_Velocity = GetClipMotion(v_CurrentClip, v_PreviousClip);
#endif
#endif
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 v_RelativePosition : TEXCOORD0;
	[[vk::location(1)]] float3 v_Normal : TEXCOORD1;
	[[vk::location(2)]] float4 v_Tangent : TEXCOORD2;
	[[vk::location(3)]] float2 v_TexCoord : TEXCOORD3;
	[[vk::location(4)]] nointerpolation uint v_InstanceIndex : TEXCOORD4;
	[[vk::location(5)]] nointerpolation uint v_LODFade : TEXCOORD5;
#ifdef ST_VELOCITY
	[[vk::location(6)]] float4 v_CurrentClip : TEXCOORD6;
	[[vk::location(7)]] float4 v_PreviousClip : TEXCOORD7;
#endif
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
	[[vk::location(1)]] uint o_EntityID : SV_Target1;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
	[[vk::location(1)]] float2 o_Velocity : SV_Target1;
#elif defined(ST_VELOCITY)
	[[vk::location(2)]] float2 o_Velocity : SV_Target2;
#endif
#endif
};
#ifdef ST_EARLY_FRAGMENT_TESTS
ST_EARLY_FRAGMENT_TESTS
#endif
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_RelativePosition = input.v_RelativePosition;
	v_Normal = input.v_Normal;
	v_Tangent = input.v_Tangent;
	v_TexCoord = input.v_TexCoord;
	v_InstanceIndex = input.v_InstanceIndex;
	v_LODFade = input.v_LODFade;
#ifdef ST_VELOCITY
	v_CurrentClip = input.v_CurrentClip;
	v_PreviousClip = input.v_PreviousClip;
#endif
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
#ifdef ST_FORWARD_GLASS
#ifndef ST_NO_ENTITY_IDS
	output.o_EntityID = o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
	output.o_Velocity = o_Velocity;
#elif defined(ST_VELOCITY)
	output.o_Velocity = o_Velocity;
#endif
#endif
	return output;
}
