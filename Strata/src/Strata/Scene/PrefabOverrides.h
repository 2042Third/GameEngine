#pragma once

#include "Strata/Core/UUID.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;

	// Per-property overrides of prefab instances: what an instance changed relative to its part of the prefab
	// (PrefabInstances::ExtractTemplate), by the prefab's entity ids, component and property names, as SceneSerializer
	// writes them (values compared as JSON; references between the instance's entities compare as the prefab entities they
	// were made from). The instance root's transform and name are the instance's own and never overrides; entities the
	// instance added (not linked to the prefab) are its own too. The list lives on the instance root
	// (PrefabInstanceComponent::Overrides): the editor keeps it as edits happen, and prefab changes reach every instance
	// except where it overrides them (Propagate).
	namespace PrefabOverrides
	{

		// The registered name scripts are stored under; their fields are overridden one by one ("<Class>.<Field>").
		constexpr std::string_view c_ScriptComponent = "Script";

		const char* KindToString(PrefabOverrideKind kind);
		std::optional<PrefabOverrideKind> KindFromString(std::string_view text);

		// The list as scenes store it ([{Entity, Component, Property, Kind}]) and back. Reading drops malformed entries and
		// unknown kinds, each named in outWarnings.
		nlohmann::json ToJson(const std::vector<PrefabOverride>& overrides);
		std::vector<PrefabOverride> FromJson(const nlohmann::json& json, std::vector<std::string>* outWarnings = nullptr);

		// How the instance at `root` differs from its part of the prefab (`part`: {"Entities": [...]}, its template root
		// first), in the part's entity order then component and property order.
		std::vector<PrefabOverride> ComputeOverrides(const Scene& scene, Entity root, const nlohmann::json& part);
		// The same from the instance's template (PrefabInstances::MakeTemplate, its root's own transform and name or the
		// prefab's: the root's are left out of the comparison).
		std::vector<PrefabOverride> ComputeOverrides(const nlohmann::json& instancePart, const nlohmann::json& part);
		// Whether the list overrides a property (or the whole component, or the entity).
		bool IsOverridden(const std::vector<PrefabOverride>& overrides, UUID entity, std::string_view component, std::string_view property = {});

		// The parts of a prefab's snapshots before and after a change, for the instances made from them: each part extracted
		// (PrefabInstances::ExtractTemplate) and the new one's version (PrefabInstances::GetPartVersion) worked out once,
		// however many instances share it. Without `previous`, Previous stays null. Both snapshots must outlive the cache.
		class PartChangeCache
		{
		public:
			struct Parts
			{
				nlohmann::json Previous;
				nlohmann::json Next;
				uint64_t NextVersion = 0;
			};

			PartChangeCache(const nlohmann::json* previous, const nlohmann::json& next);
			const Parts& Get(UUID templateRoot);
		private:
			const nlohmann::json* m_Previous;
			const nlohmann::json& m_Next;
			std::unordered_map<UUID, Parts> m_Parts;
		};

		// What Propagate did to one instance.
		struct PropagateResult
		{
			std::vector<UUID> Changed;  // The entities that changed (and are still in the scene), the root among them when its list did
			std::vector<UUID> Created;  // The entities it made
			uint64_t ContentHash = 0;   // The instance's PrefabInstances::GetContentHash afterwards, for MarkSynced
		};

		// Gives the instance at `root` what changed in its part of the prefab from `previousPart` to `nextPart`, except where it
		// overrides it: properties take their new values (set through reflection, signaled like any edit), components and
		// entities the prefab added are made unless a Removed override says otherwise, ones it removed go unless the instance
		// added them. The overrides are its recorded list, or (none recorded) how it differs from `previousPart`; afterwards
		// the list is worked out against `nextPart`. `beforeChange` is called with each entity before it changes or goes (undo
		// tracking). The root's transform and name stay. The instance is serialized at most twice (before, when no list is
		// recorded, and after, when something changed): callers mark it synced with outResult.ContentHash instead of hashing
		// it again. False, with the reason, when `nextPart` lacks the root's template or the scene cannot hold what it adds.
		bool Propagate(Scene& scene, Entity root, const nlohmann::json& previousPart, const nlohmann::json& nextPart, PropagateResult& outResult,
			const std::function<void(UUID)>& beforeChange = {}, std::string* outError = nullptr);

		// Gives one entity of an instance its prefab entity's value of a component (absent there: the component goes), or of one
		// property (`property` not empty; a script: "<Class>" for a whole script, "<Class>.<Field>" for a field). False, with
		// the reason, when the part lacks the entity, or the property where the component exists in both.
		bool Revert(Scene& scene, Entity entity, const nlohmann::json& part, std::string_view component, std::string_view property, std::string* outError = nullptr);
		// The part with one entity's component (or one property of it) taken from the instance: what prefab.apply writes for a
		// single override. `instancePart` is the instance as a template (PrefabInstances::MakeTemplate).
		nlohmann::json ApplyToPart(const nlohmann::json& part, const nlohmann::json& instancePart, UUID entity, std::string_view component, std::string_view property);

	}

}
