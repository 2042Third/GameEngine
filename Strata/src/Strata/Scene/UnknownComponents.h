#pragma once

#include <nlohmann/json.hpp>

namespace Strata
{

	// The components of an entity whose names this build does not register - those of a module the build lacks, or of a
	// newer engine - kept verbatim as {"<component name>": <its JSON>, ...}, so that loading and saving a scene, prefab or
	// undo snapshot never loses them. Runtime-only and not registered with the ComponentRegistry: SceneSerializer creates
	// it when it reads such components and writes its entries back as components; Scene::Copy, prefab snapshots,
	// DuplicateEntity and the editor's undo snapshots carry it.
	struct UnknownComponentsComponent
	{
		nlohmann::json Components = nlohmann::json::object();

		// Adds the kept components to `components`, an entity's serialized components (ComponentAccess::
		// SerializeEntityComponents).
		void AppendTo(nlohmann::json& components) const
		{
			if (!Components.is_object() || !components.is_object())
				return;
			for (const auto& [name, component] : Components.items())
				components[name] = component;
		}
	};

}
