#ifndef STRATA_MATERIAL_TEXTURES_GLSL
#define STRATA_MATERIAL_TEXTURES_GLSL

// Sampling material textures through the texture table (fragment shaders only: it takes screen-space derivatives).

#include "Include/Scene.glsl"

// The anisotropy of the material samplers (Renderer.cpp, SceneRenderer.cpp): what the hardware's level of detail divides
// the longer footprint axis by.
#define ST_MATERIAL_MAX_ANISOTROPY 16.0

// The texture of a texture table index at `uv`. An entry with a minimum level of detail (a texture fading in levels it
// just gained, TextureTable::SetFadeIn) is sampled with its gradients scaled up until the level of detail the hardware
// picks reaches that minimum: both axes by the same factor, so anisotropic filtering keeps the footprint's shape.
// Renderers of single images (captures, thumbnails: SceneRendererSpecification::FadeTextureLevels false) skip the fade.
vec4 SampleMaterialTexture(uint textureIndex, vec2 uv)
{
	uvec2 entry = u_TextureTable[textureIndex];
	uint packedSlot = entry.x;
	uint slot = packedSlot & 0xFFFFFFu;
	uint samplerIndex = min((packedSlot >> 24) & 0x7Fu, uint(ST_SAMPLER_COUNT - 1));
	float minLod = uintBitsToFloat(entry.y);
	// Taken in uniform control flow, for the fade below.
	vec2 uvDx = dFdx(uv);
	vec2 uvDy = dFdy(uv);
	vec4 value;
	if (minLod > 0.0 && u_Frame.MaterialParams.x != 0.0)
	{
		vec2 size = vec2(textureSize(sampler2D(u_Textures[nonuniformEXT(slot)], u_Samplers[nonuniformEXT(samplerIndex)]), 0));
		float lengthX = length(uvDx * size);
		float lengthY = length(uvDy * size);
		// The anisotropic level of detail: the shorter axis, unless the longer one exceeds the anisotropy it filters with.
		float footprint = max(min(lengthX, lengthY), max(lengthX, lengthY) / ST_MATERIAL_MAX_ANISOTROPY);
		float lod = log2(max(footprint, 1e-8));
		float scale = exp2(max(minLod - lod, 0.0));
		value = textureGrad(sampler2D(u_Textures[nonuniformEXT(slot)], u_Samplers[nonuniformEXT(samplerIndex)]), uv, uvDx * scale, uvDy * scale);
	}
	else
	{
		value = texture(sampler2D(u_Textures[nonuniformEXT(slot)], u_Samplers[nonuniformEXT(samplerIndex)]), uv);
	}
	return (packedSlot & ST_TEXTURE_SLOT_GRAYSCALE) != 0u ? vec4(value.rrr, 1.0) : value;
}

// The normal-mapped normal of a surface (world space) from its interpolated vertex normal and tangent (w: the bitangent's sign,
// already flipped for mirrored instances), as the forward pass shades it and the prepass's surface target stores it.
vec3 GetShadingNormal(MaterialData material, vec2 uv, vec3 vertexNormal, vec4 vertexTangent, bool frontFacing)
{
	// Back faces (double-sided materials) use the negated tangent frame: T, B and N all flip (glTF).
	float side = frontFacing ? 1.0 : -1.0;
	vec3 normal = normalize(vertexNormal);
	vec3 tangent = vertexTangent.xyz - normal * dot(normal, vertexTangent.xyz);
	float tangentLength = length(tangent);
	if (tangentLength < 1e-5)
		return normal * side;

	tangent /= tangentLength;
	vec3 bitangent = cross(normal, tangent) * (vertexTangent.w < 0.0 ? -1.0 : 1.0);
	// Normal maps store X and Y (BC5 keeps only those); Z is reconstructed from them, so every format shades alike.
	vec2 sampledXY = SampleMaterialTexture(material.NormalMap, uv).xy * 2.0 - 1.0;
	vec3 sampled = vec3(sampledXY * material.NormalScale, sqrt(clamp(1.0 - dot(sampledXY, sampledXY), 0.0, 1.0)));
	vec3 mapped = mat3(tangent, bitangent, normal) * sampled;
	float mappedLength = length(mapped);
	return (mappedLength > 1e-5 ? mapped / mappedLength : normal) * side;
}

#endif
