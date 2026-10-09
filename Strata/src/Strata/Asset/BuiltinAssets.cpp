#include "stpch.h"
#include "Strata/Asset/BuiltinAssets.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/MeshFactory.h"

namespace Strata
{

	namespace
	{
		constexpr BuiltinAssetInfo c_BuiltinAssets[] = {
			{ BuiltinAssets::CubeMesh, AssetType::Mesh, "Cube" },
			{ BuiltinAssets::SphereMesh, AssetType::Mesh, "Sphere" },
			{ BuiltinAssets::PlaneMesh, AssetType::Mesh, "Plane" },
			{ BuiltinAssets::QuadMesh, AssetType::Mesh, "Quad" },
			{ BuiltinAssets::CylinderMesh, AssetType::Mesh, "Cylinder" },
			{ BuiltinAssets::CapsuleMesh, AssetType::Mesh, "Capsule" },
			{ BuiltinAssets::ConeMesh, AssetType::Mesh, "Cone" },
			{ BuiltinAssets::TorusMesh, AssetType::Mesh, "Torus" },
			{ BuiltinAssets::DefaultMaterial, AssetType::Material, "DefaultMaterial" }
		};
	}

	std::span<const BuiltinAssetInfo> BuiltinAssets::GetAll()
	{
		return c_BuiltinAssets;
	}

	void BuiltinAssets::Register(AssetManagerBase& manager)
	{
		auto add = [&manager](const Ref<Asset>& asset, AssetHandle handle)
		{
			for (const BuiltinAssetInfo& info : c_BuiltinAssets)
			{
				if (info.Handle != handle)
					continue;
				AssetMetadata metadata;
				metadata.Handle = handle;
				metadata.Type = info.Type;
				metadata.Name = std::string(info.Name);
				metadata.Path = "Builtin/" + std::string(info.Name);
				manager.AddMemoryAsset(asset, metadata);
				return;
			}
		};

		const AssetHandle material = DefaultMaterial;
		add(Material::Create(), DefaultMaterial);
		add(MeshFactory::CreateCube(1.0f, material), CubeMesh);
		add(MeshFactory::CreateSphere(0.5f, 48, 24, material), SphereMesh);
		add(MeshFactory::CreatePlane(1.0f, 1, material), PlaneMesh);
		add(MeshFactory::CreateQuad(1.0f, material), QuadMesh);
		add(MeshFactory::CreateCylinder(0.5f, 1.0f, 48, material), CylinderMesh);
		add(MeshFactory::CreateCapsule(0.5f, 0.5f, 48, 12, material), CapsuleMesh);
		add(MeshFactory::CreateCone(0.5f, 1.0f, 48, material), ConeMesh);
		add(MeshFactory::CreateTorus(0.375f, 0.125f, 48, 24, material), TorusMesh);
	}

}
