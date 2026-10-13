#include <doctest/doctest.h>

#include "Scene/SceneLoaderTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneReadProgress.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <algorithm>
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

	TEST_CASE("A load with a preparation works on its scene once per update after all it uses is in, and is ready when that is done")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		struct Call
		{
			const Scene* Prepared = nullptr;
			size_t LoadedDependencies = 0;
		};
		std::vector<Call> calls;
		SceneLoadOptions options;
		options.Prepare = [&](Scene& scene)
		{
			Call call { &scene };
			for (const AssetHandle dependency : fixture.Dependencies)
				call.LoadedDependencies += manager.GetAssetState(dependency) == AssetState::Ready ? 1 : 0;
			calls.push_back(call);
			return calls.size() == 3;
		};
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.GetState() == SceneLoadState::Preparing || request.IsFinished();
		}, std::chrono::milliseconds(10000)));
		REQUIRE(request.GetState() == SceneLoadState::Preparing);
		CHECK(std::string(SceneLoadStateToString(request.GetState())) == "Preparing");
		// Called in the update that found everything in, not before.
		REQUIRE(calls.size() == 1);
		CHECK(calls[0].LoadedDependencies == fixture.Dependencies.size());
		CHECK_FALSE(request.IsFinished());
		CHECK_FALSE(request.TakeScene());

		request.Update();
		CHECK(request.GetState() == SceneLoadState::Preparing);
		request.Update();
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(request.GetProgress() == 1.0f);
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		REQUIRE(calls.size() == 3);
		for (const Call& call : calls)
			CHECK(call.Prepared == scene.get()); // The scene the load hands over
		request.Update();
		CHECK(calls.size() == 3); // Never again once ready

		// A load cancelled while it prepares stops preparing.
		calls.clear();
		SceneLoadRequest cancelled = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			cancelled.Update();
			return cancelled.GetState() == SceneLoadState::Preparing || cancelled.IsFinished();
		}, std::chrono::milliseconds(10000)));
		REQUIRE(cancelled.GetState() == SceneLoadState::Preparing);
		cancelled.Cancel();
		CHECK(cancelled.GetState() == SceneLoadState::Cancelled);
		cancelled.Update();
		CHECK(calls.size() == 1);
		CHECK_FALSE(cancelled.TakeScene());
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

	TEST_CASE("Reading a scene document reports its progress as it parses the text and creates the entities")
	{
		// About 4 MB of text and five passes over 30,000 entities: several reports in each step.
		const std::vector<uint8_t> document = MakeSceneDocument(30'000);
		REQUIRE(document.size() > (size_t(3) << 20));
		std::vector<float> shares;
		std::string error;
		const Ref<SceneAsset> asset = SceneAsset::Deserialize(document, &error, [&shares](float share) { shares.push_back(share); });
		REQUIRE_MESSAGE(asset, error);
		CHECK(asset->CreateScene()->GetEntityCount() == 30'000);

		REQUIRE(shares.size() >= 10);
		for (size_t index = 1; index < shares.size(); index++)
			CHECK(shares[index] >= shares[index - 1]);
		CHECK(shares.front() > 0.0f);
		CHECK(shares.back() == 1.0f);
		// Shares while the text is parsed, while the entities are created, and between the two steps' ends.
		size_t parsing = 0;
		size_t creating = 0;
		for (const float share : shares)
		{
			if (share < 0.4f)
				parsing++;
			else if (share > 0.45f && share < 0.8f)
				creating++;
		}
		CHECK(parsing >= 3);
		CHECK(creating >= 6);
	}

	TEST_CASE("The reads of scene documents under way are found by scene asset, the furthest one counting")
	{
		const AssetHandle scene = UUID(0x5CE7E);
		const AssetHandle other = UUID(0x07E4);
		CHECK_FALSE(SceneReadProgress::Get(scene).has_value());
		{
			SceneReadProgress::Tracker first(scene);
			REQUIRE(SceneReadProgress::Get(scene).has_value());
			CHECK(*SceneReadProgress::Get(scene) == 0.0f);
			first.Report(0.3f);
			first.Report(0.2f); // A read never goes back
			CHECK(*SceneReadProgress::Get(scene) == doctest::Approx(0.3f));
			CHECK_FALSE(SceneReadProgress::Get(other).has_value());
			{
				SceneReadProgress::Tracker second(scene);
				second.Report(0.6f);
				CHECK(*SceneReadProgress::Get(scene) == doctest::Approx(0.6f));
			}
			CHECK(*SceneReadProgress::Get(scene) == doctest::Approx(0.3f));
			first.Report(5.0f);
			CHECK(*SceneReadProgress::Get(scene) == 1.0f);
		}
		CHECK_FALSE(SceneReadProgress::Get(scene).has_value());
	}

	TEST_CASE("A scene load's progress counts the read of its document for the document's share of the bytes")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		manager.CloseGate();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		REQUIRE(request.GetState() == SceneLoadState::LoadingScene);
		CHECK(request.GetProgress() == 0.0f);

		// While the document is read (here: a read of it standing at half), half of its share is done. The ten dependencies
		// hold 1,000 of the closure's bytes.
		const uint64_t sceneBytes = manager.GetMetadata(fixture.SceneHandle)->StoredSize;
		const float sceneShare = static_cast<float>(static_cast<double>(sceneBytes) / static_cast<double>(sceneBytes + 1000));
		{
			SceneReadProgress::Tracker read(fixture.SceneHandle);
			read.Report(0.5f);
			request.Update();
			CHECK(request.GetProgress() == doctest::Approx(sceneShare * 0.5f));
		}
		// Once that read is over the progress keeps what it reached until the loaded scene counts in full.
		CHECK(request.GetProgress() == doctest::Approx(sceneShare * 0.5f));

		manager.OpenGate();
		std::vector<float> progress;
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			progress.push_back(request.GetProgress());
			return request.IsFinished();
		}, std::chrono::milliseconds(10000)));
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(progress.front() >= sceneShare * 0.5f);
		for (size_t index = 1; index < progress.size(); index++)
			CHECK(progress[index] >= progress[index - 1]);
		CHECK(progress.back() == 1.0f);
	}

	TEST_CASE("A scene load leaves the other scenes the scene names unloaded")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		// The next level, which the scene names (a script field holding it), with an asset only it uses.
		const AssetHandle next = manager.AddStored(0x300, AssetType::Scene, MakeSceneDocument(2, "Next"), "Scenes/Next.stscene");
		const AssetHandle nextOnly = manager.Add(0x301, MakeUsage(100), 100);
		manager.SetDependencies(next, { nextOnly, fixture.Dependencies[0] });
		std::vector<AssetHandle> references = fixture.Dependencies;
		references.push_back(next);
		manager.SetDependencies(fixture.SceneHandle, references);

		// Everything the scene references, transitively, is what a game must ship; what using it needs stops at other scenes.
		CHECK(manager.GetDependencyClosure(fixture.SceneHandle).size() == 13);
		const std::vector<AssetHandle> used = manager.GetDependencyClosure(fixture.SceneHandle, AssetClosureScope::Load);
		CHECK(used.size() == 11);
		CHECK(std::find(used.begin(), used.end(), next) == used.end());
		CHECK(std::find(used.begin(), used.end(), nextOnly) == used.end());
		// The other scene's own closure starts with it.
		CHECK(manager.GetDependencyClosure(next, AssetClosureScope::Load).size() == 3);

		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		CHECK(request.GetAssetCount() == 11);
		CHECK(request.GetSkippedAssetCount() == 0);
		CHECK(request.GetTotalBytes() == manager.GetMetadata(fixture.SceneHandle)->StoredSize + 1000);
		CHECK(manager.GetPinCount(next) == 0);
		fixture.Finish(request);
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(request.TakeScene());
		CHECK(manager.GetAssetState(next) == AssetState::Unloaded);
		CHECK(manager.GetAssetState(nextOnly) == AssetState::Unloaded);
		CHECK(manager.GetReadCount(next) == 0);
		CHECK(manager.GetReadCount(nextOnly) == 0);
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
	}

	TEST_CASE("A scene load waits for a pool's share of what it uses only when all of it fits in the budget")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;

		// System memory has room for four and a half of the ten 100-byte assets the scene uses, one of which is loaded
		// already: the nine others are left to the scene (any four of them could be ones it does not draw first).
		REQUIRE(manager.LoadAssetSync(fixture.Dependencies[9]));
		AssetResidencyBudgets budgets = manager.GetResidencyBudgets();
		const uint64_t cpuBudget = manager.GetStats().Resident.Cpu + 450;
		budgets.Cpu = cpuBudget;
		manager.SetResidencyBudgets(budgets);
		{
			// The scene and the loaded asset (it holds its memory already).
			SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
			CHECK(request.GetAssetCount() == 2);
			CHECK(request.GetSkippedAssetCount() == 9);
			CHECK(request.GetTotalBytes() == manager.GetMetadata(fixture.SceneHandle)->StoredSize + 100);
			for (size_t index = 0; index < fixture.Dependencies.size(); index++)
			{
				CAPTURE(index);
				CHECK(manager.GetPinCount(fixture.Dependencies[index]) == (index == 9 ? 1u : 0u));
			}
			fixture.Finish(request);
			CHECK(request.GetState() == SceneLoadState::Ready);
			CHECK(request.GetProgress() == 1.0f);
			REQUIRE(request.TakeScene());
			// What was left out was never read; the pool stays within its budget.
			for (size_t index = 0; index < 9; index++)
			{
				CAPTURE(index);
				CHECK(manager.GetReadCount(fixture.Dependencies[index]) == 0);
			}
			CHECK(manager.GetStats().Resident.Cpu <= cpuBudget);
		}

		// The GPU texture pool has room for one of the scene's two textures: both are left to the scene, and so is an asset
		// that references one of them (requesting it would prefetch the texture). System memory is unlimited again: what
		// lives there waits.
		const AssetHandle firstTexture = manager.Add(0x400, MakeUsage(0, 1000), 1000, AssetType::Texture);
		const AssetHandle secondTexture = manager.Add(0x401, MakeUsage(0, 1000), 1000, AssetType::Texture);
		const AssetHandle textured = manager.Add(0x402, MakeUsage(100), 100);
		manager.SetDependencies(textured, { firstTexture });
		std::vector<AssetHandle> references = fixture.Dependencies;
		references.insert(references.end(), { firstTexture, secondTexture, textured });
		manager.SetDependencies(fixture.SceneHandle, references);
		budgets.Cpu = AssetResidencyBudgets::c_Unlimited;
		budgets.GpuTextures = manager.GetStats().Resident.GpuTextures + 1500;
		manager.SetResidencyBudgets(budgets);
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		CHECK(request.GetAssetCount() == 11);
		CHECK(request.GetSkippedAssetCount() == 3);
		CHECK(request.GetTotalBytes() == manager.GetMetadata(fixture.SceneHandle)->StoredSize + 1000);
		for (const AssetHandle dependency : fixture.Dependencies)
			CHECK(manager.GetPinCount(dependency) == 1);
		for (const AssetHandle leftOut : { firstTexture, secondTexture, textured })
			CHECK(manager.GetPinCount(leftOut) == 0);
		fixture.Finish(request);
		CHECK(request.GetState() == SceneLoadState::Ready);
		REQUIRE(request.TakeScene());
		for (const AssetHandle leftOut : { firstTexture, secondTexture, textured })
			CHECK(manager.GetReadCount(leftOut) == 0);
		CHECK(StateCountsMatch(manager));
	}

	TEST_CASE("A scene load fails when its scene goes away, and goes on when an asset it uses does")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;

		// A dependency deleted while it loads counts as finished (failed): the scene shows a fallback for it.
		manager.CloseGate();
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle);
		manager.AllowReads(1);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.GetState() != SceneLoadState::LoadingScene;
		}, std::chrono::milliseconds(10000)));
		REQUIRE(request.GetState() == SceneLoadState::LoadingDependencies);
		manager.Remove(fixture.Dependencies[3]);
		manager.OpenGate();
		fixture.Finish(request);
		CHECK(request.GetState() == SceneLoadState::Ready);
		CHECK(request.GetProgress() == 1.0f);
		CHECK(request.TakeScene());

		// The scene asset deleted while it is read: the load fails, naming it, instead of waiting for ever.
		manager.CloseGate();
		SceneLoadRequest removed = SceneLoader::LoadAsync(fixture.SceneHandle);
		REQUIRE(removed.GetState() == SceneLoadState::LoadingScene);
		manager.Remove(fixture.SceneHandle);
		for (int frame = 0; frame < 100 && !removed.IsFinished(); frame++)
		{
			manager.Update();
			removed.Update();
		}
		CHECK(removed.GetState() == SceneLoadState::Failed);
		CHECK(removed.GetError().find(fixture.SceneHandle.ToString()) != std::string::npos);
		CHECK(removed.GetError().find("no longer an asset") != std::string::npos);
		CHECK_FALSE(removed.TakeScene());
		manager.OpenGate();
		REQUIRE(manager.WaitForPendingLoads());
		CHECK(StateCountsMatch(manager));
	}
}
