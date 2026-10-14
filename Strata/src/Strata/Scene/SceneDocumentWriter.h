#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Core/DocumentOutput.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EntityPrototype;
	class Scene;
	struct ComponentInfo;

	// The documents of entity hierarchies the engine writes: their header names the format, and their content object
	// ("Scene", "Prefab", "Model") holds the entity list.
	enum class SceneDocumentKind : uint8_t
	{
		Scene = 0, // {"Strata": {"Format": "Scene", ...}, "Scene": {"Name", "Settings", "EntityCount", "Entities"}}
		Prefab,    // {"Strata": {"Format": "Prefab", ...}, "Prefab": {"EntityCount", "Entities"}}
		Model      // {"Strata": {"Format": "Model", ...}, "Model": {"EntityCount", "Entities"}}
	};

	const char* SceneDocumentKindToString(SceneDocumentKind kind);

	struct SceneWriteOptions
	{
		SceneDocumentKind Kind = SceneDocumentKind::Scene;
		// The entities to write: those with these IDs, each with its descendants (a root below another listed one is written
		// as part of that one; IDs the scene does not have are left out), in the order given. Empty: the whole scene. An
		// entity's parent is written only when it is written too: the subset's top-level entities have none (cells of a world
		// are written this way, from a snapshot, which keeps the IDs).
		std::vector<UUID> Roots;
		// Scene documents: write the scene's settings (cells of a world use their host scene's and write none).
		bool WriteSettings = true;
		// Scene documents: the name written instead of the scene's.
		std::optional<std::string> Name;
		// Asked between chunks of entities: true ends the write (it fails as cancelled).
		std::function<bool()> Cancel;
		// The share of the work done so far, in [0, 1] (serializing and writing the entities, each about half), after every chunk
		// serialized and every batch written: from the job threads, one call at a time, each with a larger share than the last.
		std::function<void(float share)> Progress;
		// Entities serialized by one job, in parallel on the job system; chunks are written in order, so the bytes never
		// depend on the thread count.
		uint32_t ChunkEntities = 4096;
		// Of the jobs that serialize chunks and write them (background work: saves no frame waits for).
		JobPriority Priority = JobPriority::Background;
		// What the extra data of a component (ComponentInfo::SerializeExtra: data that is no property, such as script fields)
		// references: called, on the serializing threads, with each such component's JSON, appending the assets it names. Without
		// it, only asset properties count (SceneWriteResult::References).
		std::function<void(const ComponentInfo& info, const nlohmann::json& component, std::vector<AssetHandle>& outReferences)> ExtraReferences;
		// Components not written (a prefab stores no prefab links: its instances do).
		std::vector<const ComponentInfo*> LeaveOut;
	};

	struct SceneWriteResult
	{
		uint64_t Entities = 0;
		uint64_t Bytes = 0;
		// Every asset the written entities reference (asset properties, and extra data through SceneWriteOptions::ExtraReferences),
		// each valid handle once, in the order it first appears: what AssetDependencies::Scan finds in the bytes.
		std::vector<AssetHandle> References;
		Sha256Digest Sha256 = {};  // Of the bytes written (the recovery journal's base)
		uint64_t ContentHash = 0;  // Hash::FNV1a of the bytes written (the import records' "SourceHash")
		bool Cancelled = false;    // SceneWriteOptions::Cancel ended it
	};

	// Writes scene, prefab and model documents as a stream, without building the document: one entity per line, so that a
	// document of a million entities never exists in memory and files diff entity by entity. The layout (the format and its
	// version are SceneSerializer's):
	//
	//   {"Strata":{"Format":"Scene","Version":1},"Scene":{"Name":"...","Settings":{...},"EntityCount":N,"Entities":[
	//   {"Components":{...},"ID":"...","Parent":"..."},
	//   ...
	//   {"Components":{...},"ID":"..."}
	//   ]}}
	//
	// Each entity line (without its comma) is byte for byte JsonUtils::Dump of the entity object SceneSerializer builds
	// (components and properties sorted by name as nlohmann sorts them, floats in FloatToJson's form), in hierarchy order
	// (parents first, siblings in order). EntityCount lets readers refuse a document over the entity limit at once and size
	// their tables; older readers ignore it. Entities are serialized in parallel chunks (SceneWriteOptions::ChunkEntities)
	// and written in order while the next chunks are serialized.
	//
	// Any thread, while nothing changes the scene: saves write a snapshot of the edited scene (Scene::Copy with
	// SceneCopyOptions::SerializedOnly) on a background job.
	class SceneDocumentWriter
	{
	public:
		// Writes the document into `output`. False with the reason when the output fails or the write is cancelled
		// (outResult->Cancelled); the output then holds part of a document.
		static bool Write(const Scene& scene, DocumentOutput& output, const SceneWriteOptions& options = {}, SceneWriteResult* outResult = nullptr,
			std::string* outError = nullptr);
		// Writes a prototype's entities (a prefab's or a model's: options.Kind is Prefab or Model; Roots, WriteSettings and Name
		// do not apply) in the same layout: what Write writes of a scene that holds them, in the prototype's order.
		static bool WritePrototype(const EntityPrototype& prototype, DocumentOutput& output, const SceneWriteOptions& options, SceneWriteResult* outResult = nullptr,
			std::string* outError = nullptr);
		// Lays a parsed prefab or model document ({"Strata": header, "<Kind>": {"Entities": [...]}}) out in the writer's
		// layout, its entities as they are (each line is JsonUtils::Dump of its entity object): for documents edited as JSON
		// (prefab.apply). Keys of the content object besides the entity list are not written. False, with the reason, when it
		// is no such document.
		static bool WriteJson(const nlohmann::json& document, SceneDocumentKind kind, DocumentOutput& output, std::string* outError = nullptr);

		// The entities serialized on the calling thread so far: tests check that a thread (the main thread, while a save runs
		// in the background) serialized none.
		static uint64_t GetThreadEntityCount();
	};

}
