#include "stpch.h"
#include "Strata/Renderer/RendererRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Texture.h"

namespace Strata
{

	namespace
	{

		void RegisterBuiltinAsset(AssetHandle handle, BuiltinAssetFactory factory)
		{
			const bool registered = BuiltinAssets::RegisterFactory(handle, std::move(factory));
			ST_CORE_VERIFY(registered, "The renderer could not provide built-in asset {}", handle.ToString());
		}

	}

	void RegisterRendererModule()
	{
		// Textures and meshes are stored in their cooked binary form, materials and fonts as their source. Textures and
		// fonts keep their bytes: they take them over instead of copying them.
		AssetLoaderRegistry::Register(AssetType::Texture, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Texture::Deserialize(data.TakeBytes(), outError);
		});
		AssetLoaderRegistry::Register(AssetType::Mesh, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Mesh::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Material, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Material::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Font, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Font::Create(data.TakeBytes(), outError);
		});

		// The primitive meshes use the default material.
		RegisterBuiltinAsset(BuiltinAssets::DefaultMaterial, []() -> Ref<Asset> { return Material::Create(); });
		RegisterBuiltinAsset(BuiltinAssets::CubeMesh, []() -> Ref<Asset> { return MeshFactory::CreateCube(1.0f, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::SphereMesh, []() -> Ref<Asset> { return MeshFactory::CreateSphere(0.5f, 48, 24, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::PlaneMesh, []() -> Ref<Asset> { return MeshFactory::CreatePlane(1.0f, 1, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::QuadMesh, []() -> Ref<Asset> { return MeshFactory::CreateQuad(1.0f, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::CylinderMesh, []() -> Ref<Asset> { return MeshFactory::CreateCylinder(0.5f, 1.0f, 48, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::CapsuleMesh, []() -> Ref<Asset> { return MeshFactory::CreateCapsule(0.5f, 0.5f, 48, 12, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::ConeMesh, []() -> Ref<Asset> { return MeshFactory::CreateCone(0.5f, 1.0f, 48, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::TorusMesh, []() -> Ref<Asset> { return MeshFactory::CreateTorus(0.375f, 0.125f, 48, 24, BuiltinAssets::DefaultMaterial); });
	}

}
