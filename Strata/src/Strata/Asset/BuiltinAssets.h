#pragma once

#include "Strata/Asset/Asset.h"

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

		// Adds every built-in asset to the manager as a memory asset.
		static void Register(AssetManagerBase& manager);
	};

}
