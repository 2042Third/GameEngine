#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Scene.glsl"
#include "Include/PBR.glsl"
#include "Include/Shadows.glsl"
#include "Include/Clusters.glsl"

// Forward shading into the HDR target, compiled in two variants (see Strata/CMakeLists.txt):
// - Scene/Forward.frag shades opaque and alpha-masked surfaces after the depth prepass with an EQUAL depth test. The
//   prepass already discarded masked texels, so this variant never discards and forces early depth testing.
// - Scene/ForwardTransparent.frag (ST_FORWARD_TRANSPARENT) shades alpha-blended surfaces. They are not in the
//   prepass, so the screen-space ambient occlusion (which belongs to the opaque surface behind them) is not applied.

#ifndef ST_FORWARD_TRANSPARENT
layout(early_fragment_tests) in;
#endif

layout(location = 0) in vec3 v_WorldPosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent; // w: bitangent sign (already flipped for mirrored instances)
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_InstanceIndex;

layout(location = 0) out vec4 o_Color;

vec3 GetShadingNormal(MaterialData material, vec2 uv)
{
	// Back faces (double-sided materials) use the negated tangent frame: T, B and N all flip (glTF).
	float side = gl_FrontFacing ? 1.0 : -1.0;
	vec3 normal = normalize(v_Normal);
	vec3 tangent = v_Tangent.xyz - normal * dot(normal, v_Tangent.xyz);
	float tangentLength = length(tangent);
	if (tangentLength < 1e-5)
		return normal * side;

	tangent /= tangentLength;
	vec3 bitangent = cross(normal, tangent) * (v_Tangent.w < 0.0 ? -1.0 : 1.0);
	vec3 sampled = SampleMaterialTexture(material.NormalMap, uv).xyz * 2.0 - 1.0;
	sampled.xy *= material.NormalScale;
	vec3 mapped = mat3(tangent, bitangent, normal) * sampled;
	float mappedLength = length(mapped);
	return (mappedLength > 1e-5 ? mapped / mappedLength : normal) * side;
}

void main()
{
	// Derivatives need uniform control flow: take them before any branch or discard.
	vec3 positionDx = dFdx(v_WorldPosition);
	vec3 positionDy = dFdy(v_WorldPosition);

	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	vec2 uv = TransformUV(material, v_TexCoord);

	vec4 baseColor = material.BaseColor * SampleMaterialTexture(material.BaseColorMap, uv);
#ifdef ST_FORWARD_TRANSPARENT
	if ((material.Flags & ST_MATERIAL_ALPHA_MASK) != 0u && baseColor.a < material.AlphaCutoff)
		discard;
	float alpha = (material.Flags & ST_MATERIAL_ALPHA_BLEND) != 0u ? baseColor.a : 1.0;
#else
	const float alpha = 1.0;
#endif
	vec3 emissive = material.Emissive.rgb * SampleMaterialTexture(material.EmissiveMap, uv).rgb;

	if ((material.Flags & ST_MATERIAL_UNLIT) != 0u)
	{
		o_Color = vec4(SanitizeHDR(baseColor.rgb + emissive), alpha);
		return;
	}

	vec4 metallicRoughness = SampleMaterialTexture(material.MetallicRoughnessMap, uv);
	SurfaceData surface;
	surface.Position = v_WorldPosition;
	surface.Normal = GetShadingNormal(material, uv);
	surface.View = GetViewVector(v_WorldPosition);
	surface.Albedo = baseColor.rgb;
	surface.Alpha = baseColor.a;
	surface.Metallic = clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0);
	surface.Roughness = clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0);
	surface.Occlusion = mix(1.0, SampleMaterialTexture(material.OcclusionMap, uv).r, material.OcclusionStrength);
	surface.Emissive = emissive;

	float viewDepth = -(u_Frame.View * vec4(v_WorldPosition, 1.0)).z;
	vec3 color = vec3(0.0);
	if (u_Frame.DirectionalLightDirection.w > 0.0)
	{
		float shadow = 1.0;
		if (u_Frame.ShadowParams.w > 0.0)
		{
			vec3 geometricNormal = normalize(v_Normal) * (gl_FrontFacing ? 1.0 : -1.0);
			shadow = SampleDirectionalShadow(v_WorldPosition, geometricNormal, viewDepth, gl_FragCoord.xy, positionDx, positionDy);
		}
		color += EvaluateLight(surface, -u_Frame.DirectionalLightDirection.xyz, u_Frame.DirectionalLightColor.rgb) * shadow;
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
			color += EvaluateLight(surface, normalize(toLight), light.Color.rgb * attenuation);
		}
	}

	// Screen-space occlusion darkens indirect light only (direct light has shadows).
#ifdef ST_FORWARD_TRANSPARENT
	float screenOcclusion = 1.0;
#else
	float screenOcclusion = u_Frame.AOParams.x > 0.0 ? texelFetch(u_AmbientOcclusion, ivec2(gl_FragCoord.xy), 0).r : 1.0;
#endif
	float diffuseOcclusion = surface.Occlusion * screenOcclusion;
	if (u_Frame.EnvironmentParams.w > 0.0)
	{
		// Image-based lighting: diffuse irradiance plus split-sum specular (prefiltered radiance x environment BRDF).
		float NoV = max(dot(surface.Normal, surface.View), 1e-4);
		vec3 reflected = reflect(-surface.View, surface.Normal);
		vec3 f0 = mix(vec3(0.04), surface.Albedo, surface.Metallic);
		vec2 dfg = textureLod(sampler2D(u_BRDFLut, u_LinearClampSampler), vec2(NoV, surface.Roughness), 0.0).rg;
		vec3 specularColor = f0 * dfg.x + dfg.y;
		vec3 irradiance = textureLod(samplerCube(u_IrradianceMap, u_LinearClampSampler), RotateEnvironmentDirection(surface.Normal), 0.0).rgb;
		vec3 radiance = textureLod(samplerCube(u_PrefilteredMap, u_LinearClampSampler), RotateEnvironmentDirection(reflected),
			surface.Roughness * u_Frame.EnvironmentParams.z).rgb;
		vec3 diffuse = irradiance * surface.Albedo * (1.0 - surface.Metallic) * (1.0 - specularColor);
		// Specular occlusion from ambient occlusion (Lagarde): tighter for smooth surfaces and grazing views.
		float specularOcclusion = clamp(pow(NoV + diffuseOcclusion, exp2(-16.0 * surface.Roughness - 1.0)) - 1.0 + diffuseOcclusion, 0.0, 1.0);
		color += (diffuse * diffuseOcclusion + radiance * specularColor * specularOcclusion) * u_Frame.EnvironmentParams.x;
	}
	else
	{
		// Constant ambient without an environment map: diffuse only, modulated by material occlusion.
		color += u_Frame.AmbientColor.rgb * surface.Albedo * (1.0 - surface.Metallic) * diffuseOcclusion;
	}
	color += surface.Emissive;

	o_Color = vec4(SanitizeHDR(color), alpha);
}
