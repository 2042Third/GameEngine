#pragma once

#include "Strata/Asset/Asset.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace Strata
{

	class AssetManagerBase;

	// The memory pools resident assets are accounted in (see AssetMemoryUsage), each with its own budget.
	enum class AssetMemoryPool : uint8_t
	{
		Cpu = 0,
		GpuTextures,
		GpuBuffers
	};

	constexpr std::array<AssetMemoryPool, 3> c_AssetMemoryPools = { AssetMemoryPool::Cpu, AssetMemoryPool::GpuTextures, AssetMemoryPool::GpuBuffers };

	uint64_t GetPoolBytes(const AssetMemoryUsage& usage, AssetMemoryPool pool);

	// How much an asset manager keeps resident and how fast it streams.
	//
	// Pool budgets: after each update, the manager evicts the least recently requested assets of a pool that is over its
	// budget, until the pool fits or nothing more may go (see AssetManagerBase::Update). Pinned assets, assets requested
	// within the eviction grace window (AssetManagerBase::GetEvictionGraceFrames), memory and built-in assets and assets
	// in use elsewhere (Asset::IsDataShared, or an object still referenced outside the manager) stay, so a pool can exceed
	// its budget by what those hold.
	struct AssetResidencyBudgets
	{
		static constexpr uint64_t c_Unlimited = std::numeric_limits<uint64_t>::max();
		// Shares of the graphics device's memory budget (VK_EXT_memory_budget) the default GPU budgets take.
		static constexpr double c_DefaultGpuTextureShare = 0.5;
		static constexpr double c_DefaultGpuBufferShare = 0.15;

		uint64_t GpuTextures = c_Unlimited;
		uint64_t GpuBuffers = c_Unlimited;
		uint64_t Cpu = c_Unlimited;
		// Bytes of loads that were dispatched and not finalized yet (read, decoded or waiting for the main thread), counted
		// by their stored size (see AssetStreamingQueue).
		uint64_t InFlightBytes = 128ull << 20;
		// GPU bytes finalization uploads per frame; the first finalization of a frame always runs.
		uint64_t UploadBytesPerFrame = 64ull << 20;
		// Main-thread time finalization may take per frame, in milliseconds; the first finalization of a frame always runs.
		float FinalizeMsPerFrame = 4.0f;

		// The defaults for a graphics device whose memory budget is deviceBudgetBytes: GPU textures may use 50% and GPU
		// buffers 15% of it. 0 (no device) leaves the GPU pools unlimited. The CPU pool is unlimited either way.
		static AssetResidencyBudgets FromDeviceBudget(uint64_t deviceBudgetBytes);

		uint64_t GetPoolBudget(AssetMemoryPool pool) const;
	};

	namespace AssetResidency
	{
		// Frames after a scene switch until its owner trims what the previous scene used (AssetManagerBase::ScheduleTrim),
		// and how recently an asset must have been requested to stay: the new scene has rendered and requested what it
		// uses by then.
		constexpr uint32_t c_SceneSwitchTrimFrames = 3;
	}

	// Keeps an asset resident while it lives: pinned assets are never evicted for budgets or trims (an explicit unload
	// still unloads them, and the next request loads them again). Created by AssetManagerBase::Pin; movable, not copyable.
	// A pin outliving its asset manager does nothing. Pins of one asset add up: it stays pinned until the last is released.
	class AssetPin
	{
	public:
		AssetPin() = default;
		~AssetPin();

		AssetPin(AssetPin&& other) noexcept;
		AssetPin& operator=(AssetPin&& other) noexcept;
		AssetPin(const AssetPin&) = delete;
		AssetPin& operator=(const AssetPin&) = delete;

		bool IsValid() const { return m_Handle.IsValid(); }
		AssetHandle GetHandle() const { return m_Handle; }
		// Releases the pin now.
		void Reset();
	private:
		friend class AssetManagerBase;
		AssetPin(std::weak_ptr<AssetManagerBase> manager, AssetHandle handle, uint64_t registration);
	private:
		std::weak_ptr<AssetManagerBase> m_Manager;
		AssetHandle m_Handle = UUID::Null();
		uint64_t m_Registration = 0; // The asset's registration the pin counts in (a re-registered asset starts unpinned)
	};

}
