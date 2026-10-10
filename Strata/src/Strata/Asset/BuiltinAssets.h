#pragma once

#include "Strata/Asset/Asset.h"

#include <functional>
#include <span>
#include <string_view>

namespace Strata
{

	class AssetManagerBase;

	struct BuiltinAssetInfo
	{
		AssetHandle Handle;
		AssetType Type;
		std::string_view Name; // Also the asset path: "Builtin/<Name>"
	};

	// Creates the object of one built-in asset. Called for every asset manager, which owns what it gets.
	using BuiltinAssetFactory = std::function<Ref<Asset>()>;

	// Assets every project has, with fixed handles (UUIDs 1-255 are reserved for them). Scenes and scripts may
	// reference these handles directly, e.g. a MeshRenderer using BuiltinAssets::CubeMesh.
	class BuiltinAssets
	{
	public:
		static constexpr AssetHandle CubeMesh = UUID(0x01);
		static constexpr AssetHandle SphereMesh = UUID(0x02);
		static constexpr AssetHandle PlaneMesh = UUID(0x03);
		static constexpr AssetHandle QuadMesh = UUID(0x04);
		static constexpr AssetHandle CylinderMesh = UUID(0x05);
		static constexpr AssetHandle CapsuleMesh = UUID(0x06);
		static constexpr AssetHandle ConeMesh = UUID(0x07);
		static constexpr AssetHandle TorusMesh = UUID(0x08);
		static constexpr AssetHandle DefaultMaterial = UUID(0x10);

		static bool IsBuiltin(AssetHandle handle) { return IsBuiltinAssetHandle(handle); }
		static std::span<const BuiltinAssetInfo> GetAll();

		// Opens the factory registry (Engine::RegisterBuiltinModules). Creating an asset manager before that fails
		// ST_CORE_VERIFY.
		static void BeginRegistration();
		// The module that owns a built-in asset's type provides its object (the renderer module: the primitive meshes and
		// the default material), so that the asset layer depends on no module. A later factory replaces an earlier one.
		// Returns false (and logs) for a handle that is not one of GetAll or an empty factory.
		static bool RegisterFactory(AssetHandle handle, BuiltinAssetFactory factory);

		// Adds every built-in asset that has a factory to the manager as a memory asset.
		static void Register(AssetManagerBase& manager);
	};

}
