#pragma once

namespace Strata
{

	// Registers the animation module with the engine's registries: the Skeleton, AnimationClip and AnimationController
	// loaders. Called by Engine::RegisterBuiltinModules only, after the scripting and physics modules.
	void RegisterAnimationModule();

}
