#include <doctest/doctest.h>

#include "Strata/Asset/AssetStreamingQueue.h"
#include "Strata/Renderer/TextureStreamer.h"

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
		// The bias moves every result, within the levels.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 64.0f, 1) == 6);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 64.0f, -2) == 3);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 2048.0f, -1) == 0);
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(2048, 12, 1.0f, 1.0f, 3) == 11);
		// Non-square textures use their larger side.
		CHECK(TextureStreamingPolicy::ComputeWantedLevel(4096, 13, 1.0f, 256.0f) == 4);
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
	}
}
