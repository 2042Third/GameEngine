#include <doctest/doctest.h>

#include "Strata/Asset/AssetStreamingQueue.h"
#include "Strata/Math/AABB.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/TextureStreamer.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Strata;

// The texture streamer's decisions without a GPU (TextureStreamingPolicy): which level a texture needs, how needs of
// several uses and views combine, when detail falls again, and in which order the reads of levels go.
TEST_SUITE("Renderer.TextureStreamer")
{
	TEST_CASE("The wanted level follows the texels a pixel covers: distance and texture coordinate tiling")
	{
		// A 2048x2048 texture with 12 levels.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 2048.0f) == 0);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 4096.0f) == 0); // Larger than its texels
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 1024.0f) == 1);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 1000.0f) == 1);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 64.0f) == 5);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 33.0f) == 5);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 32.0f) == 6);
		// Twice as far, half the pixels: one level coarser.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 128.0f) == 4);
		// Tiled four times: each repeat covers a quarter of the pixels, so the GPU samples two levels coarser.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 4.0f, 128.0f) == 6);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 4.0f, 2048.0f) == 2);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 0.5f, 128.0f) == 3);
		// Tiny, invisible or broken sizes need the coarsest level; the levels it has bound the result.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 1.0f) == 11);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 0.0f) == 11);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, -5.0f) == 11);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, std::numeric_limits<float>::quiet_NaN()) == 11);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 4, 1.0f, 2.0f) == 3);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 0.0f, 64.0f) == 0);
		// Coordinates that do not change over a surface (GetUVPixels: the largest float) sample level 0.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, std::numeric_limits<float>::max()) == 0);
		// Non-square textures use their larger side.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(4096, 13, 1.0f, 256.0f) == 4);
	}

	TEST_CASE("A world unit spans the most pixels at the bounds' point nearest the camera")
	{
		SceneCamera camera;
		camera.Position = glm::vec3(0.0f, 1.5f, 40.0f);
		camera.VerticalFOV = glm::radians(60.0f);
		camera.Near = 0.1f;
		const TextureStreamingPolicy::ViewScale view = TextureStreamingPolicy::GetViewScale(camera, 1080.0f);
		CHECK(view.UnitPixels == doctest::Approx(1080.0f / (2.0f * std::tan(glm::radians(30.0f)))));

		// A floor of 100 x 100 units 1.5 units below the camera (its center 40 units away): at its nearest point.
		const AABB ground(glm::vec3(-50.0f, 0.0f, -50.0f), glm::vec3(50.0f, 0.0f, 50.0f));
		CHECK(TextureStreamingPolicy::GetWorldPixels(view, ground) == doctest::Approx(view.UnitPixels / 1.5f));
		// Beyond its end, along the axis; inside the bounds, at the near plane.
		camera.Position = glm::vec3(0.0f, 0.0f, 150.0f);
		CHECK(TextureStreamingPolicy::GetWorldPixels(TextureStreamingPolicy::GetViewScale(camera, 1080.0f), ground) == doctest::Approx(view.UnitPixels / 100.0f));
		camera.Position = glm::vec3(0.0f);
		CHECK(TextureStreamingPolicy::GetWorldPixels(TextureStreamingPolicy::GetViewScale(camera, 1080.0f), ground) == doctest::Approx(view.UnitPixels / 0.1f));
		// An orthographic camera shows VerticalFOV units over the view's height, wherever things are.
		camera.Orthographic = true;
		camera.VerticalFOV = 10.0f;
		const TextureStreamingPolicy::ViewScale orthographic = TextureStreamingPolicy::GetViewScale(camera, 1080.0f);
		CHECK(TextureStreamingPolicy::GetWorldPixels(orthographic, ground) == doctest::Approx(108.0f));

		// The floor is the unit plane (one texture across it, facing +Y) scaled 100 times and tiled 20 times by its material,
		// with a 2048x2048 texture: under the camera 2048 * 20 texels cover 100 units, which 62352 pixels span there, so it
		// needs level 0. Its bounding sphere seen from 40 units (the projected size of the old estimate, 4500 pixels) would
		// have asked for level 3.
		const float uvPixels = TextureStreamingPolicy::GetUVPixels(view.UnitPixels / 1.5f, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(100.0f, 1.0f, 100.0f));
		CHECK(uvPixels == doctest::Approx(100.0f * view.UnitPixels / 1.5f));
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 20.0f, uvPixels) == 0);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 20.0f, 4500.0f) == 3);
	}

	TEST_CASE("Texture coordinates per world unit follow the scales along the surfaces, the sparsest surfaces deciding")
	{
		// A unit cube's faces each span the texture once. Flattened into a slab of 100 x 1 x 100 units, its top and bottom
		// spread the texture over 100 units each way, its sides over 100 units one way and 1 the other: the top and bottom
		// show each texel largest, so they decide.
		const glm::vec3 cube(1.0f);
		const glm::vec3 slab(100.0f, 1.0f, 100.0f);
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, cube, glm::vec3(1.0f)) == doctest::Approx(50.0f));
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, cube, glm::vec3(2.0f)) == doctest::Approx(100.0f));
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, cube, slab) == doctest::Approx(5000.0f));
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, glm::vec3(0.0f, 1.0f, 0.0f), slab) == doctest::Approx(5000.0f));
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, glm::vec3(1.0f, 0.0f, 1.0f), slab) == doctest::Approx(500.0f)); // The sides alone
		// No density (coordinates that do not change): level 0, like the GPU samples it. A scale of 0 stays finite.
		CHECK(TextureStreamingPolicy::GetUVPixels(50.0f, glm::vec3(0.0f), glm::vec3(1.0f)) == std::numeric_limits<float>::max());
		const float flattened = TextureStreamingPolicy::GetUVPixels(50.0f, cube, glm::vec3(1.0f, 0.0f, 1.0f));
		CHECK(std::isfinite(flattened));
		CHECK(flattened >= 0.0f);
	}

	TEST_CASE("A view that moved far or turned much since its previous frame jumped")
	{
		const glm::vec3 forward(0.0f, 0.0f, -1.0f);
		const glm::vec3 start(0.0f);
		CHECK_FALSE(TextureStreamingPolicy::IsCameraCut(start, forward, start, forward));
		CHECK_FALSE(TextureStreamingPolicy::IsCameraCut(start, forward, glm::vec3(3.0f, 0.0f, -5.0f), forward));
		const glm::vec3 turned30 = glm::normalize(glm::vec3(std::sin(glm::radians(30.0f)), 0.0f, -std::cos(glm::radians(30.0f))));
		CHECK_FALSE(TextureStreamingPolicy::IsCameraCut(start, forward, start, turned30));
		CHECK(TextureStreamingPolicy::IsCameraCut(start, forward, glm::vec3(0.0f, 0.0f, -TextureStreamingPolicy::c_CameraCutDistance - 0.5f), forward));
		const glm::vec3 turned50 = glm::normalize(glm::vec3(std::sin(glm::radians(50.0f)), 0.0f, -std::cos(glm::radians(50.0f))));
		CHECK(TextureStreamingPolicy::IsCameraCut(start, forward, start, turned50));
	}

	TEST_CASE("Needs of several uses and views combine into the finest level and the largest coverage")
	{
		TextureStreamingPolicy policy;
		const AssetHandle texture(0x9100);
		const AssetHandle other(0x9101);
		// One frame: a far use, a near use (another material), a second view that sees it larger but jumped.
		policy.AddNeed(texture, 6, 0.05f, false);
		policy.AddNeed(texture, 3, 0.20f, false);
		policy.AddNeed(texture, 4, 0.40f, true, true);
		policy.AddNeed(other, 8, 0.01f, false);
		policy.Update(1);
		const TextureStreamingPolicy::Target* target = policy.Find(texture);
		REQUIRE(target);
		CHECK(target->Level == 3);
		CHECK(target->Coverage == doctest::Approx(0.40f));
		CHECK(target->CameraCut);
		CHECK(target->Whole); // One of its views draws without streaming
		CHECK(target->LastNeededFrame == 1);
		REQUIRE(policy.Find(other));
		CHECK(policy.Find(other)->Level == 8);
		CHECK_FALSE(policy.Find(other)->CameraCut);
		CHECK(policy.GetTargets().size() == 2);

		// The next frame starts anew: its needs alone count (coverage, camera cut).
		policy.AddNeed(texture, 5, 0.10f, false);
		policy.Update(2);
		CHECK(policy.Find(texture)->Coverage == doctest::Approx(0.10f));
		CHECK_FALSE(policy.Find(texture)->CameraCut);
		CHECK_FALSE(policy.Find(texture)->Whole);
		CHECK(policy.Find(texture)->Level == 3); // Held: see the hysteresis test
	}

	TEST_CASE("Detail rises at once and falls only after 120 frames without a need of it")
	{
		TextureStreamingPolicy policy;
		const AssetHandle texture(0x9200);
		uint64_t frame = 1;
		policy.AddNeed(texture, 5, 0.1f, false);
		policy.Update(frame);
		CHECK(policy.Find(texture)->Level == 5);

		// Finer: at once.
		policy.AddNeed(texture, 2, 0.5f, false);
		policy.Update(++frame);
		CHECK(policy.Find(texture)->Level == 2);
		const uint64_t heldFrom = frame;

		// Coarser needs: the finer level is held for 120 frames after it was last needed, then follows the need.
		for (; frame < heldFrom + TextureStreamingPolicy::c_DropDelayFrames - 1;)
		{
			policy.AddNeed(texture, 6, 0.05f, false);
			policy.Update(++frame);
			REQUIRE(policy.Find(texture)->Level == 2);
		}
		policy.AddNeed(texture, 6, 0.05f, false);
		policy.Update(++frame);
		CHECK(frame == heldFrom + TextureStreamingPolicy::c_DropDelayFrames);
		CHECK(policy.Find(texture)->Level == 6);

		// A need of the held level (or finer) restarts the wait.
		policy.AddNeed(texture, 1, 0.9f, false);
		policy.Update(++frame);
		const uint64_t raisedAt = frame;
		policy.AddNeed(texture, 4, 0.2f, false);
		policy.Update(++frame);
		policy.AddNeed(texture, 1, 0.9f, false);
		policy.Update(++frame);
		for (uint32_t step = 0; step < 100; step++)
		{
			policy.AddNeed(texture, 4, 0.2f, false);
			policy.Update(++frame);
		}
		CHECK(frame - raisedAt > 100);
		CHECK(policy.Find(texture)->Level == 1);

		// Needed by nobody: kept for 120 frames, then the tail (no level), then forgotten.
		policy.AddNeed(texture, 1, 0.9f, false);
		policy.Update(++frame);
		const uint64_t lastNeeded = frame;
		while (frame < lastNeeded + TextureStreamingPolicy::c_DropDelayFrames - 1)
			policy.Update(++frame);
		CHECK(policy.Find(texture)->Level == 1);
		CHECK(policy.Find(texture)->Coverage == 0.0f);
		while (frame < lastNeeded + TextureStreamingPolicy::c_DropDelayFrames + 1)
			policy.Update(++frame);
		CHECK(policy.Find(texture)->Level == TextureStreamingPolicy::c_NoLevel);
		while (frame < lastNeeded + TextureStreamingPolicy::c_ForgetFrames)
			policy.Update(++frame);
		CHECK(policy.Find(texture) == nullptr);
	}

	TEST_CASE("Only targets that changed in a way that can call for another level are listed as changed")
	{
		TextureStreamingPolicy policy;
		const AssetHandle texture(0x9300);
		const AssetHandle other(0x9301);
		uint64_t frame = 1;
		auto changed = [&policy]()
		{
			std::vector<AssetHandle> handles(policy.GetChanged().begin(), policy.GetChanged().end());
			std::sort(handles.begin(), handles.end());
			return handles;
		};

		// New targets.
		policy.AddNeed(texture, 4, 0.2f, false);
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(frame);
		CHECK(changed() == std::vector<AssetHandle> { texture, other });

		// The same needs, also with another coverage or a camera cut: nothing that calls for another level.
		policy.AddNeed(texture, 4, 0.3f, true);
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(++frame);
		CHECK(changed().empty());
		// A coarser need while the finer level is held: no change either.
		policy.AddNeed(texture, 5, 0.1f, false);
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(++frame);
		CHECK(changed().empty());
		CHECK(policy.Find(texture)->Level == 4);

		// A finer level.
		policy.AddNeed(texture, 2, 0.6f, false);
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(++frame);
		CHECK(changed() == std::vector<AssetHandle> { texture });

		// Not needed any more (a texture no longer drawn may give up what it does not need), then needed again.
		policy.AddNeed(texture, 2, 0.6f, false);
		policy.Update(++frame);
		CHECK(changed() == std::vector<AssetHandle> { other });
		policy.AddNeed(texture, 2, 0.6f, false);
		policy.Update(++frame);
		CHECK(changed().empty());
		policy.AddNeed(texture, 2, 0.6f, false);
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(++frame);
		CHECK(changed() == std::vector<AssetHandle> { other });

		// Unneeded long enough: its level goes (once), then it is forgotten (once).
		const uint64_t lastNeeded = frame;
		uint32_t levelChanges = 0;
		while (frame < lastNeeded + TextureStreamingPolicy::c_ForgetFrames - 1)
		{
			policy.AddNeed(other, 6, 0.1f, false);
			policy.Update(++frame);
			if (frame == lastNeeded + 1)
				CHECK(changed() == std::vector<AssetHandle> { texture });
			else if (!changed().empty())
			{
				CHECK(changed() == std::vector<AssetHandle> { texture });
				CHECK(policy.Find(texture)->Level == TextureStreamingPolicy::c_NoLevel);
				levelChanges++;
			}
		}
		CHECK(levelChanges == 1);
		REQUIRE(policy.Find(texture));
		policy.AddNeed(other, 6, 0.1f, false);
		policy.Update(++frame);
		CHECK(changed() == std::vector<AssetHandle> { texture });
		CHECK(policy.Find(texture) == nullptr);
	}

	TEST_CASE("Reads of levels rank by missing levels times coverage, behind the loads of whole assets")
	{
		const TextureStreamingPolicy::Rank small = TextureStreamingPolicy::RankRaise(1, 0.05f, false);
		const TextureStreamingPolicy::Rank nearer = TextureStreamingPolicy::RankRaise(1, 0.5f, false);
		const TextureStreamingPolicy::Rank deeper = TextureStreamingPolicy::RankRaise(4, 0.05f, false);
		const TextureStreamingPolicy::Rank both = TextureStreamingPolicy::RankRaise(4, 0.5f, false);
		CHECK(small.Priority == AssetPriority::Low);
		CHECK(both.Priority == AssetPriority::Low);
		CHECK(nearer.Score > small.Score);
		CHECK(deeper.Score > small.Score);
		CHECK(both.Score > nearer.Score);
		CHECK(both.Score > deeper.Score);
		CHECK(both.Score == doctest::Approx(4.0f * 0.5f));
		CHECK(both.Outranks(nearer));
		CHECK_FALSE(nearer.Outranks(both));
		CHECK_FALSE(both.Outranks(both));
		// A camera cut's reads outrank every other, whatever their scores.
		const TextureStreamingPolicy::Rank cut = TextureStreamingPolicy::RankRaise(1, 0.01f, true);
		CHECK(cut.Outranks(both));
		CHECK_FALSE(both.Outranks(cut));
		// Coverage counts up to the whole view.
		CHECK(TextureStreamingPolicy::RankRaise(2, 5.0f, false).Score == doctest::Approx(2.0f));
		CHECK(TextureStreamingPolicy::RankRaise(2, std::numeric_limits<float>::infinity(), false).Score == 0.0f);

		// In the streaming queue: reads go in that order (higher scores first within the priority).
		std::vector<uint64_t> dispatched;
		AssetStreamingQueue queue([&dispatched](const AssetStreamingRequest& request)
		{
			dispatched.push_back(request.RangeId);
			return true;
		});
		queue.EnqueueRange(1, AssetHandle(0x9301), small.Priority, small.Score, 1024);
		queue.EnqueueRange(2, AssetHandle(0x9302), both.Priority, both.Score, 1024);
		queue.EnqueueRange(3, AssetHandle(0x9303), nearer.Priority, nearer.Score, 1024);
		queue.EnqueueRange(4, AssetHandle(0x9304), deeper.Priority, deeper.Score, 1024);
		queue.Pump();
		CHECK(dispatched == std::vector<uint64_t> { 2, 3, 4, 1 });
	}

	TEST_CASE("After a camera cut the new view's first read goes before 50 older Low reads")
	{
		std::vector<uint64_t> order;
		AssetStreamingQueue queue([&order](const AssetStreamingRequest& request)
		{
			order.push_back(request.RangeId);
			return true;
		});
		AssetStreamingLimits limits;
		limits.MaxOutstandingReads = 1;
		queue.SetLimits(limits);

		// One read runs; 50 reads of the previous view wait behind it, some of them ranked high (large, missing much).
		queue.EnqueueRange(1000, AssetHandle(0x9400), AssetPriority::Low, 0.0f, 1024);
		queue.Pump();
		REQUIRE(order == std::vector<uint64_t> { 1000 });
		for (uint64_t index = 0; index < 50; index++)
		{
			const TextureStreamingPolicy::Rank rank = TextureStreamingPolicy::RankRaise(4, 0.9f, false);
			queue.EnqueueRange(1 + index, AssetHandle(0x9500 + index), rank.Priority, rank.Score, 1024);
		}
		// The camera cut: the new view's first read, small on screen and missing one level.
		const TextureStreamingPolicy::Rank cut = TextureStreamingPolicy::RankRaise(1, 0.01f, true);
		CHECK(cut.Priority == AssetPriority::Normal);
		queue.EnqueueRange(2000, AssetHandle(0x9600), cut.Priority, cut.Score, 1024);

		// It is served next.
		queue.OnReadFinished();
		queue.OnLoadFinished(1024, true);
		REQUIRE(order.size() == 2);
		CHECK(order[1] == 2000);
		const AssetStreamingQueueStats stats = queue.GetStats();
		CHECK(stats.QueuedRanges == 50);
		CHECK(stats.InFlightRanges == 1);

		// The new view also needs a texture whose read the previous view queued: the streamer raises that read to the cut's
		// rank (TextureStreamer::OnUpdate, AssetManagerBase::RaiseRange), so it goes before the other older reads too.
		const TextureStreamingPolicy::Rank queued = TextureStreamingPolicy::RankRaise(4, 0.9f, false);
		const TextureStreamingPolicy::Rank raised = TextureStreamingPolicy::RankRaise(2, 0.3f, true);
		REQUIRE(raised.Outranks(queued));
		CHECK(queue.RaiseRange(37, raised.Priority, raised.Score));
		queue.OnReadFinished();
		queue.OnLoadFinished(1024, true);
		REQUIRE(order.size() == 3);
		CHECK(order[2] == 37);
	}
}
