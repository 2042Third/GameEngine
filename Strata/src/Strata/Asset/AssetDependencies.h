#pragma once

#include "Strata/Asset/Asset.h"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Strata
{

	// Finds the assets a stored form references (asset pipeline: the editor records what it finds per asset, see
	// EditorAssetManager, and packs carry it as their dependency table). Thread-safe.
	namespace AssetDependencies
	{

		// The references of the document types the engine writes:
		// - scenes, prefabs and models: every asset property of every registered component of their entities
		//   (ComponentRegistry; e.g. meshes, materials, environment maps, fonts, audio clips, prefab links) and every script
		//   field of asset type;
		// - materials: their texture maps;
		// - animation controllers: the clips of their states.
		// Other types reference nothing in their stored form: importers that know more (a glTF mesh's materials) record it
		// themselves (ImportResult::References). Appends each handle once, in the order it first appears, without null
		// handles. False, with the reason, when the document cannot be read. Scenes, prefabs and models are read as streams
		// (SceneDocumentReader: one entity's DOM at a time, whatever the document's size) and must be documents of their kind.
		bool Scan(AssetType type, std::span<const uint8_t> storedForm, std::vector<AssetHandle>& outReferences, std::string* outError = nullptr);
		// Scan of a document that is parsed already (what a serializer produced, before it is written): no parsing again.
		bool ScanDocument(AssetType type, const nlohmann::json& document, std::vector<AssetHandle>& outReferences, std::string* outError = nullptr);
		// Whether Scan reads the type's stored form (a document type), rather than finding nothing.
		bool CanReference(AssetType type);

		// The references of the entities of a snapshot ({"Entities": [...]}, SceneSerializer's format), appended like Scan's.
		void ScanEntities(const nlohmann::json& snapshot, std::vector<AssetHandle>& outReferences);

		// References as Scan appends them: each valid handle once, in the order it first appears (the handles the vector holds
		// already count as seen).
		class ReferenceList
		{
		public:
			explicit ReferenceList(std::vector<AssetHandle>& references);
			void Add(AssetHandle handle);
		private:
			std::vector<AssetHandle>& m_References;
			std::unordered_set<AssetHandle> m_Seen;
		};
		// The references of one entity object of a document of entities, by Scan's rules: for readers that see the entities
		// one at a time (AssetPackCook).
		void ScanEntity(const nlohmann::json& entity, ReferenceList& references);

	}

}
