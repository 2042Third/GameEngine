#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	// Receives the share of reading a document's entities done so far, in [0, 1] (SceneSerializer::Deserialize,
	// DeserializeEntities), on the reading thread: every few thousand entities of each pass over them, and 1 once all of
	// them exist. Reading a million entities takes seconds.
	using EntityReadProgress = std::function<void(float share)>;

	struct EntityInstantiationOptions
	{
		// Give every created entity a new UUID and remap entity references between the created entities.
		bool GenerateNewUUIDs = true;
		// Parent for the top-level entities of the snapshot (invalid: scene roots).
		Entity Parent;
		// When set, every created entity gets a PrefabInstanceComponent linking it to this prefab.
		AssetHandle SourcePrefab = UUID::Null();
		// Warn (once per name) about components this build does not register. Off for data the scene wrote itself, such as
		// the editor's undo snapshots, where they are no news.
		bool ReportUnknownComponents = true;
	};

	// Scene and entity serialization to versioned JSON.
	//
	// Format (scene):
	//   { "Strata": { "Format": "Scene", "Version": 1 },
	//     "Scene": { "Name": "...", "Settings": {...},
	//                "Entities": [ { "ID": "<uuid>", "Parent": "<uuid>", "Components": { "Transform": {...}, ... } } ] } }
	// Entities are listed in depth-first hierarchy order (parents before children), so sibling order survives a
	// round trip. Unknown properties are skipped with warnings, so newer files still load. Components whose names this
	// build does not register (a module it lacks, a newer engine) are kept verbatim in an UnknownComponentsComponent and
	// written back unchanged, with one warning per name per load.
	class SceneSerializer
	{
	public:
		static constexpr uint32_t c_FormatVersion = 1;

		static nlohmann::json Serialize(const Scene& scene);
		// Loads into an empty scene. Returns false (with an error) for structurally invalid documents.
		static bool Deserialize(Scene& scene, const nlohmann::json& json, std::string* outError = nullptr, std::vector<std::string>* outWarnings = nullptr,
			const EntityReadProgress& progress = {});
		// How many scene documents Deserialize has read in this process (any thread): loading a scene asset reads its document
		// once, whoever takes the scene (SceneAsset::TakePreparedScene), and tests check it with the difference of two calls.
		static uint64_t GetDeserializeCount();

		static bool SaveToFile(const Scene& scene, const std::filesystem::path& path, std::string* outError = nullptr);
		static Ref<Scene> LoadFromFile(const std::filesystem::path& path, std::string* outError = nullptr);

		// Snapshot of entity hierarchies: {"Entities": [...]} containing each root and all its descendants. Roots below another
		// listed root are part of its subtree; invalid roots and roots of other scenes are left out.
		static nlohmann::json SerializeEntities(const Scene& scene, const std::vector<Entity>& roots);
		// Instantiates a snapshot produced by SerializeEntities (or the "Entities" of a scene/prefab document).
		// Returns the created top-level entities in order.
		static std::vector<Entity> DeserializeEntities(Scene& scene, const nlohmann::json& json, const EntityInstantiationOptions& options,
			std::string* outError = nullptr, std::vector<std::string>* outWarnings = nullptr, const EntityReadProgress& progress = {});
	};

}
