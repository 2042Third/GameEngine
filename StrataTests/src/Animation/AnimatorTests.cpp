#include <doctest/doctest.h>

#include "Animation/AnimationTestUtils.h"
#include "Strata/Animation/AnimatorInstance.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A clip that holds the elbow at one angle about +Z for a second.
	Ref<AnimationClip> CreateHoldClip(float degrees, std::string name)
	{
		std::vector<AnimationChannelData> channels;
		channels.push_back(MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, 1.0f }, Concat({ RotationZ(degrees), RotationZ(degrees) })));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create(std::move(name), std::move(channels), &error);
		REQUIRE_MESSAGE(clip, error);
		return clip;
	}

	AnimationState MakeState(std::string name, bool loop = true, std::vector<AnimationEvent> events = {})
	{
		AnimationState state;
		state.Name = std::move(name);
		state.Clip = UUID(0xC11);
		state.Loop = loop;
		state.Events = std::move(events);
		return state;
	}

	AnimationTransition MakeTransition(uint32_t from, uint32_t to, std::vector<AnimationCondition> conditions, std::optional<float> exitTime = std::nullopt, float duration = 0.0f)
	{
		AnimationTransition transition;
		transition.From = from;
		transition.To = to;
		transition.Conditions = std::move(conditions);
		transition.ExitTime = exitTime;
		transition.Duration = duration;
		return transition;
	}

	struct LocomotionAnimator
	{
		TestAnimatorAssets Assets { CreateArmSkeleton(), CreateController(CreateLocomotionControllerData(UUID(0xC1), UUID(0xC2), UUID(0xC3))),
			{ CreateWaveClip(1.0f, 0.0f, "Idle"), CreateWaveClip(1.0f, 0.0f, "Walk"), CreateWaveClip(1.0f, 0.0f, "Jump") } };
		AnimatorInstance Instance;
		AnimatorSettings Settings;
		std::vector<AnimatorFiredEvent> Events;

		bool Advance(float seconds)
		{
			Events.clear();
			return Instance.Advance(seconds, Assets.Get(), Settings, Events);
		}
	};

	std::vector<float> EventTimes(const std::vector<AnimatorFiredEvent>& events)
	{
		std::vector<float> times;
		for (const AnimatorFiredEvent& event : events)
			times.push_back(event.Time);
		return times;
	}

}

TEST_SUITE("Animation.StateMachine")
{
	TEST_CASE("Conditions choose the transitions, from the state the animator is in and from any state")
	{
		LocomotionAnimator animator;
		animator.Instance.Start();
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.GetCurrentState() == 0); // Idle, the default state
		CHECK_FALSE(animator.Instance.IsInTransition());

		// Speed 0.4 is not above 0.5.
		CHECK(animator.Instance.SetFloat(animator.Assets.Get(), "Speed", 0.4f));
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.GetCurrentState() == 0);
		// Above it, Idle fades to Walk over 0.2 seconds.
		CHECK(animator.Instance.SetFloat(animator.Assets.Get(), "Speed", 1.0f));
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.GetCurrentState() == 1);
		CHECK(animator.Instance.IsInTransition());
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.IsInTransition());
		REQUIRE(animator.Advance(0.1f));
		CHECK_FALSE(animator.Instance.IsInTransition());
		CHECK(animator.Instance.GetPlaying().size() == 1);

		// Parameters of another type, unknown ones and non-finite values are refused.
		CHECK_FALSE(animator.Instance.SetBool(animator.Assets.Get(), "Speed", true));
		CHECK_FALSE(animator.Instance.SetFloat(animator.Assets.Get(), "Missing", 1.0f));
		CHECK_FALSE(animator.Instance.SetFloat(animator.Assets.Get(), "Speed", std::numeric_limits<float>::quiet_NaN()));
		CHECK(animator.Instance.GetFloat("Speed") == 1.0f);

		// The jump from any state needs its trigger and Grounded: without Grounded the trigger stays set.
		CHECK(animator.Instance.SetBool(animator.Assets.Get(), "Grounded", false));
		CHECK(animator.Instance.SetTrigger(animator.Assets.Get(), "Jump", true));
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.GetCurrentState() == 1);
		CHECK(animator.Instance.GetBool("Jump"));
		CHECK(animator.Instance.SetBool(animator.Assets.Get(), "Grounded", true));
		REQUIRE(animator.Advance(0.1f));
		CHECK(animator.Instance.GetCurrentState() == 2);
		CHECK_FALSE(animator.Instance.GetBool("Jump"));

		// Jump does not loop: at its exit time 1 it lands in Idle at once (no fade).
		CHECK(animator.Instance.SetFloat(animator.Assets.Get(), "Speed", 0.0f));
		REQUIRE(animator.Advance(0.5f));
		CHECK(animator.Instance.GetCurrentState() == 2);
		REQUIRE(animator.Advance(0.6f));
		CHECK(animator.Instance.GetCurrentState() == 0);
		CHECK_FALSE(animator.Instance.IsInTransition());
	}

	TEST_CASE("Exit times fire in the update that reaches them, at every loop of a looping state")
	{
		AnimationControllerData data;
		data.Parameters = { AnimationParameter { "Go", AnimationParameterType::Bool, 0.0f, false } };
		data.States = { MakeState("A"), MakeState("B"), MakeState("Once", false), MakeState("Twice") };
		data.Transitions = {
			MakeTransition(0, 1, { AnimationCondition { 0, AnimationConditionMode::True, 0.0f } }, 0.5f),
			MakeTransition(2, 1, { AnimationCondition { 0, AnimationConditionMode::True, 0.0f } }, 0.5f),
			MakeTransition(3, 1, {}, 2.0f) };
		TestAnimatorAssets assets(CreateArmSkeleton(), CreateController(data),
			{ CreateWaveClip(1.0f, 0.0f, "A"), CreateWaveClip(1.0f, 0.0f, "B"), CreateWaveClip(1.0f, 0.0f, "Once"), CreateWaveClip(1.0f, 0.0f, "Twice") });
		const AnimatorSettings settings;
		std::vector<AnimatorFiredEvent> events;

		SUBCASE("A condition that holds waits for the exit time")
		{
			AnimatorInstance animator;
			REQUIRE(animator.Play(assets.Get(), "A"));
			REQUIRE(animator.SetBool(assets.Get(), "Go", true));
			REQUIRE(animator.Advance(0.3f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 0);
			REQUIRE(animator.Advance(0.3f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 1);
		}
		SUBCASE("A looping state that missed its exit time waits for the next loop's")
		{
			AnimatorInstance animator;
			REQUIRE(animator.Play(assets.Get(), "A"));
			REQUIRE(animator.Advance(0.7f, assets.Get(), settings, events));
			REQUIRE(animator.SetBool(assets.Get(), "Go", true));
			REQUIRE(animator.Advance(0.2f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 0);
			REQUIRE(animator.Advance(0.5f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 0);
			CHECK(animator.GetNormalizedTime() == doctest::Approx(1.4));
			REQUIRE(animator.Advance(0.2f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 1);
		}
		SUBCASE("A state that does not loop fires as soon as the condition holds after its exit time")
		{
			AnimatorInstance animator;
			REQUIRE(animator.Play(assets.Get(), "Once"));
			REQUIRE(animator.Advance(0.7f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 2);
			REQUIRE(animator.SetBool(assets.Get(), "Go", true));
			REQUIRE(animator.Advance(0.01f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 1);
		}
		SUBCASE("An exit time beyond 1 counts loops")
		{
			AnimatorInstance animator;
			REQUIRE(animator.Play(assets.Get(), "Twice"));
			REQUIRE(animator.Advance(1.5f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 3);
			REQUIRE(animator.Advance(0.4f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 3);
			REQUIRE(animator.Advance(0.2f, assets.Get(), settings, events));
			CHECK(animator.GetCurrentState() == 1);
		}
	}

	TEST_CASE("Triggers are consumed by the transition that fires, once")
	{
		AnimationControllerData data;
		data.Parameters = { AnimationParameter { "Next", AnimationParameterType::Trigger, 0.0f, false } };
		data.States = { MakeState("A"), MakeState("B"), MakeState("C") };
		data.Transitions = {
			MakeTransition(0, 1, { AnimationCondition { 0, AnimationConditionMode::Set, 0.0f } }),
			MakeTransition(1, 2, { AnimationCondition { 0, AnimationConditionMode::Set, 0.0f } }) };
		TestAnimatorAssets assets(CreateArmSkeleton(), CreateController(data),
			{ CreateWaveClip(1.0f, 0.0f, "A"), CreateWaveClip(1.0f, 0.0f, "B"), CreateWaveClip(1.0f, 0.0f, "C") });
		const AnimatorSettings settings;
		std::vector<AnimatorFiredEvent> events;
		AnimatorInstance animator;
		animator.Start();
		REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		CHECK(animator.GetCurrentState() == 0);

		// One trigger moves one step: A -> B consumes it, so B -> C does not fire on it.
		REQUIRE(animator.SetTrigger(assets.Get(), "Next", true));
		REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		CHECK(animator.GetCurrentState() == 1);
		CHECK_FALSE(animator.GetBool("Next"));
		for (int update = 0; update < 5; update++)
			REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		CHECK(animator.GetCurrentState() == 1);

		// A trigger reset before an update fires nothing.
		REQUIRE(animator.SetTrigger(assets.Get(), "Next", true));
		REQUIRE(animator.SetTrigger(assets.Get(), "Next", false));
		REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		CHECK(animator.GetCurrentState() == 1);
		REQUIRE(animator.SetTrigger(assets.Get(), "Next", true));
		REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		CHECK(animator.GetCurrentState() == 2);
	}

	TEST_CASE("Cross-fade weights add up to 1, also when fades interrupt each other")
	{
		AnimationControllerData data;
		data.States = { MakeState("S0"), MakeState("S1"), MakeState("S2"), MakeState("S3"), MakeState("S4") };
		std::vector<Ref<AnimationClip>> clips;
		for (int index = 0; index < 5; index++)
			clips.push_back(CreateHoldClip(20.0f * static_cast<float>(index), "Hold" + std::to_string(index)));
		TestAnimatorAssets assets(CreateArmSkeleton(), CreateController(data), clips);
		const AnimatorSettings settings;
		std::vector<AnimatorFiredEvent> events;
		AnimatorInstance animator;
		REQUIRE(animator.Play(assets.Get(), "S0"));
		REQUIRE(animator.Advance(0.05f, assets.Get(), settings, events));

		// Fades started before the previous ones end: more entries than an animator plays at once, too.
		const char* targets[] = { "S1", "S2", "S3", "S4", "S0", "S2", "S1" };
		const float durations[] = { 0.5f, 0.4f, 0.3f, 0.6f, 0.2f, 0.25f, 0.5f };
		for (size_t fade = 0; fade < std::size(targets); fade++)
		{
			REQUIRE(animator.CrossFade(assets.Get(), targets[fade], durations[fade]));
			for (int update = 0; update < 3; update++)
			{
				REQUIRE(animator.Advance(0.03f + 0.01f * static_cast<float>(update), assets.Get(), settings, events));
				CAPTURE(fade);
				CAPTURE(update);
				CHECK(animator.GetPlaying().size() <= AnimatorInstance::c_MaxPlaying);
				CHECK(SumWeights(animator) == doctest::Approx(1.0f).epsilon(1e-6));
				for (const AnimatorInstance::Playing& playing : animator.GetPlaying())
				{
					CHECK(playing.Weight >= 0.0f);
					CHECK(playing.Weight <= 1.0f);
				}
			}
		}
		// Once the last fade is over, only its target plays.
		REQUIRE(animator.Advance(1.0f, assets.Get(), settings, events));
		REQUIRE(animator.GetPlaying().size() == 1);
		CHECK(animator.GetPlaying()[0].Weight == 1.0f);
		CHECK(animator.GetCurrentState() == 1);
		// Negative and non-finite fades are refused.
		CHECK_FALSE(animator.CrossFade(assets.Get(), "S2", -1.0f));
		CHECK_FALSE(animator.CrossFade(assets.Get(), "S2", std::numeric_limits<float>::infinity()));
		CHECK_FALSE(animator.CrossFade(assets.Get(), "Missing", 0.1f));
	}

	TEST_CASE("A cross-fade blends the poses by the weights")
	{
		AnimationControllerData data;
		data.States = { MakeState("Straight"), MakeState("Bent") };
		TestAnimatorAssets assets(CreateArmSkeleton(), CreateController(data), { CreateHoldClip(0.0f, "Straight"), CreateHoldClip(90.0f, "Bent") });
		const AnimatorSettings settings;
		std::vector<AnimatorFiredEvent> events;
		AnimatorInstance animator;
		REQUIRE(animator.Play(assets.Get(), "Straight"));
		REQUIRE(animator.Advance(0.1f, assets.Get(), settings, events));
		REQUIRE(animator.CrossFade(assets.Get(), "Bent", 1.0f));
		REQUIRE(animator.Advance(0.5f, assets.Get(), settings, events));
		REQUIRE(animator.GetPlaying().size() == 2);
		CHECK(animator.GetPlaying()[1].Weight == doctest::Approx(0.5f));

		// Equal weights of two rotations give the rotation halfway between them; the wrist rides on the elbow.
		std::vector<JointPose> scratch;
		std::vector<glm::mat4> model;
		REQUIRE(animator.Evaluate(assets.Get(), settings, scratch, model));
		REQUIRE(model.size() == 3);
		const glm::quat elbow = glm::quat_cast(glm::mat3(model[1]));
		CHECK(glm::degrees(glm::angle(elbow)) == doctest::Approx(45.0f).epsilon(1e-4));
		const glm::vec3 wrist = glm::vec3(model[2][3]);
		CHECK(wrist.x == doctest::Approx(-std::sin(glm::radians(45.0f))).epsilon(1e-4));
		CHECK(wrist.y == doctest::Approx(1.0f + std::cos(glm::radians(45.0f))).epsilon(1e-4));
	}

	TEST_CASE("Commands given while the controller loads apply once it is known")
	{
		LocomotionAnimator animator;
		AnimatorAssets loading = animator.Assets.Get();
		loading.Controller = nullptr;
		loading.ControllerId = 0;
		loading.ControllerLoading = true;
		animator.Instance.Start();
		CHECK(animator.Instance.SetFloat(loading, "Speed", 0.25f));
		CHECK(animator.Instance.SetFloat(loading, "Speed", 0.3f)); // The latest value waits
		CHECK(animator.Instance.Play(loading, "Walk"));
		std::vector<AnimatorFiredEvent> events;
		CHECK_FALSE(animator.Instance.Advance(0.1f, loading, animator.Settings, events));
		CHECK(animator.Instance.GetFloat("Speed") == 0.3f);

		// Without a controller (none set, or one that will not load) parameters and states are refused.
		AnimatorAssets none = loading;
		none.ControllerLoading = false;
		CHECK_FALSE(animator.Instance.SetFloat(none, "Speed", 1.0f));
		CHECK_FALSE(animator.Instance.Play(none, "Walk"));

		// Loaded: the parameter has the value set while loading, and Walk plays instead of the default state. Speed 0.3 is
		// below 0.5, so Walk fades back to Idle.
		REQUIRE(animator.Advance(0.0f));
		CHECK(animator.Instance.GetFloat("Speed") == 0.3f);
		CHECK(animator.Instance.GetPlaying().size() >= 1);
		CHECK(animator.Instance.GetPlaying()[0].State == 1);
	}
}

TEST_SUITE("Animation.Sampling")
{
	TEST_CASE("Looping clips wrap and other clips stop at their end")
	{
		TestAnimatorAssets assets(CreateArmSkeleton(), nullptr, { CreateWaveClip(1.0f) });
		std::vector<AnimatorFiredEvent> events;
		std::vector<JointPose> scratch;
		std::vector<glm::mat4> model;

		AnimatorSettings looping;
		AnimatorInstance repeating;
		repeating.Start();
		REQUIRE(repeating.Advance(2.25f, assets.Get(), looping, events));
		REQUIRE(repeating.GetPlaying().size() == 1);
		CHECK(repeating.GetPlaying()[0].Time == doctest::Approx(0.25f));
		CHECK(repeating.GetNormalizedTime() == doctest::Approx(2.25));
		// A quarter into the wave the elbow is bent 45 degrees.
		REQUIRE(repeating.Evaluate(assets.Get(), looping, scratch, model));
		CHECK(glm::degrees(glm::angle(glm::quat_cast(glm::mat3(model[1])))) == doctest::Approx(45.0f).epsilon(1e-3));

		AnimatorSettings once;
		once.Loop = false;
		AnimatorInstance clamped;
		clamped.Start();
		REQUIRE(clamped.Advance(2.25f, assets.Get(), once, events));
		CHECK(clamped.GetPlaying()[0].Time == 1.0f);
		CHECK(clamped.GetNormalizedTime() == 1.0);
		REQUIRE(clamped.Evaluate(assets.Get(), once, scratch, model));
		CHECK(glm::degrees(glm::angle(glm::quat_cast(glm::mat3(model[1])))) == doctest::Approx(0.0f).epsilon(1e-3));
		// Backwards it stops at the start.
		once.Speed = -1.0f;
		REQUIRE(clamped.Advance(0.25f, assets.Get(), once, events));
		CHECK(clamped.GetPlaying()[0].Time == doctest::Approx(0.75f));
		REQUIRE(clamped.Advance(5.0f, assets.Get(), once, events));
		CHECK(clamped.GetPlaying()[0].Time == 0.0f);
	}

	TEST_CASE("Events fire once each when the playhead passes them, in order, also across a loop boundary")
	{
		// Times are binary fractions, so the playhead lands exactly where the test says.
		LocomotionAnimator animator;
		REQUIRE(animator.Instance.Play(animator.Assets.Get(), "Walk"));
		REQUIRE(animator.Instance.SetFloat(animator.Assets.Get(), "Speed", 1.0f)); // Walk stays
		// Walk's events, sorted by time: StepLeft at 0.25 and StepRight at 0.75.
		REQUIRE(animator.Advance(0.125f));
		CHECK(animator.Events.empty());
		REQUIRE(animator.Advance(0.75f));
		CHECK(EventTimes(animator.Events) == std::vector<float> { 0.25f, 0.75f });
		for (const AnimatorFiredEvent& event : animator.Events)
			CHECK(event.State == 1);
		REQUIRE_FALSE(animator.Events.empty());
		CHECK(animator.Assets.GetController().GetStates()[1].Events[animator.Events[0].Event].Name == "StepLeft");
		// Across the boundary: 0.875 -> 1.375 passes only the next loop's 0.25.
		REQUIRE(animator.Advance(0.5f));
		CHECK(EventTimes(animator.Events) == std::vector<float> { 0.25f });
		// Two loops in one update: every event of each pass, in the order they were passed.
		REQUIRE(animator.Advance(2.0f));
		CHECK(EventTimes(animator.Events) == std::vector<float> { 0.75f, 0.25f, 0.75f, 0.25f });
		// Landing on an event fires it once, not again from there.
		REQUIRE(animator.Advance(0.375f));
		CHECK(EventTimes(animator.Events) == std::vector<float> { 0.75f });
		REQUIRE(animator.Advance(0.125f));
		CHECK(animator.Events.empty());
		// Backwards they fire in reverse order.
		animator.Settings.Speed = -1.0f;
		REQUIRE(animator.Advance(0.75f));
		CHECK(EventTimes(animator.Events) == std::vector<float> { 0.75f, 0.25f });
	}

	TEST_CASE("An event at the start of a looping clip fires when it starts and once per loop")
	{
		AnimationControllerData data;
		data.States = { MakeState("Loop", true, { AnimationEvent { "Start", 0.0f } }), MakeState("Other") };
		TestAnimatorAssets assets(CreateArmSkeleton(), CreateController(data), { CreateWaveClip(1.0f), CreateWaveClip(1.0f) });
		const AnimatorSettings settings;
		std::vector<AnimatorFiredEvent> events;
		AnimatorInstance animator;
		animator.Start();
		REQUIRE(animator.Advance(0.5f, assets.Get(), settings, events));
		CHECK(EventTimes(events) == std::vector<float> { 0.0f });
		events.clear();
		REQUIRE(animator.Advance(0.4f, assets.Get(), settings, events));
		CHECK(events.empty());
		REQUIRE(animator.Advance(0.2f, assets.Get(), settings, events));
		CHECK(EventTimes(events) == std::vector<float> { 0.0f });
		events.clear();
		// A state fading out fires none.
		REQUIRE(animator.CrossFade(assets.Get(), "Other", 5.0f));
		REQUIRE(animator.Advance(1.0f, assets.Get(), settings, events));
		CHECK(events.empty());
	}
}
