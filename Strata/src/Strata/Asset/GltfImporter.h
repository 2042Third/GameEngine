#pragma once

#include "Strata/Asset/AssetImporter.h"

namespace Strata
{

	// Imports glTF 2.0 models (".gltf" with external or embedded buffers, and binary ".glb").
	//
	// The model asset is an entity hierarchy (one entity per node: transform, mesh renderer, camera, punctual light)
	// that instantiates into scenes. Sub-assets: "Mesh/<mesh index>" (one Mesh per glTF mesh, one submesh per
	// primitive, with LODs; a mesh that nodes draw with several skins, or with and without one, gets one more per use:
	// "Mesh/<mesh index>/Skin/<skin index>" or ".../Rigid"), "Material/<material index>" (PBR metallic-roughness),
	// "Texture/<image index>/<usage>/<sampler>" (cooked per use like TextureImporter's textures: base color and emissive
	// as color, the normal texture as a normal map, metallic-roughness and occlusion as data), "Skeleton" (the joints of
	// every skin and every node an animation moves, with their ancestors; see Skeleton) and "Animation/<name>" (one
	// AnimationClip per animation; "Animation/<index>" for animations without a unique name). Meshes drawn with a skin
	// carry its binding (Mesh::CreateSkinned: the four strongest influences per vertex, the skin's joints in the skeleton
	// and its inverse bind matrices) and are placed under the model's root, since glTF ignores the transform of the node
	// that draws them. glTF and Strata share conventions (right-handed, +Y up, counter-clockwise front faces, meters), so
	// no conversion is applied besides the optional uniform scale.
	//
	// Settings: { "Scale": 1.0, "GenerateLODs": true, "LODCount": 3, "OptimizeMeshes": true, "ImportMaterials": true,
	// "ImportCameras": true, "ImportLights": true, "MaxTextureSize": 0, "TextureCompression": "Auto",
	// "TextureQuality": "High" } (the last two as TextureImportSettings' Compression and Quality).
	// Morph targets, their animation and vertex colors are not imported (a warning names what was skipped).
	// Files referenced by the model must lie inside the asset directory; changing them re-imports the model. Buffer views
	// compressed with EXT_meshopt_compression are decoded with meshoptimizer's decoders and filters.
	class GltfImporter final : public AssetImporter
	{
	public:
		AssetType GetType() const override { return AssetType::Model; }
		std::vector<std::string> GetExtensions() const override { return { ".gltf", ".glb" }; }
		// 6: skins, animations and skinned meshes; 7: buffer views compressed with EXT_meshopt_compression too.
		uint32_t GetVersion() const override { return 7; }
		bool ReadsExternalFiles() const override { return true; }
		// The URIs of the model's buffers and images that name files (data URIs and buffers in a .glb are no files); fails for
		// URLs and absolute paths.
		bool FindExternalFiles(std::span<const uint8_t> data, std::vector<std::string>& outRelativePaths, std::string* outError) const override;
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		std::vector<PropertyInfo> GetSettingsProperties() const override;
		// LODCount needs GenerateLODs, the texture settings ImportMaterials, TextureQuality compressed textures.
		std::string GetInactiveReason(std::string_view setting, const nlohmann::json& settings) const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;
		// From the document (a .glb's JSON chunk, a .gltf; read past the head up to c_MaxEstimatedDocumentBytes): its buffers,
		// twice (read and converted), and its images, embedded or files beside it, by their headers as textures count
		// (TextureImporter::EstimateImageMemory); four times the source's size when the document cannot be read, or more.
		uint64_t EstimateImportMemory(const ImportEstimateSource& source, const nlohmann::json& settings) const override;
		// The longest JSON document (a .gltf, a .glb's JSON chunk) an estimate reads; a longer one counts by its file's size.
		static constexpr uint64_t c_MaxEstimatedDocumentBytes = 64ull << 20;
	};

}
