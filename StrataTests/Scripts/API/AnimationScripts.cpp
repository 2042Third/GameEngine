// Scripts of the Scripting.Animation tests (StrataTests/src/Scripting/ScriptAnimationTests.cpp). They run on "Rig", whose
// Animator plays the arm's skeleton with the locomotion controller (Idle, Walk with the events StepLeft at 0.25 and
// StepRight at 0.75 seconds, Jump; parameters Speed, Grounded and Jump) and whose field Clip names the Walk clip; "Plain"
// has no Animator.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	constexpr float c_NaN = std::numeric_limits<float>::quiet_NaN();

}

// Drives the rig through its controller over frames and records the events it receives.
class AnimatorAPI : public CheckingScript
{
public:
	AssetHandle Clip;
	int32_t Frame = 0;
	int32_t EventCount = 0;
	std::string Events;
	bool Done = false;

	void OnUpdate(float) override
	{
		Frame++;
		Animator animator = GetEntity().GetAnimator();
		if (Frame == 1)
		{
			Expect(animator.GetEntity() == GetEntity(), "Animator::GetEntity");
			Expect(animator.Play("Walk"), "Play a state");
			Expect(animator.GetCurrentState() == "Walk", "GetCurrentState after Play");
			Expect(!animator.IsInTransition(), "Play does not blend");
			Expect(animator.SetFloat("Speed", 1.0f), "SetFloat keeps Walk");
			Expect(animator.SetBool("Grounded", true), "SetBool");
		}
		else if (Frame == 50)
		{
			// Walk ran 49 updates: its events at 0.25 and 0.75 seconds have reached OnAnimationEvent.
			Expect(Near(animator.GetNormalizedTime(), 49.0f / 60.0f, 1e-3f), "GetNormalizedTime");
			Expect(animator.CrossFade("Idle", 0.5f), "CrossFade");
			Expect(animator.IsInTransition(), "IsInTransition during the fade");
			Expect(animator.GetCurrentState() == "Idle", "the fade's target is the current state");
			Expect(animator.SetFloat("Speed", 0.0f), "Speed 0 keeps Idle");
			Expect(animator.SetTrigger("Jump"), "SetTrigger");
			Expect(animator.ResetTrigger("Jump"), "ResetTrigger");
		}
		else if (Frame == 51)
		{
			Expect(animator.GetCurrentState() == "Idle", "a reset trigger fires nothing");
			Expect(animator.SetSpeed(2.0f), "SetSpeed");
			Expect(GetEntity().GetProperty<float>("Animator", "Speed").value_or(0.0f) == 2.0f, "the speed is the component's");
			Expect(animator.Play(Clip), "Play a clip");
			Expect(animator.GetCurrentState().empty(), "a clip played directly is no state");

			// The wrist rides 2 units above the rig's origin in the rest pose; the walk bends it towards -X.
			const std::optional<JointTransform> wrist = animator.GetJointTransform("Wrist");
			Expect(wrist.has_value(), "GetJointTransform");
			const glm::vec3 origin = GetEntity().GetTransform().GetWorldPosition();
			Expect(wrist && wrist->Position.y > origin.y + 1.0f && wrist->Position.x < origin.x, "the joint is posed in the world");
			Expect(wrist && Near(wrist->Scale, glm::vec3(1.0f), 1e-3f), "the joint's scale");
			Expect(wrist && Near(glm::length(glm::vec4(wrist->Rotation.x, wrist->Rotation.y, wrist->Rotation.z, wrist->Rotation.w)), 1.0f, 1e-3f), "the joint's rotation");
			Expect(!animator.GetJointTransform("Tail"), "a joint the skeleton does not have");
			Done = true;
		}
	}

	void OnAnimationEvent(std::string_view name) override
	{
		EventCount++;
		Events += name;
		Events += ';';
	}
};

ST_SCRIPT_CLASS(AnimatorAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Frame);
	ST_SCRIPT_FIELD(EventCount);
	ST_SCRIPT_FIELD(Events);
	ST_SCRIPT_FIELD(Done);
}

// Calls the animator functions with what they refuse: every call fails harmlessly.
class AnimatorMisuse : public CheckingScript
{
public:
	AssetHandle Clip;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		Animator plain = Scene::FindEntityByName("Plain").GetAnimator();
		Expect(!plain.Play("Walk") && !plain.SetFloat("Speed", 1.0f) && !plain.SetSpeed(1.0f), "an entity without an Animator");
		Expect(plain.GetCurrentState().empty() && plain.GetNormalizedTime() == 0.0f && !plain.IsInTransition(), "queries of an entity without an Animator");
		Expect(!plain.GetJointTransform("Wrist"), "joints of an entity without an Animator");
		Expect(!Animator(Entity()).Play("Walk"), "the SDK's null entity");
		Expect(!host->AnimatorPlay(context, 0, Detail::ToABIString("Walk")), "the null entity");
		Expect(!host->AnimatorSetTrigger(context, 0x51DE5u, Detail::ToABIString("Jump")), "an entity that does not exist");

		Animator animator = GetEntity().GetAnimator();
		Expect(!animator.Play("Swim") && !animator.CrossFade("Swim", 0.1f), "a state the controller does not have");
		Expect(!animator.CrossFade("Walk", -1.0f) && !animator.CrossFade("Walk", c_NaN), "a negative or non-finite fade");
		Expect(!animator.SetFloat("Grounded", 1.0f) && !animator.SetBool("Speed", true) && !animator.SetTrigger("Speed"), "a parameter of another type");
		Expect(!animator.SetFloat("Missing", 1.0f) && !animator.ResetTrigger("Missing"), "a parameter the controller does not have");
		Expect(!animator.SetFloat("Speed", c_NaN), "a non-finite value");
		Expect(!animator.SetSpeed(1000.0f) && !animator.SetSpeed(c_NaN), "speeds out of range");
		Expect(!animator.Play(AssetHandle()) && !animator.Play(AssetHandle(0x51DE5u)), "clips that do not exist");
		Expect(!animator.Play(Clip), "an asset that is not an animation clip");
		Expect(!host->AnimatorGetJointTransform(context, GetEntity().GetID(), Detail::ToABIString("Wrist"), nullptr), "no transform to write");
		Expect(!animator.IsInTransition() && animator.GetCurrentState().empty(), "refused commands start nothing");
	}
};

ST_SCRIPT_CLASS(AnimatorMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Done);
}

// Where nothing animates every call fails harmlessly. Mode 0: the scene plays without its animation system; 1: the engine
// predates the animator functions (the script uses a host table that ends before them).
class AnimatorUnavailable : public CheckingScript
{
public:
	AssetHandle Clip;
	int32_t Mode = 0;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		StrataScriptHostAPI older = *Detail::GetHost();
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, AnimatorPlay));
		const StrataScriptHostAPI* engine = Detail::s_Host;
		if (Mode == 1)
			Detail::s_Host = &older;

		Animator animator = GetEntity().GetAnimator();
		const bool commanded = animator.Play("Walk") || animator.Play(Clip) || animator.CrossFade("Walk", 0.1f) || animator.SetFloat("Speed", 1.0f)
			|| animator.SetBool("Grounded", true) || animator.SetTrigger("Jump") || animator.ResetTrigger("Jump") || animator.SetSpeed(1.0f);
		const bool answered = !animator.GetCurrentState().empty() || animator.GetNormalizedTime() != 0.0f || animator.IsInTransition()
			|| animator.GetJointTransform("Wrist").has_value();
		Detail::s_Host = engine;

		Expect(!commanded, "commands fail");
		Expect(!answered, "queries answer nothing");
	}
};

ST_SCRIPT_CLASS(AnimatorUnavailable)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Clip);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Done);
}
