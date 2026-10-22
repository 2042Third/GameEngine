#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Scene/SceneLoaderTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Scene/PhysicsLayerSettings.h"
#include "Strata/Scene/ProjectCollisionLayers.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// How scenes loaded through SceneLoader take the project's collision layers (ProjectCollisionLayers, Scene::ApplyProjectLayers).

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The project's settings of a test, provided as IProjectSettings while it lives: a registry with the collision layers and
	// a simple test section (an edit of another section).
	struct TestProject
	{
		SettingsRegistry Registry;
		ProjectSettingsValues Values;
		Scope<EngineServices::ScopedProvider<IProjectSettings>> Provider;

		TestProject()
		{
			REQUIRE(Registry.Register(ProjectCollisionLayers::MakeSection()));
			REQUIRE(Registry.Register(MakeSimpleTestSection()));
			Provider = CreateScope<EngineServices::ScopedProvider<IProjectSettings>>(Values);
		}

		void SetLayers(const PhysicsLayerSettings& layers)
		{
			std::string error;
			REQUIRE_MESSAGE(Values.Set(ProjectCollisionLayers::c_SectionId, layers.ToJson(), Registry, &error), error);
		}
	};

	// Provides another provider's values and records the threads that read them.
	class RecordingSettings final : public IProjectSettings
	{
	public:
		explicit RecordingSettings(const IProjectSettings& values)
			: m_Values(values)
		{
		}

		const nlohmann::json* FindSection(std::string_view id) const override
		{
			Record();
			return m_Values.FindSection(id);
		}

		uint64_t GetVersion() const override
		{
			Record();
			return m_Values.GetVersion();
		}

		std::vector<std::thread::id> GetThreads() const
		{
			std::lock_guard lock(m_Mutex);
			return m_Threads;
		}
	private:
		void Record() const
		{
			std::lock_guard lock(m_Mutex);
			m_Threads.push_back(std::this_thread::get_id());
		}
	private:
		const IProjectSettings& m_Values;
		mutable std::mutex m_Mutex;
		mutable std::vector<std::thread::id> m_Threads;
	};

	// A scene document of a few entities whose settings store `layers` (PhysicsLayerSettings' stored form), as 0.5.0 wrote.
	std::vector<uint8_t> MakeDocumentStoring(const nlohmann::json& layers)
	{
		Scene scene("Level");
		for (int index = 0; index < 3; index++)
			scene.CreateEntity("Entity " + std::to_string(index));
		nlohmann::json document = SceneSerializer::Serialize(scene);
		document["Scene"]["Settings"]["PhysicsLayers"] = layers;
		const std::string text = JsonUtils::Dump(document);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// What 0.5.0's PhysicsPlayground start scene stores: "Ball" and "Ghost", which pass through each other.
	PhysicsLayerSettings MakeStoredLayers()
	{
		PhysicsLayerSettings layers;
		layers.Names[1] = "Ball";
		layers.Names[2] = "Ghost";
		layers.SetCollide(1, 2, false);
		return layers;
	}

	PhysicsLayerSettings MakeProjectLayers()
	{
		PhysicsLayerSettings layers;
		layers.Names[3] = "Enemies";
		layers.SetCollide(0, 3, false);
		return layers;
	}

	Ref<Scene> LoadScene(SceneLoaderFixture& fixture)
	{
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		fixture.Finish(request);
		REQUIRE_MESSAGE(request.GetState() == SceneLoadState::Ready, request.GetError());
		Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		return scene;
	}

}

TEST_SUITE("Physics.Layers")
{
	TEST_CASE("A scene loaded through SceneLoader takes the project's table at hand-over, also when the table changed while it loaded")
	{
		TestProject project;
		const PhysicsLayerSettings first = MakeProjectLayers();
		project.SetLayers(first);
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		manager.CloseGate();
		SceneLoadOptions options;
		std::optional<PhysicsLayerSettings> prepared;
		options.Prepare = [&prepared](Scene& scene, SceneWorkBudget&)
		{
			prepared = scene.GetPhysicsLayers();
			return true;
		};
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		// Only the scene's read passes: it arrives and waits for what it uses.
		manager.AllowReads(1);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.GetState() == SceneLoadState::LoadingDependencies;
		}, std::chrono::milliseconds(10000)));

		// The table changes while the load goes on (an edit in Project Settings, a migration).
		PhysicsLayerSettings second = first;
		second.Names[4] = "Pickups";
		second.SetCollide(4, 4, false);
		project.SetLayers(second);
		manager.OpenGate();
		fixture.Finish(request);
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		// The preparation (where physics creates bodies) saw the table the scene took when it arrived; the hand-over gives it the
		// table as it is now.
		REQUIRE(prepared.has_value());
		CHECK(*prepared == first);
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		CHECK(scene->GetPhysicsLayers() == second);
		CHECK_FALSE(scene->DoStoredLayersDiffer());
		CHECK_FALSE(scene->ApplyProjectLayers()); // Taken already
	}

	TEST_CASE("Scenes loaded through SceneLoader read the project settings only on the main thread")
	{
		TestProject project;
		project.SetLayers(MakeProjectLayers());
		RecordingSettings recording(project.Values);
		EngineServices::ScopedProvider<IProjectSettings> provider(recording);
		SceneLoaderFixture fixture(MakeDocumentStoring(MakeStoredLayers().ToJson()));
		const std::thread::id mainThread = std::this_thread::get_id();

		const Ref<Scene> scene = LoadScene(fixture);
		CHECK(scene->GetPhysicsLayers() == MakeProjectLayers());
		const std::vector<std::thread::id> threads = recording.GetThreads();
		REQUIRE_FALSE(threads.empty());
		for (const std::thread::id thread : threads)
			CHECK(thread == mainThread);
	}

	TEST_CASE("A scene's stored layers do not replace the project's, and a difference is reported")
	{
		TestProject project;
		project.SetLayers(MakeProjectLayers());
		SceneLoaderFixture fixture(MakeDocumentStoring(MakeStoredLayers().ToJson()));
		const Ref<Scene> scene = LoadScene(fixture);
		CHECK(scene->GetPhysicsLayers() == MakeProjectLayers());
		REQUIRE(scene->GetStoredPhysicsLayers().has_value());
		CHECK(*scene->GetStoredPhysicsLayers() == MakeStoredLayers());
		CHECK(scene->DoStoredLayersDiffer());

		// A project whose table is the scene's (a migrated one) reports nothing.
		project.SetLayers(MakeStoredLayers());
		const Ref<Scene> again = LoadScene(fixture);
		CHECK(again->GetPhysicsLayers() == MakeStoredLayers());
		CHECK_FALSE(again->DoStoredLayersDiffer());
	}

	TEST_CASE("A document storing the default table reports nothing")
	{
		// What 0.5.0 wrote into every scene without named layers.
		TestProject project;
		project.SetLayers(MakeProjectLayers());
		SceneLoaderFixture fixture(MakeDocumentStoring({ { "Names", nlohmann::json::object() }, { "NoCollisions", nlohmann::json::array() } }));
		const Ref<Scene> scene = LoadScene(fixture);
		CHECK_FALSE(scene->GetStoredPhysicsLayers().has_value());
		CHECK_FALSE(scene->DoStoredLayersDiffer());
		CHECK(scene->GetPhysicsLayers() == MakeProjectLayers());
	}

	TEST_CASE("Without a project a scene file's layers apply as in 0.5.0")
	{
		REQUIRE(EngineServices::Get<IProjectSettings>() == nullptr);
		SceneLoaderFixture fixture(MakeDocumentStoring(MakeStoredLayers().ToJson()));
		const Ref<Scene> scene = LoadScene(fixture);
		CHECK(scene->GetPhysicsLayers() == MakeStoredLayers());
		CHECK_FALSE(scene->ApplyProjectLayers());
		CHECK_FALSE(scene->DoStoredLayersDiffer());
	}

	TEST_CASE("A script's SetLayersCollide lasts for its scene, and an edit of another section keeps it")
	{
		TestProject project;
		project.SetLayers(MakeProjectLayers());
		SceneLoaderFixture fixture;
		const Ref<Scene> scene = LoadScene(fixture);
		REQUIRE_FALSE(scene->GetPhysicsLayers().DoCollide(0, 3));
		// What Physics::SetLayersCollide does: the running scene's copy changes.
		scene->GetPhysicsLayers().SetCollide(0, 3, true);

		// Another section changes (the provider's version does, the table does not): the script's change stays, in copies too.
		std::string error;
		REQUIRE_MESSAGE(project.Values.Set("Test.Simple", { { "Count", 5 } }, project.Registry, &error), error);
		CHECK_FALSE(scene->ApplyProjectLayers());
		CHECK(scene->GetPhysicsLayers().DoCollide(0, 3));
		const Ref<Scene> copy = Scene::Copy(scene);
		CHECK_FALSE(copy->ApplyProjectLayers());
		CHECK(copy->GetPhysicsLayers().DoCollide(0, 3));

		// The next scene (a switch) starts from the project's table.
		const Ref<Scene> next = LoadScene(fixture);
		CHECK_FALSE(next->GetPhysicsLayers().DoCollide(0, 3));

		// A change of the table itself replaces the script's change.
		PhysicsLayerSettings changed = MakeProjectLayers();
		changed.Names[5] = "Water";
		project.SetLayers(changed);
		CHECK(scene->ApplyProjectLayers());
		CHECK(scene->GetPhysicsLayers() == changed);
	}
}
