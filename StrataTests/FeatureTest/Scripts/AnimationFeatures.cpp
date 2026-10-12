// The animation API of scripts (Animator, Script::OnAnimationEvent) on "Arm Rig": an Animator with the arm's skeleton and
// the Arm controller (states Wave with the event WaveTop, Swing with SwingEnd, Rest; parameters Speed, Raised and the
// trigger Swing), not playing on start. "Arm Tip" follows the rig's wrist (AttachToJoint); "Swing Rig" plays the Swing clip
// with root motion, which moves it 0.5 along +X.
//
// The scenario advances by what the animator reports, not by frame numbers: exported games load the animation assets
// asynchronously, so the controller may arrive a few frames after Play (which waits for it).

#include "FeatureScript.h"

#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace FeatureTest;

class AnimationFeatures : public FeatureScript
{
public:
	// Swing Rig plays its clip at half speed for two seconds from its start; it has arrived well before this frame.
	static constexpr int32_t c_RootMotionFrame = 175;

	AssetHandle SwingClip;
	int32_t Stage = 0;
	int32_t Frames = 0;
	int32_t WaveTops = 0;
	int32_t SwingEnds = 0;
	bool JournaledEvent = false;

	void OnCreate() override
	{
		Journal(*this, "AnimationFeatures", "OnCreate");
	}

	void OnUpdate(float) override
	{
		if (Completed)
			return;

		// The animation system updates after the scripts: what a frame commands shows in the next frame.
		Animator animator = GetEntity().GetAnimator();
		Frames++;
		switch (Stage)
		{
			case 0:
				Expect(animator.GetEntity() == GetEntity(), "Animator::GetEntity");
				Expect(animator.GetCurrentState().empty(), "nothing plays before Play (PlayOnStart is off)");
				Expect(animator.Play("Wave") && animator.GetCurrentState() == "Wave", "Animator::Play");
				Expect(!animator.IsInTransition(), "Play does not blend");
				Expect(animator.SetFloat("Speed", 0.0f), "Animator::SetFloat");
				Expect(animator.SetBool("Raised", true), "Animator::SetBool");
				Stage = 1;
				break;
			case 1:
				// WaveTop, half a second into the wave.
				if (WaveTops == 0)
					break;
				Expect(WaveTops == 1, "the Wave state's event reached OnAnimationEvent once");
				Expect(animator.GetNormalizedTime() >= 0.5f, "Animator::GetNormalizedTime");
				Expect(animator.CrossFade("Rest", 0.25f), "Animator::CrossFade");
				Expect(animator.IsInTransition() && animator.GetCurrentState() == "Rest", "Animator::IsInTransition");
				Stage = 2;
				break;
			case 2:
				if (animator.IsInTransition())
					break;
				Expect(animator.GetCurrentState() == "Rest", "the fade ended in Rest");
				// From any state to Swing: the trigger with Raised set.
				Expect(animator.SetTrigger("Swing"), "Animator::SetTrigger");
				Stage = 3;
				break;
			case 3:
				Expect(animator.GetCurrentState() == "Swing", "the trigger fired the any-state transition");
				Expect(animator.ResetTrigger("Swing"), "Animator::ResetTrigger");
				Stage = 4;
				break;
			case 4:
			{
				// Swing does not loop: at its end (exit time 1, Speed below 0.5) it goes back to Wave, after its event.
				if (animator.GetCurrentState() != "Wave")
					break;
				Expect(SwingEnds == 1 && WaveTops == 1, "each event fired once");

				// The tip follows the wrist through its AttachToJoint component, which the pose places like GetJointTransform.
				const std::optional<JointTransform> wrist = animator.GetJointTransform("Wrist");
				const Entity tip = Scene::FindEntityByName("Arm Tip");
				Expect(wrist.has_value() && tip.IsValid(), "Animator::GetJointTransform");
				if (wrist && tip)
				{
					Expect(Near(wrist->Position, tip.GetTransform().GetWorldPosition(), 1e-3f), "the attachment is at the joint");
					Expect(Near(wrist->Rotation, tip.GetTransform().GetWorldRotation(), 1e-3f), "the attachment turns with the joint");
					Expect(Near(wrist->Scale, tip.GetTransform().GetWorldScale(), 1e-3f), "the attachment scales with the joint");
				}

				Expect(animator.SetSpeed(1.5f), "Animator::SetSpeed");
				Expect(animator.Play(SwingClip) && animator.GetCurrentState().empty(), "Animator::Play with a clip");
				Stage = 5;
				break;
			}
			case 5:
			{
				if (Frames < c_RootMotionFrame)
					break;
				// The swing rig's root motion moved it by the clip's travel.
				const Entity swingRig = Scene::FindEntityByName("Swing Rig");
				Expect(swingRig && Near(swingRig.GetTransform().GetWorldPosition(), glm::vec3(-2.5f, 0.0f, 4.0f), 1e-3f), "root motion moved the swing rig");
				Completed = true;
				break;
			}
			default:
				break;
		}
	}

	void OnAnimationEvent(std::string_view name) override
	{
		if (!JournaledEvent)
		{
			JournaledEvent = true;
			Journal(*this, "AnimationFeatures", "OnAnimationEvent");
		}
		if (name == "WaveTop")
			WaveTops++;
		else if (name == "SwingEnd")
			SwingEnds++;
	}
};

ST_SCRIPT_CLASS(AnimationFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(SwingClip);
	ST_SCRIPT_FIELD(Stage);
	ST_SCRIPT_FIELD(Frames);
	ST_SCRIPT_FIELD(WaveTops);
	ST_SCRIPT_FIELD(SwingEnds);
	ST_SCRIPT_FIELD(JournaledEvent);
}
