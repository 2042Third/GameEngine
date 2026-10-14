#include <doctest/doctest.h>

#include "Animation/AnimationTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Animation/AnimationSystem.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneSystem.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The animation assets of the tests, in the active asset manager while it exists.
	class AnimationProject
	{
	public:
		AnimationProject()
		{
			AssetManager::SetActive(Manager);
			AddAnimationTestAssets(*Manager, true);
		}

		~AnimationProject()
		{
			AssetManager::SetActive(nullptr);
		}

		AnimationProject(const AnimationProject&) = delete;
		AnimationProject& operator=(const AnimationProject&) = delete;

		Ref<FakeAssetManager> Manager = CreateRef<FakeAssetManager>();
	};

	// The scene the animation test scripts expect (StrataTests/Scripts/API/AnimationScripts.cpp): "Rig" runs `className` with
	// `clip` in its Clip field; its Animator plays the arm with the locomotion controller. "Plain" has no Animator.
	Entity CreateAnimationScene(Scene& scene, const std::string& className, uint64_t clip = AnimationTestAssets::Walk)
	{
		Entity rig = scene.CreateEntity("Rig");
		rig.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 0.0f, 0.0f);
		AnimatorComponent& animator = rig.AddComponent<AnimatorComponent>();
		animator.Skeleton = UUID(AnimationTestAssets::Skeleton);
		animator.Controller = UUID(AnimationTestAssets::Controller);
		animator.Clip = UUID(AnimationTestAssets::Wave);
		ScriptEntry& entry = AddScriptEntry(rig, className);
		AddFieldOverride(entry, "Clip", PropertyType::Asset, PropertyValue(UUID(clip)));
		scene.CreateEntity("Plain");
		return rig;
	}

	// Scenes play without their animation system while this lives (a build without the animation module).
	class ScopedWithoutAnimationSystem
	{
	public:
		ScopedWithoutAnimationSystem()
		{
			const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
			const auto animation = std::find_if(descriptors.begin(), descriptors.end(), [](const SceneSystemDescriptor& descriptor) { return descriptor.Name == "Animation"; });
			REQUIRE(animation != descriptors.end());
			m_Descriptor = *animation;
			for (const SceneSystemDescriptor& descriptor : descriptors)
				m_Order.push_back(descriptor.Name);
			// The systems after it, registered again after it when it comes back: a registration counts as the latest.
			m_Following.assign(animation + 1, descriptors.end());
			REQUIRE(SceneSystemRegistry::Unregister("Animation"));
		}

		~ScopedWithoutAnimationSystem()
		{
			// No system names Animation: registering it again, then the systems that followed it, restores the order.
			CHECK(SceneSystemRegistry::Register(m_Descriptor));
			for (const SceneSystemDescriptor& descriptor : m_Following)
				CHECK(SceneSystemRegistry::Register(descriptor));
			std::vector<std::string> order;
			for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
				order.push_back(descriptor.Name);
			CHECK(order == m_Order);
		}

		ScopedWithoutAnimationSystem(const ScopedWithoutAnimationSystem&) = delete;
		ScopedWithoutAnimationSystem& operator=(const ScopedWithoutAnimationSystem&) = delete;
	private:
		SceneSystemDescriptor m_Descriptor;
		std::vector<SceneSystemDescriptor> m_Following;
		std::vector<std::string> m_Order;
	};

	void RunAnimationScript(const std::string& className, int32_t minimumChecks, uint64_t clip, std::optional<int32_t> mode = std::nullopt)
	{
		AnimationProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity rig = CreateAnimationScene(scene, className, clip);
		if (mode)
			AddFieldOverride(rig.GetComponent<ScriptComponent>().Scripts.back(), "Mode", PropertyType::Int, PropertyValue(*mode));
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, rig, className, "Done"));
		CheckScriptChecks(system, rig, className, minimumChecks);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

}

TEST_SUITE("Scripting.Animation")
{
	TEST_CASE("Scripts play states, set parameters, read joints and receive the events of their animator")
	{
		AnimationProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity rig = CreateAnimationScene(scene, "AnimatorAPI");
		scene.OnRuntimeStart();
		RunFrames(scene, 51);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, rig, "AnimatorAPI", "Done"));
		CheckScriptChecks(system, rig, "AnimatorAPI", 23);
		// Walk's events reached the script in the order the playhead passed them, once each.
		CHECK(GetField<std::string>(system, rig, "AnimatorAPI", "Events") == "StepLeft;StepRight;");
		CHECK(GetField<int32_t>(system, rig, "AnimatorAPI", "EventCount") == 2);
		CHECK_FALSE(engine->IsFaulted());

		// The engine plays what the script asked for: the Walk clip directly, at speed 2.
		AnimationSystem* animation = scene.GetSystem<AnimationSystem>();
		REQUIRE(animation != nullptr);
		const AnimatorInstance* instance = animation->FindInstance(rig);
		REQUIRE(instance != nullptr);
		CHECK(instance->GetPlayedClip() == UUID(AnimationTestAssets::Walk));
		CHECK(rig.GetComponent<AnimatorComponent>().Speed == 2.0f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Animator functions refuse misuse harmlessly")
	{
		// The Clip field names the controller: not an animation clip.
		RunAnimationScript("AnimatorMisuse", 16, AnimationTestAssets::Controller);
	}

	TEST_CASE("Animator functions do nothing in scenes without animation and on older engines")
	{
		{
			ScopedWithoutAnimationSystem withoutAnimation;
			RunAnimationScript("AnimatorUnavailable", 2, AnimationTestAssets::Walk, 0);
		}
		RunAnimationScript("AnimatorUnavailable", 2, AnimationTestAssets::Walk, 1);
	}

	TEST_CASE("Animation events reach the scripts of active animators only")
	{
		AnimationProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity rig = CreateAnimationScene(scene, "AnimatorAPI");
		scene.OnRuntimeStart();
		RunFrames(scene, 10); // Walk at 9/60 seconds: before its first event
		rig.SetActive(false);
		RunFrames(scene, 60);
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(system, rig, "AnimatorAPI", "EventCount") == 0);
		scene.OnRuntimeStop();
	}
}
