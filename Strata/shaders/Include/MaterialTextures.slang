#ifndef STRATA_MATERIAL_TEXTURES_SLANG
#define STRATA_MATERIAL_TEXTURES_SLANG

// Sampling material textures through the texture table (fragment shaders only: it takes screen-space derivatives).

#include "Include/Scene.slang"

// The anisotropy of the material samplers (Renderer.cpp, SceneRenderer.cpp): what the hardware's level of detail divides
// the longer footprint axis by.
#define ST_MATERIAL_MAX_ANISOTROPY 16.0

// The texture of a texture table index at `uv`. An entry with a minimum level of detail (a texture fading in levels it
// just gained, TextureTable::SetFadeIn) is sampled with its gradients scaled up until the level of detail the hardware
// picks reaches that minimum: both axes by the same factor, so anisotropic filtering keeps the footprint's shape.
// Renderers of single images (captures, thumbnails: SceneRendererSpecification::FadeTextureLevels false) skip the fade.
float4 SampleMaterialTexture(uint textureIndex, float2 uv)
{
	uint2 entry = u_TextureTable[textureIndex];
	uint packedSlot = entry.x;
	uint slot = packedSlot & 0xFFFFFFu;
	uint samplerIndex = min((packedSlot >> 24) & 0x7Fu, uint(ST_SAMPLER_COUNT - 1));
	float minLod = asfloat(entry.y);
	// Taken in uniform control flow, for the fade below.
	float2 uvDx = ddx(uv);
	float2 uvDy = ddy(uv);
	float4 value;
	if (minLod > 0.0 && u_Frame.MaterialParams.x != 0.0)
	{
		float2 size = float2(TextureSize(u_Textures[NonUniformResourceIndex(slot)], 0));
		float lengthX = length(uvDx * size);
		float lengthY = length(uvDy * size);
		// The anisotropic level of detail: the shorter axis, unless the longer one exceeds the anisotropy it filters with.
		float footprint = max(min(lengthX, lengthY), max(lengthX, lengthY) / ST_MATERIAL_MAX_ANISOTROPY);
		float lod = log2(max(footprint, 1e-8));
		float scale = exp2(max(minLod - lod, 0.0));
		value = u_Textures[NonUniformResourceIndex(slot)].SampleGrad(u_Samplers[NonUniformResourceIndex(samplerIndex)], uv, uvDx * scale, uvDy * scale);
	}
	else
	{
		value = u_Textures[NonUniformResourceIndex(slot)].Sample(u_Samplers[NonUniformResourceIndex(samplerIndex)], uv);
	}
	return (packedSlot & ST_TEXTURE_SLOT_GRAYSCALE) != 0u ? float4(value.rrr, 1.0) : value;
}

// The normal of a surface (world space) mapped by the normal map of a texture table index, scaled by `normalScale`, from its
// interpolated vertex normal and tangent (w: the bitangent's sign, already flipped for mirrored instances).
float3 GetMappedNormal(uint normalMap, float normalScale, float2 uv, float3 vertexNormal, float4 vertexTangent, bool frontFacing)
{
	// Back faces (double-sided materials) use the negated tangent frame: T, B and N all flip (glTF).
	float side = frontFacing ? 1.0 : -1.0;
	float3 normal = normalize(vertexNormal);
	float3 tangent = vertexTangent.xyz - normal * dot(normal, vertexTangent.xyz);
	float tangentLength = length(tangent);
	if (tangentLength < 1e-5)
		return normal * side;

	tangent /= tangentLength;
	float3 bitangent = cross(normal, tangent) * (vertexTangent.w < 0.0 ? -1.0 : 1.0);
	// Normal maps store X and Y (BC5 keeps only those); Z is reconstructed from them, so every format shades alike.
	float2 sampledXY = SampleMaterialTexture(normalMap, uv).xy * 2.0 - 1.0;
	float3 sampled = float3(sampledXY * normalScale, sqrt(clamp(1.0 - dot(sampledXY, sampledXY), 0.0, 1.0)));
	float3 mapped = mul(transpose(float3x3(tangent, bitangent, normal)), sampled);
	float mappedLength = length(mapped);
	return (mappedLength > 1e-5 ? mapped / mappedLength : normal) * side;
}

// The normal-mapped normal of a surface (world space), as the forward pass shades it and the prepass's surface target stores
// it.
float3 GetShadingNormal(MaterialData material, float2 uv, float3 vertexNormal, float4 vertexTangent, bool frontFacing)
{
	return GetMappedNormal(material.NormalMap, material.NormalScale, uv, vertexNormal, vertexTangent, frontFacing);
}

#endif
