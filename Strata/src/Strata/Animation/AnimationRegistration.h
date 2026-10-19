#pragma once

namespace Strata
{

	// Registers the animation module with the engine's registries: its components, the Skeleton, AnimationClip and
	// AnimationController loaders and the "Animation" scene system (AnimationSystem). Called by
	// Engine::RegisterBuiltinModules only.
	void RegisterAnimationModule();

	// Registers the animation components (Skin, Animator, AttachToJoint: AnimationComponents.cpp); part of
	// RegisterAnimationModule.
	void RegisterAnimationComponents();

}
