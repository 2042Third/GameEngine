#pragma once

#include "Strata/Core/UUID.h"
#include "Strata/Scene/SceneSerializer.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;

	// What SceneDocumentReader hands a document's content to as it parses it: the small fields of its content object (a
	// scene's "Name" and "Settings") and each value of its entity list, one at a time, in document order. Each value is a
	// DOM of that value only, valid for the call: a reader of a million entities never holds more than one entity's DOM.
	class EntitySink
	{
	public:
		virtual ~EntitySink() = default;

		// The count the document declares (EntityCount) when it declares it before its entities, at most the reader's limit:
		// room to reserve. The entities that follow may be fewer (or more, up to the limit).
		virtual void ReserveEntities([[maybe_unused]] uint64_t count) {}
		// A field of the content object other than the entity list ("Name" and "Settings" of scenes), in any order, before or
		// after the entities.
		virtual void ReadField([[maybe_unused]] std::string_view key, [[maybe_unused]] const nlohmann::json& value) {}
		// One value of the entity list (an entity object when the document is valid). False, with the reason, ends the read.
		virtual bool ReadEntity(const nlohmann::json& entity, std::string* outError) = 0;
		// Once the whole document is read and its header validated. False, with the reason, fails the read.
		virtual bool Finish(std::string* outError) = 0;
	};

	namespace Detail
	{

		// The components of entities read so far that this build does not register: one warning per name, with the count of
		// entities, once all are read (SceneSerializer's rule).
		struct UnknownComponentTally
		{
			std::vector<std::string> Names; // In the order they were first seen
			std::unordered_map<std::string, size_t> Counts;

			void Add(const std::string& name);
			void AppendWarnings(std::vector<std::string>& outWarnings) const;
		};

	}

	// Builds a scene of a document's entities as they are read (SceneDocumentReader): each entity and its components are
	// created in the scene's registry at once (ComponentAccess::Deserialize per component, as
	// SceneSerializer::DeserializeEntities does), and only its entity and parent ID are kept until the end, which links the
	// hierarchy and signals the components (on_update) with DeserializeEntities' rules: entities may come before their
	// parents, an invalid parent, a missing one or one that closes a cycle attaches the entity at the top level with a
	// warning, and components this build does not register are kept (UnknownComponentsComponent) with one warning per name.
	// The scene must be empty; the entities keep their IDs (a document holding an ID twice is refused). The document's
	// "Name" and "Settings" fields name and set up the scene.
	class SceneBuilder final : public EntitySink
	{
	public:
		// `outWarnings` receives, at Finish, what the document holds that is not read as written, in
		// SceneSerializer::Deserialize's order (settings, entities, unknown components, parents). `linkProgress` hears of
		// Finish's work, from 0 to 1.
		SceneBuilder(Scene& scene, std::vector<std::string>* outWarnings, const EntityReadProgress& linkProgress = {});

		void ReserveEntities(uint64_t count) override;
		void ReadField(std::string_view key, const nlohmann::json& value) override;
		bool ReadEntity(const nlohmann::json& entity, std::string* outError) override;
		bool Finish(std::string* outError) override;
	private:
		Scene& m_Scene;
		std::vector<std::string>* m_Warnings = nullptr;
		EntityReadProgress m_LinkProgress;
		bool m_WasEmpty = true;
		bool m_HasName = false;
		std::vector<std::string> m_SettingsWarnings;
		std::vector<std::string> m_EntityWarnings;
		// Per entity, in document order: its entity and its parent's ID (null: none). The document indices of entities whose
		// "Parent" is not an ID.
		std::vector<entt::entity> m_Handles;
		std::vector<UUID> m_Parents;
		std::vector<uint32_t> m_InvalidParents;
		Detail::UnknownComponentTally m_Unknown;
	};

}
