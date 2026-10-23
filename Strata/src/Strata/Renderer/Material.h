#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Reflection/Property.h"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>
#include <vector>

namespace Strata
{

	enum class MaterialAlphaMode : uint8_t
	{
		Opaque = 0,
		Mask,  // Alpha-tested against AlphaCutoff
		Blend  // Alpha-blended (rendered after opaque geometry, sorted back to front)
	};

	// PBR metallic-roughness parameters (glTF 2.0 model). Texture maps are optional; factors multiply them.
	struct MaterialProperties
	{
		glm::vec4 BaseColor = { 1.0f, 1.0f, 1.0f, 1.0f }; // Linear RGBA
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		// Emitted luminance in nits (cd/m^2): EmissiveColor * EmissiveIntensity, times the emissive map.
		glm::vec3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
		float EmissiveIntensity = 1.0f;
		float NormalScale = 1.0f;
		float OcclusionStrength = 1.0f;
		MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		bool Unlit = false;
		glm::vec2 UVTiling = { 1.0f, 1.0f };
		glm::vec2 UVOffset = { 0.0f, 0.0f };

		AssetHandle BaseColorMap = UUID::Null();         // sRGB color (+ alpha)
		AssetHandle MetallicRoughnessMap = UUID::Null(); // Linear: G = roughness, B = metallic (glTF packing)
		AssetHandle NormalMap = UUID::Null();            // Tangent-space normal map
		AssetHandle OcclusionMap = UUID::Null();         // Linear: R = ambient occlusion
		AssetHandle EmissiveMap = UUID::Null();          // sRGB emissive color

		// A clear coat over the base (KHR_materials_clearcoat): a second, dielectric specular layer (IOR 1.5) with its own
		// roughness and normal; the light it reflects no longer reaches the base.
		float Clearcoat = 0.0f;          // The coat's strength, times ClearcoatMap's red channel
		float ClearcoatRoughness = 0.0f; // Perceptual, times ClearcoatRoughnessMap's green channel
		AssetHandle ClearcoatMap = UUID::Null();          // Linear: R = coat strength
		AssetHandle ClearcoatRoughnessMap = UUID::Null(); // Linear: G = coat roughness
		AssetHandle ClearcoatNormalMap = UUID::Null();    // Tangent-space normal map of the coat (the vertex normal without one)

		// Light passing through the surface (KHR_materials_transmission, _ior, _volume): a transmissive material is drawn as
		// glass, which shows what lies behind it refracted, tinted by the base color and, through its thickness, absorbed.
		float Transmission = 0.0f;                  // The share of light passed through, times TransmissionMap's red channel
		AssetHandle TransmissionMap = UUID::Null(); // Linear: R = transmission
		float IOR = 1.5f;                           // Index of refraction (1: no bending; glass 1.5, water 1.33, diamond 2.42)
		float Thickness = 0.0f;                     // Of the volume behind the surface, in the mesh's units (0: a thin wall)
		glm::vec3 AttenuationColor = { 1.0f, 1.0f, 1.0f }; // Linear color white light has after AttenuationDistance in the volume
		float AttenuationDistance = 0.0f;           // World units; 0: the volume absorbs nothing
	};

	// Material asset. Stored as JSON (".stmat"):
	//   { "Strata": { "Format": "Material", "Version": 1 }, "Material": { "BaseColor": [1, 1, 1, 1], "Roughness": 0.5, ... } }
	class Material : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Material; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_FormatVersion = 1;

		static Ref<Material> Create(const MaterialProperties& properties = {});
		static Ref<Material> FromJson(const nlohmann::json& json, std::string* outError = nullptr, std::vector<std::string>* outWarnings = nullptr);
		static Ref<Material> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		nlohmann::json Serialize() const;

		// Requests the referenced textures so they start streaming as soon as the material is in use, except those already on
		// their way (prefetched alongside the material, see AssetManagerBase::GetDependencies). A material that arrives only
		// prefetched (AssetFinalizeContext::Prefetched) requests none.
		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// Parameters only: the textures are assets of their own.
		AssetMemoryUsage GetMemoryUsage() const override { return AssetMemoryUsage { sizeof(Material) }; }

		// Read only: a material is not changed once made. Edits publish a new object (AssetManagerBase::ReplaceLoadedAsset),
		// which is how renderers that keep a material's GPU data from frame to frame learn of them
		// (SceneRenderer::MaterialResolution).
		const MaterialProperties& GetProperties() const { return m_Properties; }
		std::vector<AssetHandle> GetTextureHandles() const;

		// Reflection metadata of MaterialProperties (inspector, automation API, JSON conversion).
		static const std::vector<PropertyInfo>& GetPropertyInfos();
	private:
		Material() = default;
	private:
		MaterialProperties m_Properties;
	};

}
