#pragma once

#include "Strata/Asset/AssetImporter.h"

namespace Strata
{

	// Imports glTF 2.0 models (".gltf" with external or embedded buffers, and binary ".glb").
	//
	// The model asset is an entity hierarchy (one entity per node: transform, mesh renderer, camera, punctual light)
	// that instantiates into scenes. Sub-assets: "Mesh/<mesh index>" (one Mesh per glTF mesh, one submesh per
	// primitive, with LODs), "Material/<material index>" (PBR metallic-roughness) and
	// "Texture/<image index>/<usage>/<sampler>" (decoded per use, so color and data textures get the right color
	// space). glTF and Strata share conventions (right-handed, +Y up, counter-clockwise front faces, meters), so no
	// conversion is applied besides the optional uniform scale.
	//
	// Settings: { "Scale": 1.0, "GenerateLODs": true, "LODCount": 3, "OptimizeMeshes": true, "ImportMaterials": true,
	// "ImportCameras": true, "ImportLights": true, "MaxTextureSize": 0 }.
	// Skins, animations, morph targets and vertex colors are not imported (a warning names what was skipped).
	// Files referenced by the model must lie inside the asset directory; changing them re-imports the model.
	class GltfImporter final : public AssetImporter
	{
	public:
		AssetType GetType() const override { return AssetType::Model; }
		std::vector<std::string> GetExtensions() const override { return { ".gltf", ".glb" }; }
		uint32_t GetVersion() const override { return 1; }
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;
	};

}
