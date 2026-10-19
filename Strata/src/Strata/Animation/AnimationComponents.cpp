#include "stpch.h"
#include "Strata/Animation/AnimationComponents.h"

#include "Strata/Animation/AnimationRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Scene/Components.h"

namespace Strata
{

	void RegisterAnimationComponents()
	{
		ComponentRegistry::Register<SkinComponent>("Skin")
			.Category("Animation")
			.Description("Poses the entity's skinned mesh with an animator's joints")
			.EntityProperty("Animator", &SkinComponent::Animator, { .Tooltip = "The entity with the Animator whose pose moves this mesh" });

		// Speeds are bounded like a controller state's (AnimationController::c_MaxSpeed, which the Scene layer cannot include).
		ComponentRegistry::Register<AnimatorComponent>("Animator")
			.Category("Animation")
			.Description("Plays a skeleton's animations: an animation controller's states, or one clip")
			.AssetProperty("Skeleton", &AnimatorComponent::Skeleton, AssetType::Skeleton, { .Tooltip = "The joints the animations move (a model's Skeleton)" })
			.AssetProperty("Controller", &AnimatorComponent::Controller, AssetType::AnimationController, { .Tooltip = "States, transitions and parameters; without one the animator plays Clip" })
			.AssetProperty("Clip", &AnimatorComponent::Clip, AssetType::AnimationClip, { .Tooltip = "The clip played without a controller" })
			.Property("Speed", &AnimatorComponent::Speed, WithTooltip(Range(-100.0f, 100.0f, 0.01f), "Playback rate; negative plays backwards"))
			.Property("Loop", &AnimatorComponent::Loop, WithTooltip({}, "Whether the clip (without a controller) repeats"))
			.Property("PlayOnStart", &AnimatorComponent::PlayOnStart, WithTooltip({}, "Starts playing when the scene starts"))
			.Property("ApplyRootMotion", &AnimatorComponent::ApplyRootMotion, WithTooltip({}, "Moves the entity by the root joint's horizontal motion and turn"))
			.EnumProperty("CullingMode", &AnimatorComponent::CullingMode, { { "AlwaysAnimate", 0, "Always Animate" }, { "WhenVisible", 1, "When Visible" } },
				{ .Tooltip = "When Visible poses the skeleton only while a renderer draws a mesh it skins or its shadow; one that skins no mesh but moves attached parts poses always" });

		ComponentRegistry::Register<AttachToJointComponent>("AttachToJoint")
			.DisplayName("Attach To Joint")
			.Category("Animation")
			.Description("Places the entity at a joint of an animator's pose (a sword in a hand)")
			.EntityProperty("Animator", &AttachToJointComponent::Animator, { .Tooltip = "The entity with the Animator" })
			.Property("Joint", &AttachToJointComponent::Joint, WithTooltip({}, "The joint's name in the animator's skeleton"));
	}

}
