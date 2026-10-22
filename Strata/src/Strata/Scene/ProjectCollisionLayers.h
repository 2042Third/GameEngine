#pragma once

#include "Strata/Scene/PhysicsLayerSettings.h"

#include <cstdint>

namespace Strata
{

	struct SettingsSection;

	// The project's collision layers: the project settings section Scene.CollisionLayers (registered by RegisterSceneModule,
	// so a build or game without physics keeps the names), whose value is PhysicsLayerSettings' stored form. Every scene works
	// on a copy of the table (Scene::GetPhysicsLayers), taken on the main thread when the scene is handed over
	// (Scene::ApplyProjectLayers), so a layer's name means the same in every scene and prefab. Docs/DeveloperGuide.md,
	// "Collision layers".
	namespace ProjectCollisionLayers
	{

		inline constexpr const char* c_SectionId = "Scene.CollisionLayers";

		// The section as RegisterSceneModule registers it: a complex section whose Read refuses a value with anything that is
		// not a valid name or pair (PhysicsLayerSettings::FromJson's problems, members other than Names and NoCollisions) and
		// returns the stored form ToJson writes.
		SettingsSection MakeSection();

		// The project's table as the engine service IProjectSettings provides it (the section's default when the project does
		// not set it), parsed again only when the provider's version changes.
		struct Table
		{
			PhysicsLayerSettings Layers;
			uint64_t Version = 0; // Changes only when the table itself does; never 0
		};

		// Null when nothing provides IProjectSettings: a program that runs no project (tools, tests, a game exported before
		// 0.6.0), whose scenes keep the layers their documents store. Main thread only, like the service; the pointer is valid
		// until the next call.
		const Table* Find();

	}

}
