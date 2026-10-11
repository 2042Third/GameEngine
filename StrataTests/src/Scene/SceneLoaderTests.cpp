#include <doctest/doctest.h>

#include "Scene/SceneLoaderTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Scene.Loader")
{
	TEST_CASE("A scene loads with everything it uses, read once, and becomes ready when all of it is in")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		const uint64_t reads = SceneSerializer::GetDeserializeCount();

		manager.CloseGate();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		REQUIRE(request.IsValid());
		CHECK(request.GetState() == SceneLoadState::LoadingScene);
		CHECK(request.GetAssetCount() == 11);
		CHECK(request.GetTotalBytes() == manager.GetMetadata(fixture.SceneHandle)->StoredSize + 1000);
		CHECK(request.GetProgress() == 0.0f);
		// Everything the scene uses is pinned while the request lives.
		CHECK(manager.GetPinCount(fixture.SceneHandle) == 1);
		for (AssetHandle dependency : fixture.Dependencies)
			CHECK(manager.GetPinCount(dependency) == 1);
		CHECK_FALSE(request.TakeScene());

		// One read at a time: the scene first (prepared on the worker), then its dependencies; the progress never decreases
		// and the load is ready only when the last one is in.
		std::vector<float> progress;
		bool sawDependencies = false;
		for (int step = 0; step < 400 && !request.IsFinished(); step++)
		{
			manager.AllowReads(1);
			manager.Update();
			request.Update();
			progress.push_back(request.GetProgress());
			if (request.GetState() == SceneLoadState::LoadingDependencies)
			{
				sawDependencies = true;
				CHECK(progress.back() < 1.0f);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		manager.OpenGate();
		fixture.Finish(request);
		CHECK(sawDependencies);
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		CHECK(request.GetProgress() == 1.0f);
		for (size_t index = 1; index < progress.size(); index++)
			CHECK(progress[index] >= progress[index - 1]);
		for (AssetHandle dependency : fixture.Dependencies)
			CHECK(manager.GetAssetState(dependency) == AssetState::Ready);

		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		CHECK_FALSE(request.TakeScene());
		CHECK(scene->GetName() == "Level");
		CHECK(scene->GetEntityCount() == 5);
		CHECK(scene->GetSourceAsset() == fixture.SceneHandle);
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		// Prepared on the worker, world transforms included.
		CHECK(scene->GetEntitiesInHierarchyOrder().back().GetComponent<WorldTransformComponent>().Matrix[3].x == 4.0f);
		CheckSceneCaches(*scene);
		// The spent scene asset holds nothing; the next load reads the file again.
		CHECK(manager.GetAssetState(fixture.SceneHandle) == AssetState::Unloaded);

		// The pins go with the request.
		request = SceneLoadRequest();
		CHECK(manager.GetPinCount(fixture.SceneHandle) == 0);
		for (AssetHandle dependency : fixture.Dependencies)
			CHECK(manager.GetPinCount(dependency) == 0);
		CHECK(StateCountsMatch(manager));
	}

	TEST_CASE("A scene load may become ready before all it uses is in")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		SceneLoadOptions options;
		options.RequiredProgress = 0.0f;
		manager.CloseGate();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		// Only the scene's read passes: the load is ready without its dependencies, which keep loading (pinned).
		manager.AllowReads(1);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.IsFinished();
		}, std::chrono::milliseconds(10000)));
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(request.TakeScene());
		CHECK(manager.GetAssetState(fixture.Dependencies.back()) == AssetState::Loading);
		CHECK(manager.GetPinCount(fixture.Dependencies.back()) == 1);
		manager.OpenGate();
		REQUIRE(manager.WaitForPendingLoads());
	}

	TEST_CASE("Two loads of one scene each get a scene of their own")
	{
		SceneLoaderFixture fixture;
		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		SceneLoadRequest first = SceneLoader::LoadAsync(fixture.SceneHandle);
		SceneLoadRequest second = SceneLoader::LoadAsync(fixture.SceneHandle);
		fixture.Finish(first);
		fixture.Finish(second);
		REQUIRE(first.GetState() == SceneLoadState::Ready);
		REQUIRE(second.GetState() == SceneLoadState::Ready);
		const Ref<Scene> a = first.TakeScene();
		const Ref<Scene> b = second.TakeScene();
		REQUIRE(a);
		REQUIRE(b);
		CHECK(a != b);
		CHECK(a->GetEntityCount() == b->GetEntityCount());
		// The second read the document again, after the first took the scene of the first read.
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 2);
	}

	TEST_CASE("Cancelling a scene load releases every pin and leaves nothing of it")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		manager.CloseGate();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		// The scene and two dependencies are read; the rest waits at the gate.
		manager.AllowReads(3);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.GetState() == SceneLoadState::LoadingDependencies && manager.GetAssetState(fixture.Dependencies[1]) == AssetState::Ready;
		}, std::chrono::milliseconds(10000)));
		const float progress = request.GetProgress();
		CHECK(progress > 0.0f);
		CHECK(progress < 1.0f);

		request.Cancel();
		CHECK(request.GetState() == SceneLoadState::Cancelled);
		CHECK(request.IsFinished());
		CHECK_FALSE(request.TakeScene());
		for (AssetHandle member : { fixture.SceneHandle, fixture.Dependencies[0], fixture.Dependencies[9] })
			CHECK(manager.GetPinCount(member) == 0);
		manager.OpenGate();
		REQUIRE(manager.WaitForPendingLoads());
		CHECK(manager.GetStats().PinnedAssets == 0);
		CHECK(StateCountsMatch(manager));
		// The loads that had not started never ran.
		CHECK(manager.GetAssetState(fixture.Dependencies[9]) == AssetState::Unloaded);
		CHECK(manager.GetReadCount(fixture.Dependencies[9]) == 0);

		// A request destroyed before it finished cancels itself the same way.
		manager.CloseGate();
		{
			SceneLoadRequest abandoned = SceneLoader::LoadAsync(fixture.SceneHandle);
			CHECK(manager.GetPinCount(fixture.Dependencies[9]) == 1);
		}
		CHECK(manager.GetPinCount(fixture.Dependencies[9]) == 0);
		manager.OpenGate();
		REQUIRE(manager.WaitForPendingLoads());
		CHECK(manager.GetStats().PinnedAssets == 0);
	}

	TEST_CASE("A scene load fails for what is not a valid scene, and failed dependencies do not stop it")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;

		// Not a scene asset, or no asset at all: failed at once.
		SceneLoadRequest notScene = SceneLoader::LoadAsync(fixture.Dependencies[0]);
		CHECK(notScene.GetState() == SceneLoadState::Failed);
		CHECK(notScene.GetError().find("not a scene asset") != std::string::npos);
		CHECK(SceneLoader::LoadAsync(UUID(0x9999)).GetState() == SceneLoadState::Failed);

		// A broken document fails when the asset fails to load, with the loader's reason.
		const AssetHandle broken = manager.AddStored(0x300, AssetType::Scene, std::vector<uint8_t> { '{' }, "Scenes/Broken.stscene");
		SceneLoadRequest brokenRequest = SceneLoader::LoadAsync(broken);
		fixture.Finish(brokenRequest);
		CHECK(brokenRequest.GetState() == SceneLoadState::Failed);
		CHECK(brokenRequest.GetError().find("Loading scene") != std::string::npos);
		CHECK_FALSE(brokenRequest.TakeScene());

		// A dependency that fails to load (no stored form) counts as finished: the scene shows a fallback for it.
		const AssetHandle missing = manager.Add(0x400, MakeUsage(100), 100, AssetType::Texture);
		manager.SetDependencies(fixture.SceneHandle, { fixture.Dependencies[0], missing });
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		fixture.Finish(request);
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(manager.GetAssetState(missing) == AssetState::Failed);
		CHECK(request.TakeScene());

		// Without an active manager nothing can load.
		ScopedActiveManager none(nullptr);
		CHECK(SceneLoader::LoadAsync(fixture.SceneHandle).GetState() == SceneLoadState::Failed);
	}
}
