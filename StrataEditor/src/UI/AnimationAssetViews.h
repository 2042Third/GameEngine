#pragma once

#include <Strata/Animation/AnimationController.h>
#include <Strata/Asset/AssetTypes.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Skeleton;

	// The Asset tab's views of animation assets: skeletons (their joints as a tree) and clips (duration and channels) to
	// read, animation controllers (parameters, states, transitions) to edit. Assets are requested every frame they show and
	// drawn once loaded. Main thread only.
	class AnimationAssetViews
	{
	public:
		// The joint count, the root motion joint and the joints as a tree (indented by depth, only the rows in view drawn).
		void DrawSkeleton(AssetHandle skeleton);
		// Duration, keys and a row per channel (joint, path, interpolation, keys).
		void DrawClip(AssetHandle clip);
		// The controller as editable tables. Every finished edit (a text committed, a drag released, a click) is written and
		// recorded as one undo step (AssetEdit::SetControllerData); while a value is dragged or typed the tables show it
		// without writing. Edits that would make the controller invalid are refused with the reason.
		//   Parameters: name, type, default; states: the default one, name, clip, speed, loop and events ("Name@seconds, ...");
		//   transitions: from (or Any), to, conditions ("Speed > 0.5, Jump, !Grounded": floats compared, bools that must
		//   hold or not, triggers by name), exit time (normalized; off: none) and cross-fade seconds.
		void DrawController(EditorContext& context, AssetHandle controller);
		// Ends an edit in progress without writing it (another asset shows).
		void Reset();

		// Conditions as the transitions table writes them, and back (false with the reason for text it cannot read).
		static std::string FormatConditions(const AnimationControllerData& data, const AnimationTransition& transition);
		static bool ParseConditions(const AnimationControllerData& data, std::string_view text, std::vector<AnimationCondition>& outConditions,
			std::string* outError = nullptr);
		static std::string FormatEvents(const AnimationState& state);
		static bool ParseEvents(std::string_view text, std::vector<AnimationEvent>& outEvents, std::string* outError = nullptr);
	private:
		struct QueuedEdit
		{
			AnimationControllerData Data;
			bool Finished = false;
			std::string Name; // Of the undo step
		};

		// Queues the frame's edit. The tables draw from the pending edit or the loaded controller, which applying an edit
		// replaces or ends, so DrawController applies it once they are drawn. One per frame: a finished edit is kept over an
		// unfinished one (another widget's, in the same frame).
		void Edit(AnimationControllerData data, bool finished, const std::string& name);
		// Applies an edit: kept to show while unfinished, else written as one undo step.
		void ApplyEdit(EditorContext& context, QueuedEdit edit);
		void DrawParameters(EditorContext& context, const AnimationControllerData& data);
		void DrawStates(EditorContext& context, const AnimationControllerData& data);
		void DrawTransitions(EditorContext& context, const AnimationControllerData& data);
	private:
		// The skeleton whose joint depths are cached.
		std::weak_ptr<const Skeleton> m_DepthSkeleton;
		std::vector<uint32_t> m_Depths;

		AssetHandle m_Controller = UUID::Null();
		std::weak_ptr<const AnimationController> m_Source; // The loaded controller the pending edit started from
		std::optional<AnimationControllerData> m_Pending;  // An edit in progress (shown, not written yet)
		std::optional<QueuedEdit> m_Queued;                 // The edit of the frame being drawn (applied after the tables)
		std::string m_Error;                                // Why the latest edit was refused
	};

}
