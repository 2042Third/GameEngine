#include "stpch.h"
#include "Strata/Renderer/MaterialData.h"

#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/TextureTable.h"

namespace Strata::MaterialGPU
{

	uint32_t GetFlags(const MaterialProperties& properties)
	{
		return RenderData::c_CullMaterialAvailable | (properties.DoubleSided ? RenderData::c_CullMaterialDoubleSided : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_CullMaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend ? RenderData::c_CullMaterialTransparent : 0u);
	}

	std::array<AssetHandle, c_TextureMapCount> GetTextureMaps(const MaterialProperties& properties)
	{
		return { properties.BaseColorMap, properties.MetallicRoughnessMap, properties.NormalMap, properties.OcclusionMap, properties.EmissiveMap };
	}

	RenderData::MaterialData Build(const Material& material, const TextureIndexResolver& textureIndex)
	{
		const MaterialProperties& properties = material.GetProperties();
		RenderData::MaterialData data = {};
		data.BaseColor = properties.BaseColor;
		data.Emissive = glm::vec4(properties.EmissiveColor * properties.EmissiveIntensity, 0.0f);
		data.Metallic = properties.Metallic;
		data.Roughness = properties.Roughness;
		data.NormalScale = properties.NormalScale;
		data.OcclusionStrength = properties.OcclusionStrength;
		data.AlphaCutoff = properties.AlphaCutoff;
		data.Flags = (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_MaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend ? RenderData::c_MaterialAlphaBlend : 0u)
			| (properties.Unlit ? RenderData::c_MaterialUnlit : 0u)
			| (properties.DoubleSided ? RenderData::c_MaterialDoubleSided : 0u);
		data.UVTransform = glm::vec4(properties.UVTiling, properties.UVOffset);
		data.BaseColorMap = textureIndex(properties.BaseColorMap, TextureTable::c_White, TextureTable::c_White);
		data.MetallicRoughnessMap = textureIndex(properties.MetallicRoughnessMap, TextureTable::c_White, TextureTable::c_White);
		data.NormalMap = textureIndex(properties.NormalMap, TextureTable::c_FlatNormal, TextureTable::c_FlatNormal);
		data.OcclusionMap = textureIndex(properties.OcclusionMap, TextureTable::c_White, TextureTable::c_White);
		data.EmissiveMap = textureIndex(properties.EmissiveMap, TextureTable::c_White, TextureTable::c_Black);
		return data;
	}

}
