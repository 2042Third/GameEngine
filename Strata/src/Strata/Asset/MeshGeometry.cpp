#include "stpch.h"
#include "Strata/Asset/MeshGeometry.h"

#include "Strata/Asset/Asset.h"
#include "Strata/Core/Assert.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace Strata
{

	namespace
	{

		struct ReaderStorage
		{
			std::atomic<bool> Open = false;
			std::mutex Mutex;
			std::unordered_map<AssetType, MeshGeometryReader> Readers;
		};

		ReaderStorage& GetStorageUnchecked()
		{
			static ReaderStorage s_Storage;
			return s_Storage;
		}

		ReaderStorage& GetStorage()
		{
			ReaderStorage& storage = GetStorageUnchecked();
			ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
				"The mesh geometry registry is used before Engine::RegisterBuiltinModules() registered the engine's modules");
			return storage;
		}

	}

	void MeshGeometryRegistry::BeginRegistration()
	{
		GetStorageUnchecked().Open.store(true, std::memory_order_release);
	}

	void MeshGeometryRegistry::Register(AssetType type, MeshGeometryReader reader)
	{
		ReaderStorage& storage = GetStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Readers[type] = reader;
	}

	bool MeshGeometryRegistry::Read(const Asset& asset, MeshGeometry& outGeometry)
	{
		outGeometry.Positions.clear();
		outGeometry.Indices.clear();
		MeshGeometryReader reader = nullptr;
		{
			ReaderStorage& storage = GetStorage();
			std::scoped_lock<std::mutex> lock(storage.Mutex);
			auto it = storage.Readers.find(asset.GetType());
			if (it != storage.Readers.end())
				reader = it->second;
		}
		if (reader && reader(asset, outGeometry))
			return true;
		outGeometry.Positions.clear();
		outGeometry.Indices.clear();
		return false;
	}

}
