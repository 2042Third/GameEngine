#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"

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

		// Approximate memory held by the asset (CPU + GPU), used for streaming budgets and statistics.
		virtual uint64_t GetMemoryUsage() const { return 0; }

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

		bool IsSubAsset() const { return Parent.IsValid(); }
		bool IsBuiltin() const { return IsBuiltinAssetHandle(Handle); }
	};

}
