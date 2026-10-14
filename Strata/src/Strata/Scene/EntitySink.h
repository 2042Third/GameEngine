#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/SceneSerializer.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <span>
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

	// Entities read from their JSON objects into columns rather than a registry: per entity its ID, its parent and its
	// components, each component's value in an array of its type (ComponentValueArray), and the components this build does
	// not register as JSON. An entity is read as SceneSerializer::DeserializeEntities reads it: a Name ("Entity") and a
	// Transform whatever its JSON holds, then each registered component of its "Components" (ComponentAccess::Deserialize,
	// not strict: what cannot be read as written is reported). EntityPrototypeBuilder builds prototypes of them. Any thread:
	// nothing is shared.
	class EntityColumns
	{
	public:
		static constexpr uint32_t c_NoUnknownComponents = std::numeric_limits<uint32_t>::max();

		struct EntityRecord
		{
			UUID ID;
			UUID Parent = UUID::Null(); // Null: none (or InvalidParent)
			bool InvalidParent = false;  // Its "Parent" is not an ID
			uint32_t FirstComponent = 0;
			uint32_t ComponentCount = 0;
			uint32_t Unknown = c_NoUnknownComponents; // Index of its unknown components (GetUnknown)
		};

		struct ComponentRecord
		{
			const ComponentInfo* Info = nullptr;
			uint32_t Array = 0;  // Index of the value array of its type (GetArray)
			size_t Value = 0;    // Index of the value in that array
		};

		EntityColumns();

		// Reads one entity object. False, with the error, when it cannot be read (not an object, an invalid "ID"): nothing is
		// added then. What it reads with problems is appended to outWarnings (when given), its unknown components counted in
		// `tally`.
		bool Add(const nlohmann::json& entity, std::vector<std::string>* outWarnings, Detail::UnknownComponentTally& tally, std::string* outError);
		void Reserve(size_t entities);

		size_t GetEntityCount() const { return m_Entities.size(); }
		const EntityRecord& GetEntity(size_t index) const { return m_Entities[index]; }
		std::span<const ComponentRecord> GetComponents(size_t index) const;
		// A component's value (an object of its type): valid until the next Add.
		void* GetValue(const ComponentRecord& component) const;
		nlohmann::json& GetUnknown(uint32_t index) { return m_Unknown[index]; }
		// The value arrays, one per component type the entities have; a builder takes them over once every entity is added.
		std::vector<Scope<ComponentValueArray>>& GetArrays() { return m_Arrays; }
	private:
		uint32_t FindArray(const ComponentInfo& info);
		void* AddComponent(const ComponentInfo& info);
	private:
		const ComponentInfo* m_NameInfo = nullptr;
		const ComponentInfo* m_TransformInfo = nullptr;
		std::vector<EntityRecord> m_Entities;
		std::vector<ComponentRecord> m_Components;
		std::vector<Scope<ComponentValueArray>> m_Arrays;
		std::vector<const ComponentInfo*> m_ArrayInfos; // The type of each array
		std::vector<nlohmann::json> m_Unknown;
	};

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
		// Creates an entity with the components every entity has besides Name and Transform; false when its ID is taken.
		bool CreateEntity(UUID id, UUID parent, bool invalidParent, entt::entity& outHandle, std::string* outError);
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

	// Builds an EntityPrototype of a document's entities as they are read (EntityColumns), without a scene: prefabs, models
	// and snapshots (EntityPrototype::FromSnapshot). The entities keep their IDs (a document holding an ID twice is refused)
	// and are ordered as EntityPrototype::Compile orders a scene's: in hierarchy order, parents first and siblings in
	// document order, linked with SceneSerializer::DeserializeEntities' rules and warnings (SceneBuilder's).
	class EntityPrototypeBuilder final : public EntitySink
	{
	public:
		// `outWarnings` receives, at Finish, what the document holds that is not read as written: the entities', then the
		// unknown components', then the parents'.
		explicit EntityPrototypeBuilder(std::vector<std::string>* outWarnings);

		void ReserveEntities(uint64_t count) override;
		bool ReadEntity(const nlohmann::json& entity, std::string* outError) override;
		bool Finish(std::string* outError) override;

		// The prototype once Finish succeeded (null otherwise); the first call takes it.
		Ref<EntityPrototype> TakePrototype() { return std::move(m_Prototype); }
	private:
		std::vector<std::string>* m_Warnings = nullptr;
		std::vector<std::string> m_EntityWarnings;
		EntityColumns m_Columns;
		Detail::UnknownComponentTally m_Unknown;
		std::unordered_map<UUID, uint32_t> m_IndexByID; // Document index of each entity
		Ref<EntityPrototype> m_Prototype;
	};

}
