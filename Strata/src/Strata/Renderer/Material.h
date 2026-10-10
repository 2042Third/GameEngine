#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Reflection/Property.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

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

		// Requests the referenced textures so they start streaming as soon as the material is in use.
		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// Parameters only: the textures are assets of their own.
		AssetMemoryUsage GetMemoryUsage() const override { return AssetMemoryUsage { sizeof(Material) }; }

		MaterialProperties& GetProperties() { return m_Properties; }
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
