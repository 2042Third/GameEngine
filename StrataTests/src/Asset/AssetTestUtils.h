#pragma once

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JobSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata::Tests
{

	// The asset type the fake assets below are loaded as. Its real loader is replaced while a ScopedFakeLoader lives.
	constexpr AssetType c_FakeAssetType = AssetType::Font;

	// An asset that holds nothing but reports the memory it was declared with, so residency can be tested with any
	// sizes in any pool without a GPU.
	class FakeSizedAsset final : public Asset
	{
	public:
		explicit FakeSizedAsset(const AssetMemoryUsage& usage)
			: m_Usage(usage)
		{
		}

		AssetType GetType() const override { return c_FakeAssetType; }
		AssetMemoryUsage GetMemoryUsage() const override { return m_Usage; }
		// Changes what it holds, as a partly resident asset does when it raises or drops levels (the manager hears of it
		// through AssetManagerBase::UpdateMemoryUsage).
		void SetUsage(const AssetMemoryUsage& usage) { m_Usage = usage; }
		// Levels of detail it reports (Asset::GetDetailLevels), as a texture streaming its mip levels does; none by default.
		void SetDetailLevels(const std::optional<AssetDetailLevels>& levels) { m_DetailLevels = levels; }
		bool GetDetailLevels(AssetDetailLevels& outLevels) const override
		{
			if (!m_DetailLevels)
				return false;
			outLevels = *m_DetailLevels;
			return true;
		}
	private:
		AssetMemoryUsage m_Usage;
		std::optional<AssetDetailLevels> m_DetailLevels;
	};

	// The stored form of a fake asset: its declared usage.
	inline std::vector<uint8_t> EncodeFakeAsset(const AssetMemoryUsage& usage)
	{
		std::vector<uint8_t> data(sizeof(AssetMemoryUsage));
		std::memcpy(data.data(), &usage, sizeof(usage));
		return data;
	}

	// Loads fake assets through the real pipeline while it lives: replaces the loader of c_FakeAssetType and restores
	// the previous one afterwards. Counts the loader's calls. No load may run while it is created or destroyed.
	class ScopedFakeLoader
	{
	public:
		ScopedFakeLoader()
		{
			const AssetLoadFunction* previous = AssetLoaderRegistry::Find(c_FakeAssetType);
			REQUIRE(previous);
			m_Previous = *previous;
			std::atomic<uint32_t>* calls = &m_Calls;
			AssetLoaderRegistry::Register(c_FakeAssetType, [calls](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
			{
				calls->fetch_add(1);
				if (data.size() != sizeof(AssetMemoryUsage))
				{
					if (outError)
						*outError = "Not a fake asset";
					return nullptr;
				}
				AssetMemoryUsage usage;
				std::memcpy(&usage, data.data(), sizeof(usage));
				return CreateRef<FakeSizedAsset>(usage);
			});
		}

		~ScopedFakeLoader()
		{
			AssetLoaderRegistry::Register(c_FakeAssetType, m_Previous);
		}

		ScopedFakeLoader(const ScopedFakeLoader&) = delete;
		ScopedFakeLoader& operator=(const ScopedFakeLoader&) = delete;

		uint32_t GetCallCount() const { return m_Calls.load(); }
	private:
		AssetLoadFunction m_Previous;
		std::atomic<uint32_t> m_Calls = 0;
	};

	// Starts the job system for a test and shuts it down at the end, also when the test fails. It has as many background
	// threads as workers on any machine: the application's default leaves hardware threads to the frame, which leaves one
	// background thread on a machine of three (CI's macOS runners), and tests that run imports side by side need theirs.
	class ScopedJobSystem
	{
	public:
		ScopedJobSystem(uint32_t workerThreads, uint32_t ioThreads)
		{
			REQUIRE_FALSE(JobSystem::IsInitialized());
			JobSystemSpecification specification;
			specification.WorkerThreadCount = workerThreads;
			specification.IOThreadCount = ioThreads;
			specification.BackgroundThreadCount = workerThreads;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}

		ScopedJobSystem(const ScopedJobSystem&) = delete;
		ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
	};

	// An asset manager over in-memory assets: fake ones (Add) or real ones in their stored form (AddStored). Records the
	// order of reads and can hold reads at a gate: a read waits until the test lets it through (AllowReads), so tests can
	// keep loads in flight while they queue others. Its loads read whole stored forms (AssetManagerBase::SetPartialLoads
	// false): tests of partial residency turn partial loads on.
	class FakeAssetManager final : public AssetManagerBase
	{
	public:
		FakeAssetManager()
		{
			SetPartialLoads(false);
		}

		~FakeAssetManager() override
		{
			OpenGate();
			WaitForInFlightLoads();
		}

		// Registers a fake asset declaring `usage`, whose stored form takes `storedSize` bytes (0: unknown). Assets registered
		// with another type than c_FakeAssetType read like fake assets but fail to load (the type's real loader refuses them).
		AssetHandle Add(uint64_t handle, const AssetMemoryUsage& usage, uint64_t storedSize = 0, AssetType type = c_FakeAssetType)
		{
			return Register(handle, type, EncodeFakeAsset(usage), storedSize);
		}

		// Registers an asset of any type whose stored form is `data`, decoded by the real loader of `type` (a material
		// document, or a mesh's Mesh::Serialize, say).
		AssetHandle AddStored(uint64_t handle, AssetType type, std::vector<uint8_t> data)
		{
			const uint64_t storedSize = data.size();
			return Register(handle, type, std::move(data), storedSize);
		}

		// Replaces the stored form of an asset: its next load reads the new one (ReloadAsset loads it again if it was loaded).
		void SetStoredData(AssetHandle handle, std::vector<uint8_t> data)
		{
			const uint64_t size = data.size();
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[handle] = std::move(data);
			}
			SetStoredSize(handle, size);
		}

		// Registers an asset of any type whose stored form is `data` (loaded by the type's registered loader).
		AssetHandle AddStored(uint64_t handle, AssetType type, std::vector<uint8_t> data, const std::string& name)
		{
			AssetMetadata metadata;
			metadata.Handle = UUID(handle);
			metadata.Type = type;
			metadata.Path = name;
			metadata.Name = name;
			metadata.StoredSize = data.size();
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[metadata.Handle] = std::move(data);
			}
			RegisterAsset(metadata);
			return metadata.Handle;
		}

		void Remove(AssetHandle handle) { UnregisterAsset(handle); }

		// Records what an asset depends on, as an importer or a pack's table would.
		void SetDependencies(AssetHandle handle, const std::vector<AssetHandle>& dependencies, std::optional<AABB> bounds = std::nullopt)
		{
			SetAssetReferences(handle, dependencies, bounds);
		}

		// Reads wait at the gate while it is closed, until AllowReads lets them through.
		void CloseGate()
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_GateClosed = true;
			m_AllowedReads = 0;
		}

		void OpenGate()
		{
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_GateClosed = false;
			}
			m_Condition.notify_all();
		}

		void AllowReads(uint32_t count)
		{
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_AllowedReads += count;
			}
			m_Condition.notify_all();
		}

		// Waits until this many reads wait at the gate.
		bool WaitForWaitingReads(uint32_t count, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			return m_Condition.wait_for(lock, timeout, [this, count]() { return m_WaitingReads >= count; });
		}

		uint32_t GetReadCount(AssetHandle handle) const
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			return static_cast<uint32_t>(std::count(m_ReadOrder.begin(), m_ReadOrder.end(), handle));
		}

		// Handles in the order their reads started (passed the gate).
		std::vector<AssetHandle> GetReadOrder() const
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			return m_ReadOrder;
		}

		// The manager's frame (GetFrameIndex) when the asset's latest read passed the gate; nullopt before its first read.
		std::optional<uint64_t> GetReadFrame(AssetHandle handle) const
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_ReadFrames.find(handle);
			return it != m_ReadFrames.end() ? std::optional<uint64_t>(it->second) : std::nullopt;
		}

		// Gives a registered asset another path and name, as a move does.
		void Rename(AssetHandle handle, const std::string& path)
		{
			std::optional<AssetMetadata> metadata = GetMetadata(handle);
			REQUIRE(metadata.has_value());
			metadata->Path = path;
			metadata->Name = path;
			UpdateAssetMetadata(*metadata);
		}
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			m_WaitingReads++;
			m_Condition.notify_all();
			m_Condition.wait(lock, [this]() { return !m_GateClosed || m_AllowedReads > 0; });
			if (m_GateClosed)
				m_AllowedReads--;
			m_WaitingReads--;
			m_ReadOrder.push_back(metadata.Handle);
			m_ReadFrames[metadata.Handle] = GetFrameIndex();

			auto it = m_Data.find(metadata.Handle);
			if (it == m_Data.end())
			{
				if (outError)
					*outError = "No data";
				return false;
			}
			outData = it->second;
			return true;
		}
	private:
		AssetHandle Register(uint64_t handle, AssetType type, std::vector<uint8_t> data, uint64_t storedSize)
		{
			AssetMetadata metadata;
			metadata.Handle = UUID(handle);
			metadata.Type = type;
			metadata.Path = "Fake/" + std::to_string(handle);
			metadata.Name = metadata.Path;
			metadata.StoredSize = storedSize;
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[metadata.Handle] = std::move(data);
			}
			RegisterAsset(metadata);
			return metadata.Handle;
		}
	private:
		mutable std::mutex m_Mutex;
		std::condition_variable m_Condition;
		std::unordered_map<AssetHandle, std::vector<uint8_t>> m_Data;
		std::vector<AssetHandle> m_ReadOrder;
		std::unordered_map<AssetHandle, uint64_t> m_ReadFrames;
		bool m_GateClosed = false;
		uint32_t m_AllowedReads = 0;
		uint32_t m_WaitingReads = 0;
	};

	// Whether the statistics' counts of assets per state and of pinned assets are those of the assets themselves, recounted
	// from GetResidencyInfo (which lists every asset that is not unloaded, and every pinned one), and the listed entries are
	// those a look at every entry finds (AssetManagerBase::ValidateResidencyIndex).
	inline bool StateCountsMatch(const AssetManagerBase& manager)
	{
		std::string error;
		if (!manager.ValidateResidencyIndex(&error))
		{
			MESSAGE(error);
			return false;
		}
		uint32_t ready = 0;
		uint32_t loading = 0;
		uint32_t failed = 0;
		uint32_t pinned = 0;
		for (const AssetResidencyInfo& asset : manager.GetResidencyInfo())
		{
			ready += asset.State == AssetState::Ready ? 1 : 0;
			loading += asset.State == AssetState::Loading ? 1 : 0;
			failed += asset.State == AssetState::Failed ? 1 : 0;
			pinned += asset.PinCount > 0 ? 1 : 0;
		}
		const AssetManagerStats stats = manager.GetStats();
		return stats.LoadedAssets == ready && stats.LoadingAssets == loading && stats.FailedAssets == failed && stats.PinnedAssets == pinned;
	}

	inline AssetMemoryUsage MakeUsage(uint64_t cpu, uint64_t gpuTextures = 0, uint64_t gpuBuffers = 0)
	{
		return AssetMemoryUsage { cpu, gpuTextures, gpuBuffers };
	}

}
