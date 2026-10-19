#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"

#include <cstdint>
#include <string>

namespace Strata
{

	// The animation module's components (registered by RegisterAnimationComponents, with SkinComponent, whose type is in
	// Scene/Components.h).

	enum class AnimatorCullingMode : uint8_t
	{
		AlwaysAnimate = 0, // Posed every frame
		// Posed only while a renderer draws (or would draw, wherever its clips take it) a mesh it skins or that mesh's shadow
		// in view; time, events and root motion go on. An animator that skins no mesh but places attachments poses every frame.
		WhenVisible
	};

	// Plays animations on a skeleton (the Animation module's system, in Play mode): the states of an animation controller,
	// with their transitions and cross-fades, or without a controller one clip. The pose lives in the animation system, not
	// in entities: skinned meshes (SkinComponent) and attachments (AttachToJointComponent) name this entity to follow it.
	// Scripts control it through the SDK's Animator (Entity::GetAnimator).
	struct AnimatorComponent
	{
		AssetHandle Skeleton = UUID::Null();   // The joints the animations move (a model's Skeleton sub-asset)
		AssetHandle Controller = UUID::Null(); // States, transitions and parameters; none: Clip plays
		AssetHandle Clip = UUID::Null();       // The clip played without a controller
		float Speed = 1.0f;                    // Playback rate of everything the animator plays (negative: backwards)
		bool Loop = true;                      // Whether Clip (and clips scripts play directly) repeat
		bool PlayOnStart = true;               // Starts the controller's default state (or Clip) when the scene starts
		bool ApplyRootMotion = false;          // Moves the entity by the root joint's horizontal motion and turn
		AnimatorCullingMode CullingMode = AnimatorCullingMode::AlwaysAnimate;
	};

	// Places the entity at a joint of an animator's pose every frame (a sword in a hand, a rigid part of a model): its
	// world transform becomes the animator's world transform times the joint's transform in the pose. Children keep their
	// offsets from it. Without a pose (edit mode, nothing playing) the entity keeps its transform.
	struct AttachToJointComponent
	{
		UUID Animator = UUID::Null(); // The entity with the AnimatorComponent
		std::string Joint;            // The joint's name in the animator's skeleton
	};

}
