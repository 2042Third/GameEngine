#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nvrhi
{
	class ICommandList;
}

namespace Strata
{

	class AssetManagerBase;

	enum class AssetState : uint8_t
	{
		Unloaded = 0,
		Loading,  // Queued or being read/decoded on worker threads
		Ready,
		Failed
	};

	enum class AssetPriority : uint8_t
	{
		High = 0, // Needed right now (visible, blocking gameplay)
		Normal,
		Low       // Prefetch
	};

	const char* AssetStateToString(AssetState state);

	// Handles 1-255 are reserved for the engine's built-in assets (see BuiltinAssets).
	constexpr uint64_t c_MaxBuiltinAssetHandle = 0xFF;

	inline bool IsBuiltinAssetHandle(AssetHandle handle)
	{
		return handle.IsValid() && static_cast<uint64_t>(handle) <= c_MaxBuiltinAssetHandle;
	}

	struct AssetFinalizeContext
	{
		nvrhi::ICommandList* CommandList = nullptr; // Upload command list; null when no renderer is running
		AssetManagerBase* Manager = nullptr;        // Manager publishing the asset (null for standalone assets)
	};

	// Memory an asset holds, per residency pool (see AssetResidencyBudgets): each pool has its own budget, because
	// running out of video memory and running out of system memory are different problems.
	struct AssetMemoryUsage
	{
		uint64_t Cpu = 0;         // System memory: decoded data kept on the CPU (geometry for physics, samples, documents)
		uint64_t GpuTextures = 0; // Video memory of textures
		uint64_t GpuBuffers = 0;  // Video memory of buffers (vertices, indices)

		uint64_t GetTotal() const { return Cpu + GpuTextures + GpuBuffers; }
		uint64_t GetGpu() const { return GpuTextures + GpuBuffers; }

		AssetMemoryUsage& operator+=(const AssetMemoryUsage& other)
		{
			Cpu += other.Cpu;
			GpuTextures += other.GpuTextures;
			GpuBuffers += other.GpuBuffers;
			return *this;
		}

		AssetMemoryUsage& operator-=(const AssetMemoryUsage& other)
		{
			Cpu -= other.Cpu;
			GpuTextures -= other.GpuTextures;
			GpuBuffers -= other.GpuBuffers;
			return *this;
		}

		bool operator==(const AssetMemoryUsage& other) const = default;
	};

	// Base class of every asset. Assets are shared (Ref<Asset>) and identified by their handle; code holds handles
	// and asks the asset manager for the current object, which allows hot reload and streaming to replace objects.
	class Asset
	{
	public:
		virtual ~Asset() = default;

		virtual AssetType GetType() const = 0;

		// Called on the main thread once the asset has been loaded, before it becomes Ready. GPU assets create their
		// GPU resources here; assets referencing others may request them from the context's manager. Must not block
		// on other loads (no LoadAssetSync). Return false on failure.
		virtual bool FinalizeOnMainThread(const AssetFinalizeContext&) { return true; }

		// Approximate memory held by the asset, per pool. Asset managers read it once the asset is finalized (for
		// residency budgets, upload budgets and statistics), so it must describe the asset's state from then on.
		virtual AssetMemoryUsage GetMemoryUsage() const { return {}; }

		// Whether objects outside the asset share its data, so that dropping the asset would free nothing (a playing
		// voice holds an audio clip's samples). Residency management keeps such assets resident.
		virtual bool IsDataShared() const { return false; }

		AssetHandle Handle = UUID::Null();
	};

	struct AssetMetadata
	{
		AssetHandle Handle = UUID::Null();
		AssetType Type = AssetType::None;
		// Path relative to the project's asset directory with '/' separators ("Models/Helmet.gltf"). Sub-assets share
		// their parent's path; built-in assets use "Builtin/<Name>".
		std::string Path;
		AssetHandle Parent = UUID::Null(); // Owning asset for sub-assets (e.g. meshes inside a model)
		std::string SubAssetKey;           // Identifies a sub-asset within its parent ("Mesh/0")
		std::string Name;                  // Display name
		// Bytes of the stored form a load reads (the cooked data, or the source file for assets stored as-is); 0 when
		// unknown. The streaming queue bounds the bytes of loads in flight with it.
		uint64_t StoredSize = 0;

		bool IsSubAsset() const { return Parent.IsValid(); }
		bool IsBuiltin() const { return IsBuiltinAssetHandle(Handle); }
	};

}
