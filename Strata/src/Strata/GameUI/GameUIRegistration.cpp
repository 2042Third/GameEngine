#include "stpch.h"
#include "Strata/GameUI/GameUIRegistration.h"

#include "Strata/Core/Assert.h"
#include "Strata/GameUI/GameUIRenderer.h"
#include "Strata/GameUI/GameUISystem.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	void RegisterGameUIModule()
	{
		RegisterGameUIComponents();
		// Every SceneRenderer draws the game UI over the finished image (Renderer/SceneOverlay.h).
		RegisterGameUIRenderer();

		// The UI's input runs before the scripts in Play mode, so that their OnUpdate sees the frame's widget events and knows
		// whether the pointer was over the UI (Input's pointer capture). A soft constraint: the game UI works in an engine
		// without the scripting module.
		SceneSystemDescriptor descriptor = MakeSceneSystemDescriptor<GameUISystem>("UI", false, [](Scene& scene) { return CreateScope<GameUISystem>(scene); });
		descriptor.BeforeIfPresent = { "Scripting" };
		descriptor.ProfileStage = "Scene.UI";
		const bool registered = SceneSystemRegistry::Register(std::move(descriptor));
		ST_CORE_VERIFY(registered, "The game UI module could not register its scene system");
	}

}
