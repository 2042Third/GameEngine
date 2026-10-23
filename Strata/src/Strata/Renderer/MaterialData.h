#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace Strata
{

	class Material;
	struct MaterialProperties;

	// What a material is on the GPU, the same for every renderer and path: its MaterialData (Include/Scene.glsl) and the
	// flags culling and batching read.
	namespace MaterialGPU
	{

		// How a frame draws a material (RenderData::c_CullMaterial*: available, double-sided, alpha-masked, transparent).
		uint32_t GetFlags(const MaterialProperties& properties);

		// The material's texture maps (invalid handles where it has none): base color, metallic-roughness, normal, occlusion,
		// emission, the clear coat's strength, roughness and normal, and transmission, the order Build resolves them in.
		static constexpr size_t c_TextureMapCount = 9;
		std::array<AssetHandle, c_TextureMapCount> GetTextureMaps(const MaterialProperties& properties);

		// Whether the material has a clear coat (Clearcoat above 0) and whether it is drawn as glass (Transmission above 0); unlit
		// materials have neither.
		bool HasClearcoat(const MaterialProperties& properties);
		bool IsTransmissive(const MaterialProperties& properties);
		// What a transmissive volume absorbs per world unit (Beer's law), from its attenuation color and distance: 0 where it
		// absorbs nothing (AttenuationDistance 0).
		glm::vec3 GetAbsorption(const MaterialProperties& properties);

		// The texture table index of one of a material's maps (TextureTable): given the map's handle, the index to use where it
		// has none (`missingIndex`) and where it is not uploaded yet (`loadingIndex`).
		using TextureIndexResolver = std::function<uint32_t(AssetHandle handle, uint32_t missingIndex, uint32_t loadingIndex)>;

		// The material's GPU data. Missing maps use neutral textures, maps still loading too, except emission (black until
		// the map arrives, so surfaces do not flash at full emissive strength).
		RenderData::MaterialData Build(const Material& material, const TextureIndexResolver& textureIndex);

	}

}
