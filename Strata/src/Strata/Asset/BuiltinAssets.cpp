#include "stpch.h"
#include "Strata/Asset/BuiltinAssets.h"

#include "Strata/Asset/AssetManager.h"

#include <atomic>
#include <mutex>

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

		struct FactoryStorage
		{
			std::mutex Mutex;
			std::atomic<bool> Open = false;
			std::unordered_map<AssetHandle, BuiltinAssetFactory> Factories;
		};

		FactoryStorage& GetFactoryStorageUnchecked()
		{
			static FactoryStorage s_Storage;
			return s_Storage;
		}

		FactoryStorage& GetFactoryStorage()
		{
			FactoryStorage& storage = GetFactoryStorageUnchecked();
			ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
				"The built-in assets are used before Engine::RegisterBuiltinModules() registered the engine's modules");
			return storage;
		}

		const BuiltinAssetInfo* FindInfo(AssetHandle handle)
		{
			for (const BuiltinAssetInfo& info : c_BuiltinAssets)
			{
				if (info.Handle == handle)
					return &info;
			}
			return nullptr;
		}

	}

	std::span<const BuiltinAssetInfo> BuiltinAssets::GetAll()
	{
		return c_BuiltinAssets;
	}

	void BuiltinAssets::BeginRegistration()
	{
		GetFactoryStorageUnchecked().Open.store(true, std::memory_order_release);
	}

	bool BuiltinAssets::RegisterFactory(AssetHandle handle, BuiltinAssetFactory factory)
	{
		FactoryStorage& storage = GetFactoryStorage();
		if (!FindInfo(handle))
		{
			ST_CORE_ERROR("Asset {} is not a built-in asset; it cannot get a built-in asset factory", handle.ToString());
			return false;
		}
		if (!factory)
		{
			ST_CORE_ERROR("The factory of built-in asset {} is empty", handle.ToString());
			return false;
		}

		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Factories[handle] = std::move(factory);
		return true;
	}

	void BuiltinAssets::Register(AssetManagerBase& manager)
	{
		// The factories run without the lock: they are module code.
		std::unordered_map<AssetHandle, BuiltinAssetFactory> factories;
		{
			FactoryStorage& storage = GetFactoryStorage();
			std::scoped_lock<std::mutex> lock(storage.Mutex);
			factories = storage.Factories;
		}

		for (const BuiltinAssetInfo& info : c_BuiltinAssets)
		{
			// Built-in assets of modules a build does not contain have no factory.
			auto factory = factories.find(info.Handle);
			if (factory == factories.end())
				continue;

			const Ref<Asset> asset = factory->second();
			if (!asset || asset->GetType() != info.Type)
			{
				ST_CORE_ERROR("The factory of built-in asset '{}' did not create a {}", info.Name, AssetTypeToString(info.Type));
				continue;
			}

			AssetMetadata metadata;
			metadata.Handle = info.Handle;
			metadata.Type = info.Type;
			metadata.Name = std::string(info.Name);
			metadata.Path = "Builtin/" + std::string(info.Name);
			manager.AddMemoryAsset(asset, metadata);
		}
	}

}
