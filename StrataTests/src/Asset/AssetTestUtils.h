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
#include <span>
#include <string>
#include <unordered_map>
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
	private:
		AssetMemoryUsage m_Usage;
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

	// Starts the job system for a test and shuts it down at the end, also when the test fails.
	class ScopedJobSystem
	{
	public:
		ScopedJobSystem(uint32_t workerThreads, uint32_t ioThreads)
		{
			REQUIRE_FALSE(JobSystem::IsInitialized());
			JobSystemSpecification specification;
			specification.WorkerThreadCount = workerThreads;
			specification.IOThreadCount = ioThreads;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}

		ScopedJobSystem(const ScopedJobSystem&) = delete;
		ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
	};

	// An asset manager over in-memory fake assets. Records the order of reads and can hold reads at a gate: a read waits
	// until the test lets it through (AllowReads), so tests can keep loads in flight while they queue others.
	class FakeAssetManager final : public AssetManagerBase
	{
	public:
		~FakeAssetManager() override
		{
			OpenGate();
			WaitForInFlightLoads();
		}

		// Registers a fake asset declaring `usage`, whose stored form takes `storedSize` bytes (0: unknown).
		AssetHandle Add(uint64_t handle, const AssetMemoryUsage& usage, uint64_t storedSize = 0)
		{
			AssetMetadata metadata;
			metadata.Handle = UUID(handle);
			metadata.Type = c_FakeAssetType;
			metadata.Path = "Fake/" + std::to_string(handle);
			metadata.Name = metadata.Path;
			metadata.StoredSize = storedSize;
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[metadata.Handle] = EncodeFakeAsset(usage);
			}
			RegisterAsset(metadata);
			return metadata.Handle;
		}

		void Remove(AssetHandle handle) { UnregisterAsset(handle); }

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
		mutable std::mutex m_Mutex;
		std::condition_variable m_Condition;
		std::unordered_map<AssetHandle, std::vector<uint8_t>> m_Data;
		std::vector<AssetHandle> m_ReadOrder;
		bool m_GateClosed = false;
		uint32_t m_AllowedReads = 0;
		uint32_t m_WaitingReads = 0;
	};

	inline AssetMemoryUsage MakeUsage(uint64_t cpu, uint64_t gpuTextures = 0, uint64_t gpuBuffers = 0)
	{
		return AssetMemoryUsage { cpu, gpuTextures, gpuBuffers };
	}

}
