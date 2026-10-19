#pragma once

#include <cstdint>

namespace Strata
{

	// What animators that no request poses show in a scene that does not play (AnimationPreviewSpecification::RestPoses):
	// the editor's edited scene, by its viewport setting (ViewportSettings::EditPose).
	enum class AnimationEditPose : uint8_t
	{
		Rest = 0, // The skeleton's rest pose
		// What the animator plays on start, at time 0: its controller's default state's clip, else its Clip, as play mode shows
		// it before the first update. Without either (or while that clip does not load, as in play), the rest pose.
		ClipStart
	};

}
