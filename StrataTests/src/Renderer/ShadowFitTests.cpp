#include <doctest/doctest.h>

#include "Strata/Renderer/ShadowFit.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

using namespace Strata;

namespace
{

	// Include/OrderedFloat.glsl's EncodeOrderedFloat.
	uint32_t EncodeOrdered(float value)
	{
		const uint32_t bits = std::bit_cast<uint32_t>(value);
		return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
	}

	struct TestCamera
	{
		glm::vec3 Position = glm::vec3(0.0f, 1.7f, 0.0f);
		glm::mat3 Rotation = glm::mat3(1.0f); // View space to world space
		float VerticalFOV = glm::radians(60.0f);
		float Aspect = 16.0f / 9.0f;
		bool Orthographic = false;
		float OrthographicHeight = 20.0f;

		void LookAt(const glm::vec3& target)
		{
			Rotation = glm::mat3(glm::inverse(glm::lookAt(Position, target, glm::vec3(0.0f, 1.0f, 0.0f))));
		}
		float ViewDepth(const glm::vec3& world) const { return -(glm::transpose(Rotation) * (world - Position)).z; }
	};

	glm::dmat3 GetLightRotation(const glm::vec3& direction)
	{
		const glm::vec3 light = glm::normalize(direction);
		const glm::vec3 up = std::abs(light.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		return glm::dmat3(glm::mat3(glm::lookAt(glm::vec3(0.0f), light, up)));
	}

	const glm::vec3 c_Sun = glm::vec3(-0.3f, -1.0f, -0.2f);

	ShadowFitView MakeView(const TestCamera& camera, const glm::vec3& lightDirection = c_Sun, float shadowDistance = 80.0f)
	{
		ShadowFitView view;
		view.CameraPosition = glm::dvec3(camera.Position);
		view.CameraRotation = camera.Rotation;
		view.LightRotation = GetLightRotation(lightDirection);
		view.Near = 0.1f;
		view.ShadowDistance = shadowDistance;
		view.Orthographic = camera.Orthographic;
		view.TanHalfFOV = camera.Orthographic ? 0.0f : std::tan(camera.VerticalFOV * 0.5f);
		view.OrthographicHeight = camera.Orthographic ? camera.OrthographicHeight : 0.0f;
		view.Aspect = camera.Aspect;
		view.CascadeCount = 4;
		view.MapSize = 4096;
		view.CascadeBlend = 0.1f;
		view.Distribution = 0.9f;
		return view;
	}

	// What the depth holds where rays through a grid of 160 x 90 pixels hit the ground (y = 0) within `maxDistance` of view
	// depth, inside the board |x|, |z| <= `halfBoard` (the rest is sky).
	std::vector<glm::vec3> GetGroundReceivers(const TestCamera& camera, float maxDistance, float halfBoard = 1.0e6f)
	{
		std::vector<glm::vec3> points;
		const float tanY = std::tan(camera.VerticalFOV * 0.5f);
		const float tanX = tanY * camera.Aspect;
		for (int row = 0; row < 90; row++)
		{
			for (int column = 0; column < 160; column++)
			{
				const glm::vec2 ndc((2.0f * (static_cast<float>(column) + 0.5f) / 160.0f - 1.0f), (1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / 90.0f));
				glm::vec3 origin = camera.Position;
				glm::vec3 direction;
				if (camera.Orthographic)
				{
					origin += camera.Rotation * glm::vec3(ndc.x * camera.OrthographicHeight * 0.5f * camera.Aspect, ndc.y * camera.OrthographicHeight * 0.5f, 0.0f);
					direction = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
				}
				else
				{
					direction = camera.Rotation * glm::vec3(ndc.x * tanX, ndc.y * tanY, -1.0f); // View depth 1 per unit
				}
				if (!(direction.y < -1e-4f))
					continue;
				const float t = origin.y / -direction.y;
				const glm::vec3 point = origin + direction * t;
				if (camera.ViewDepth(point) > maxDistance || std::abs(point.x) > halfBoard || std::abs(point.z) > halfBoard)
					continue;
				points.push_back(point);
			}
		}
		return points;
	}

	// A readback of Scene/ShadowFit.comp's reduction of receivers seen from `camera`.
	ShadowFitReadback MakeReadback(const TestCamera& camera, const std::vector<glm::vec3>& receivers, const ShadowFitTracker& tracker, uint64_t render)
	{
		ShadowFitReadback readback;
		readback.Render = render;
		readback.Epoch = tracker.GetEpoch();
		readback.LayoutVersion = tracker.GetLayoutVersion();
		readback.CameraPosition = glm::dvec3(camera.Position);
		readback.CameraRotation = glm::dmat3(camera.Rotation);
		RenderData::ShadowFitResult& result = readback.Result;
		for (const glm::vec3& world : receivers)
		{
			const glm::vec3 view = glm::transpose(camera.Rotation) * (world - camera.Position);
			const float distance = -view.z;
			const int bin = std::clamp(static_cast<int>(std::floor(4.0f * std::log2(distance / RenderData::c_ShadowFitBinBase))), 0,
				static_cast<int>(RenderData::c_ShadowFitBins) - 1);
			RenderData::ShadowFitBin& out = result.Bins[bin];
			out.MinX = std::max(out.MinX, ~EncodeOrdered(view.x));
			out.MinY = std::max(out.MinY, ~EncodeOrdered(view.y));
			out.MinD = std::max(out.MinD, ~EncodeOrdered(distance));
			out.MaxX = std::max(out.MaxX, EncodeOrdered(view.x));
			out.MaxY = std::max(out.MaxY, EncodeOrdered(view.y));
			out.MaxD = std::max(out.MaxD, EncodeOrdered(distance));
			out.Pixels++;
			result.Nearest = std::max(result.Nearest, ~EncodeOrdered(distance));
			result.Farthest = std::max(result.Farthest, EncodeOrdered(distance));
			result.SurfacePixels++;
		}
		return readback;
	}

	// Whether a fitted square holds a world position `marginTexels` of its own texels inside its edges.
	bool SquareHolds(const ShadowFitCascade& cascade, const ShadowFitView& view, const glm::vec3& world, double marginTexels)
	{
		const glm::dvec3 light = view.LightRotation * glm::dvec3(world);
		const double half = 0.5 * static_cast<double>(cascade.Side) - marginTexels * static_cast<double>(cascade.Side) / static_cast<double>(view.MapSize);
		return std::abs(light.x - cascade.Center.x) <= half && std::abs(light.y - cascade.Center.y) <= half;
	}

	// Every receiver whose view depth lies in a fitted cascade's slice (the first one's from the camera on) is inside its square
	// by the filter's reach, as the shaders' containment test asks.
	void CheckSquaresHold(const ShadowFitLayout& layout, const ShadowFitView& view, const TestCamera& camera, const std::vector<glm::vec3>& receivers)
	{
		for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
		{
			const ShadowFitCascade& slice = layout.Cascades[cascade];
			if (!slice.Fitted)
				continue;
			const float sliceNear = cascade == 0 ? 0.0f : slice.SliceNear;
			uint32_t missed = 0;
			for (const glm::vec3& point : receivers)
			{
				const float depth = camera.ViewDepth(point);
				if (depth >= sliceNear && depth <= slice.SliceFar && !SquareHolds(slice, view, point, ShadowFitTracker::c_FilterReachTexels))
					missed++;
			}
			CHECK_MESSAGE(missed == 0, "cascade ", cascade);
		}
	}

	// A board seen from above at 45 degrees (the Physics Playground's framing): receivers fill part of the view only.
	TestCamera BoardCamera()
	{
		TestCamera camera;
		camera.Position = glm::vec3(0.0f, 14.0f, 14.0f);
		camera.LookAt(glm::vec3(0.0f));
		return camera;
	}

}

TEST_SUITE("Renderer.ShadowFit")
{
	TEST_CASE("Bounds and sides are quarter stops")
	{
		CHECK(ShadowFitTracker::QuartersBelow(1.0) == 0);
		CHECK(ShadowFitTracker::QuartersAbove(1.0) == 0);
		CHECK(ShadowFitTracker::QuartersBelow(ShadowFitTracker::FromQuarters(5)) == 5);
		CHECK(ShadowFitTracker::QuartersAbove(ShadowFitTracker::FromQuarters(-7)) == -7);
		CHECK(ShadowFitTracker::QuartersBelow(1.18) == 0);
		CHECK(ShadowFitTracker::QuartersAbove(1.19) == 2);
		CHECK(ShadowFitTracker::FromQuarters(4) == doctest::Approx(2.0));

		const TestCamera camera = BoardCamera();
		const ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const ShadowFitReadback readback = MakeReadback(camera, GetGroundReceivers(camera, 1000.0f, 10.0f), tracker, 1);
		const ShadowFitLayout& layout = tracker.Update(view, &readback);
		REQUIRE(layout.Fitted);
		CHECK(layout.SquaresFitted);
		const auto isQuarterStop = [](float value) { const double quarters = 4.0 * std::log2(static_cast<double>(value)); return std::abs(quarters - std::round(quarters)) < 1e-4; };
		CHECK(isQuarterStop(layout.Near));
		CHECK(layout.Far < view.ShadowDistance);
		CHECK(isQuarterStop(layout.Far));
		for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
		{
			if (layout.Cascades[cascade].Fitted)
				CHECK(isQuarterStop(layout.Cascades[cascade].Side));
		}
	}

	TEST_CASE("Strata 0.6.0's layout until the first readback of an epoch, which sets the fitted layout at once")
	{
		const TestCamera camera;
		ShadowFitView view = MakeView(camera, c_Sun, 50.0f);
		ShadowFitTracker tracker;
		const ShadowFitLayout unfitted = tracker.Update(view, nullptr);
		CHECK_FALSE(unfitted.Fitted);
		CHECK_FALSE(unfitted.SquaresFitted);
		CHECK_FALSE(tracker.IsSettled());
		// 0.8 logarithmic and 0.2 uniform splits from the near clip of 0.1 m to 50 m.
		REQUIRE(unfitted.CascadeCount == 4);
		CHECK(unfitted.Cascades[0].SliceFar == doctest::Approx(2.89f).epsilon(0.005));
		CHECK(unfitted.Cascades[1].SliceFar == doctest::Approx(6.80f).epsilon(0.005));
		CHECK(unfitted.Cascades[2].SliceFar == doctest::Approx(15.96f).epsilon(0.005));
		CHECK(unfitted.Cascades[3].SliceFar == doctest::Approx(50.0f));
		CHECK(unfitted.Cascades[0].SliceNear == doctest::Approx(0.1f));

		// The first readback sets the bounds directly (no shrinking from the unfitted layout's [0.1, 50]).
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 1000.0f);
		const ShadowFitReadback first = MakeReadback(camera, ground, tracker, 1);
		const ShadowFitLayout fitted = tracker.Update(view, &first);
		CHECK(fitted.Fitted);
		CHECK(fitted.Changed);
		CHECK_FALSE(tracker.IsSettled());
		CHECK(fitted.Near > 1.0f); // The nearest ground is about 3 m away
		CHECK(fitted.Far == view.ShadowDistance);
		CHECK(fitted.Cascades[3].SliceFar == view.ShadowDistance);
		CHECK(tracker.GetChanges() == 1);

		// A readback of a frame drawn with this layout that keeps it settles it.
		const ShadowFitReadback confirm = MakeReadback(camera, ground, tracker, 2);
		CHECK_FALSE(tracker.Update(view, &confirm).Changed);
		CHECK(tracker.IsSettled());

		// Without receivers (the sky alone): 0.6.0's layout at once; the next readback with receivers sets them at once again.
		const ShadowFitReadback empty = MakeReadback(camera, {}, tracker, 3);
		const ShadowFitLayout sky = tracker.Update(view, &empty);
		CHECK_FALSE(sky.Fitted);
		CHECK(sky.Cascades[0].SliceFar == unfitted.Cascades[0].SliceFar);
		const ShadowFitReadback again = MakeReadback(camera, ground, tracker, 4);
		CHECK(tracker.Update(view, &again).Near == fitted.Near);

		// A new epoch: 0.6.0's layout, and readbacks of frames drawn before it are ignored.
		const ShadowFitReadback stale = MakeReadback(camera, ground, tracker, 5);
		tracker.Reset();
		CHECK_FALSE(tracker.Update(view, nullptr).Fitted);
		CHECK_FALSE(tracker.Update(view, &stale).Fitted);
		CHECK_FALSE(tracker.IsSettled());
		const ShadowFitReadback fresh = MakeReadback(camera, ground, tracker, 6);
		CHECK(tracker.Update(view, &fresh).Fitted);

		// Receivers all beyond the shadow distance: nothing to fit.
		tracker.Reset();
		view.ShadowDistance = 2.0f;
		const ShadowFitReadback distant = MakeReadback(camera, ground, tracker, 7);
		CHECK_FALSE(tracker.Update(view, &distant).Fitted);
	}

	TEST_CASE("Bounds grow at once and shrink after eight readbacks in a row, to the least shrunk of them")
	{
		const TestCamera camera;
		const ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		uint64_t render = 1;
		const std::vector<glm::vec3> near20 = GetGroundReceivers(camera, 20.0f);
		ShadowFitReadback readback = MakeReadback(camera, near20, tracker, render++);
		const float far20 = tracker.Update(view, &readback).Far;
		CHECK(far20 < view.ShadowDistance);

		// Farther receivers: the far bound grows in the frame their readback arrives.
		std::vector<glm::vec3> near30 = GetGroundReceivers(camera, 30.0f);
		readback = MakeReadback(camera, near30, tracker, render++);
		const float far30 = tracker.Update(view, &readback).Far;
		CHECK(far30 > far20);

		// Seven readbacks asking for less keep it; the eighth shrinks it to the least shrunk they asked for (24 m's, not 20's).
		const std::vector<glm::vec3> near24 = GetGroundReceivers(camera, 24.0f);
		for (uint32_t index = 0; index < ShadowFitTracker::c_ShrinkReadbacks - 1; index++)
		{
			readback = MakeReadback(camera, index == 3 ? near24 : near20, tracker, render++);
			CHECK(tracker.Update(view, &readback).Far == far30);
			// Frames without a new readback do not count.
			CHECK(tracker.Update(view, nullptr).Far == far30);
		}
		readback = MakeReadback(camera, near20, tracker, render++);
		const float shrunk = tracker.Update(view, &readback).Far;
		CHECK(shrunk < far30);
		CHECK(shrunk >= far20);
		ShadowFitTracker reference;
		reference.Update(view, nullptr);
		const ShadowFitReadback only24 = MakeReadback(camera, near24, reference, 1);
		CHECK(shrunk == reference.Update(view, &only24).Far);

		// A readback that wants as much as there is breaks a run of shrinking ones.
		for (uint32_t index = 0; index < ShadowFitTracker::c_ShrinkReadbacks + 2; index++)
		{
			readback = MakeReadback(camera, index == ShadowFitTracker::c_ShrinkReadbacks - 2 ? near24 : near20, tracker, render++);
			tracker.Update(view, &readback);
		}
		CHECK(tracker.GetLayout().Far == shrunk);

		// The near bound likewise: a closer receiver (on more than 1% of the pixels: no outlier) lowers it at once.
		const float nearBefore = tracker.GetLayout().Near;
		std::vector<glm::vec3> closer = near20;
		for (size_t pixel = 0; pixel < near20.size() / 50; pixel++)
			closer.push_back(glm::vec3(0.001f * static_cast<float>(pixel), 0.0f, -1.0f));
		readback = MakeReadback(camera, closer, tracker, render++);
		CHECK(tracker.Update(view, &readback).Near < nearBefore);
	}

	TEST_CASE("The far bound is the shadow distance from 0.7 of it; below, the other cascades end at it and the last one at the shadow distance")
	{
		const TestCamera camera;
		const ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		// Receivers to 20 m: F is the quarter stop at or above the farthest one's distance times 2^0.5, below 0.7 D.
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 20.0f);
		float farthest = 0.0f;
		for (const glm::vec3& point : ground)
			farthest = std::max(farthest, camera.ViewDepth(point));
		ShadowFitReadback readback = MakeReadback(camera, ground, tracker, 1);
		const ShadowFitLayout& layout = tracker.Update(view, &readback);
		REQUIRE(layout.Fitted);
		CHECK(layout.Far >= farthest * std::sqrt(2.0f));
		CHECK(layout.Far < farthest * std::sqrt(2.0f) * std::exp2(0.25f));
		CHECK(layout.Far < 0.7f * view.ShadowDistance);
		CHECK(layout.Cascades[2].SliceFar == doctest::Approx(layout.Far));
		CHECK(layout.Cascades[3].SliceFar == view.ShadowDistance);
		CHECK(layout.Cascades[3].SliceNear < layout.Far);
		CHECK(layout.Cascades[0].SliceNear == doctest::Approx(layout.Near));

		// Receivers to 60 m (the farthest row of the grid at 53 m): times 2^0.5 beyond 0.7 D, so the slices reach D.
		tracker.Reset();
		tracker.Update(view, nullptr);
		readback = MakeReadback(camera, GetGroundReceivers(camera, 60.0f), tracker, 2);
		const ShadowFitLayout& reaching = tracker.Update(view, &readback);
		CHECK(reaching.Far == view.ShadowDistance);
		CHECK(reaching.Cascades[2].SliceFar < reaching.Cascades[3].SliceFar);
		// The splits follow the distribution over [N, D].
		CHECK(reaching.Cascades[0].SliceFar == doctest::Approx(ShadowFitTracker::GetSplit(reaching.Near, view.ShadowDistance, 0.9f, 0, 4)));

		// A single cascade always ends at the shadow distance.
		ShadowFitView single = view;
		single.CascadeCount = 1;
		ShadowFitTracker one;
		one.Update(single, nullptr);
		readback = MakeReadback(camera, GetGroundReceivers(camera, 20.0f), one, 1);
		const ShadowFitLayout& alone = one.Update(single, &readback);
		CHECK(alone.Fitted);
		CHECK(alone.Far == single.ShadowDistance);
		CHECK(alone.Cascades[0].SliceFar == single.ShadowDistance);
	}

	TEST_CASE("The nearest transparent surface bounds the near end")
	{
		const TestCamera camera;
		ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 1000.0f);
		ShadowFitReadback readback = MakeReadback(camera, ground, tracker, 1);
		const float opaqueNear = tracker.Update(view, &readback).Near;
		CHECK(opaqueNear > 1.0f);
		// Glass 0.8 m away, in no depth the readback measured: N falls below 0.8 / 2^0.5 at once.
		view.NearestTransparent = 0.8f;
		const float withGlass = tracker.Update(view, nullptr).Near;
		CHECK(withGlass <= 0.8f / std::sqrt(2.0f));
		CHECK(withGlass > 0.25f);
	}

	TEST_CASE("Receivers at the camera holding under 1% of the pixels neither pull the near end nor grow the squares")
	{
		const TestCamera camera = BoardCamera();
		const ShadowFitView view = MakeView(camera);
		const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
		const auto fit = [&](const std::vector<glm::vec3>& receivers)
		{
			ShadowFitTracker tracker;
			tracker.Update(view, nullptr);
			const ShadowFitReadback readback = MakeReadback(camera, receivers, tracker, 1);
			return tracker.Update(view, &readback);
		};
		const ShadowFitLayout plain = fit(board);
		REQUIRE(plain.SquaresFitted);
		CHECK(plain.Near > 5.0f);

		// A leaf a metre from the camera, on fewer pixels than 1% of the receivers': the layout is the board's alone.
		const glm::vec3 leafCenter = camera.Position + camera.Rotation * glm::vec3(0.3f, -0.2f, -1.0f);
		std::vector<glm::vec3> withLeaf = board;
		const size_t leafPixels = board.size() / 200;
		for (size_t pixel = 0; pixel < leafPixels; pixel++)
			withLeaf.push_back(leafCenter + camera.Rotation * glm::vec3(0.01f * static_cast<float>(pixel % 10), 0.01f * static_cast<float>(pixel / 10), 0.0f));
		const ShadowFitLayout leaf = fit(withLeaf);
		CHECK(leaf.Near == plain.Near);
		for (uint32_t cascade = 0; cascade < plain.CascadeCount; cascade++)
		{
			CHECK(leaf.Cascades[cascade].SliceFar == plain.Cascades[cascade].SliceFar);
			CHECK(leaf.Cascades[cascade].Side == plain.Cascades[cascade].Side);
		}

		// On more than 1% of them it is a receiver like any other: the near end comes to it.
		std::vector<glm::vec3> withBranch = board;
		for (size_t pixel = 0; pixel < board.size() / 50; pixel++)
			withBranch.push_back(leafCenter + camera.Rotation * glm::vec3(0.01f * static_cast<float>(pixel % 20), 0.01f * static_cast<float>(pixel / 20), 0.0f));
		CHECK(fit(withBranch).Near < 1.0f);
	}

	TEST_CASE("The last cascade keeps its slice's sphere, that of [N, D] for narrow fields of view")
	{
		TestCamera camera;
		ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 1000.0f);
		ShadowFitReadback readback = MakeReadback(camera, ground, tracker, 1);
		const ShadowFitLayout wide = tracker.Update(view, &readback);
		const ShadowFitCascade& wideLast = wide.Cascades[3];
		CHECK_FALSE(wideLast.Fitted);
		// At 60 degrees, the sphere of the last slice itself.
		ShadowFitView sliceView = view;
		sliceView.CascadeCount = 1;
		sliceView.Near = wideLast.SliceNear;
		CHECK(wideLast.Radius == ShadowFitTracker::GetUnfittedLayout(sliceView).Cascades[0].Radius);

		// At 20 degrees, k = tan^2(10) (1 + aspect^2) = 0.13: the sphere of [N, D].
		camera.VerticalFOV = glm::radians(20.0f);
		view = MakeView(camera);
		ShadowFitTracker narrow;
		narrow.Update(view, nullptr);
		readback = MakeReadback(camera, GetGroundReceivers(camera, 1000.0f), narrow, 1);
		const ShadowFitLayout& narrowLayout = narrow.Update(view, &readback);
		REQUIRE(narrowLayout.Fitted);
		ShadowFitView whole = view;
		whole.CascadeCount = 1;
		whole.Near = narrowLayout.Near;
		CHECK_FALSE(narrowLayout.Cascades[3].Fitted);
		CHECK(narrowLayout.Cascades[3].Radius == ShadowFitTracker::GetUnfittedLayout(whole).Cascades[0].Radius);
		CHECK(narrowLayout.Cascades[3].SliceNear > narrowLayout.Near); // Its slice is still its own
	}

	TEST_CASE("Fitted squares hold every receiver of their slice by the filter's reach, within a sixteenth of the sphere and the sphere")
	{
		const TestCamera camera = BoardCamera();
		const ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
		const ShadowFitReadback readback = MakeReadback(camera, board, tracker, 1);
		const ShadowFitLayout& layout = tracker.Update(view, &readback);
		REQUIRE(layout.Fitted);
		uint32_t fitted = 0;
		for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
		{
			const ShadowFitCascade& slice = layout.Cascades[cascade];
			if (!slice.Fitted)
				continue;
			fitted++;
			CHECK(cascade + 1 < layout.CascadeCount); // Never the last
			CHECK(slice.Side >= 2.0f * slice.Radius / 16.0f);
			CHECK(slice.Side < 2.0f * slice.Radius);
			// On whole texels of absolute light space.
			const double worldPerTexel = static_cast<double>(slice.Side) / static_cast<double>(view.MapSize);
			CHECK(std::abs(slice.Center.x / worldPerTexel - std::round(slice.Center.x / worldPerTexel)) < 1e-3);
		}
		CHECK(fitted >= 2);
		CheckSquaresHold(layout, view, camera, board);

		// A tiny receiver: the square is a sixteenth of the sphere, not smaller.
		ShadowFitTracker tiny;
		tiny.Update(view, nullptr);
		const ShadowFitReadback single = MakeReadback(camera, { glm::vec3(0.0f) }, tiny, 1);
		const ShadowFitLayout& point = tiny.Update(view, &single);
		bool any = false;
		for (uint32_t cascade = 0; cascade < point.CascadeCount; cascade++)
		{
			const ShadowFitCascade& slice = point.Cascades[cascade];
			if (!slice.Fitted)
				continue;
			any = true;
			CHECK(slice.Side == doctest::Approx(ShadowFitTracker::FromQuarters(ShadowFitTracker::QuartersAbove(2.0 * slice.Radius / 16.0))));
		}
		CHECK(any);
	}

	TEST_CASE("Squares follow the receivers through a moving camera and a turning sun, without falling back to spheres")
	{
		TestCamera camera = BoardCamera();
		ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
		const ShadowFitReadback readback = MakeReadback(camera, board, tracker, 1);
		const ShadowFitLayout still = tracker.Update(view, &readback);
		REQUIRE(still.SquaresFitted);

		// The camera moves 1.5 m sideways before the next readback: the bins are re-projected through the readback's camera, so
		// the squares stay on the same receivers in absolute light space (and hold them).
		camera.Position += glm::vec3(1.5f, 0.0f, 0.0f);
		view = MakeView(camera);
		const ShadowFitLayout moved = tracker.Update(view, nullptr);
		CHECK(moved.SquaresFitted);
		CheckSquaresHold(moved, view, camera, board);
		for (uint32_t cascade = 0; cascade + 1 < moved.CascadeCount; cascade++)
		{
			if (!moved.Cascades[cascade].Fitted || !still.Cascades[cascade].Fitted)
				continue;
			const double worldPerTexel = static_cast<double>(moved.Cascades[cascade].Side) / static_cast<double>(view.MapSize);
			CHECK(glm::length(moved.Cascades[cascade].Center - still.Cascades[cascade].Center) <= 2.0 * worldPerTexel + 1e-9);
		}

		// The sun turns 5 degrees: the receivers re-projected into its new light space are held, by squares.
		const glm::vec3 turned = glm::vec3(glm::rotate(glm::mat4(1.0f), glm::radians(5.0f), glm::vec3(0.0f, 0.0f, 1.0f)) * glm::vec4(c_Sun, 0.0f));
		view = MakeView(camera, turned);
		const ShadowFitLayout sunTurned = tracker.Update(view, nullptr);
		CHECK(sunTurned.SquaresFitted);
		CheckSquaresHold(sunTurned, view, camera, board);
	}

	TEST_CASE("A turning camera grows the squares by how far the slices' corners moved since the readback")
	{
		TestCamera camera = BoardCamera();
		ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
		const ShadowFitReadback readback = MakeReadback(camera, board, tracker, 1);
		const ShadowFitLayout still = tracker.Update(view, &readback);
		REQUIRE(still.SquaresFitted);

		// Turned 6 degrees about the vertical, no new readback: the squares grow to hold what the turn may have revealed.
		camera.Rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(6.0f), glm::vec3(0.0f, 1.0f, 0.0f))) * camera.Rotation;
		view = MakeView(camera);
		const ShadowFitLayout turned = tracker.Update(view, nullptr);
		bool grew = false;
		for (uint32_t cascade = 0; cascade + 1 < turned.CascadeCount; cascade++)
		{
			const ShadowFitCascade& before = still.Cascades[cascade];
			const ShadowFitCascade& after = turned.Cascades[cascade];
			if (!before.Fitted)
				continue;
			CHECK((!after.Fitted || after.Side >= before.Side));
			grew = grew || !after.Fitted || after.Side > before.Side;
		}
		CHECK(grew);
		CHECK(turned.Changed);
		CheckSquaresHold(turned, view, camera, board);
	}

	TEST_CASE("Orthographic views keep their spheres and fit the depth range")
	{
		TestCamera camera = BoardCamera();
		camera.Orthographic = true;
		camera.OrthographicHeight = 24.0f;
		const ShadowFitView view = MakeView(camera);
		ShadowFitTracker tracker;
		tracker.Update(view, nullptr);
		const ShadowFitReadback readback = MakeReadback(camera, GetGroundReceivers(camera, 1000.0f, 10.0f), tracker, 1);
		const ShadowFitLayout& layout = tracker.Update(view, &readback);
		CHECK(layout.Fitted);
		CHECK_FALSE(layout.SquaresFitted);
		CHECK(layout.Near > view.Near);
		for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
			CHECK_FALSE(layout.Cascades[cascade].Fitted);
	}

	TEST_CASE("The same readbacks and views give the same layouts")
	{
		const auto run = []()
		{
			TestCamera camera = BoardCamera();
			ShadowFitTracker tracker;
			std::vector<ShadowFitLayout> layouts;
			const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
			for (uint64_t frame = 1; frame <= 12; frame++)
			{
				const ShadowFitView view = MakeView(camera);
				const ShadowFitReadback readback = MakeReadback(camera, board, tracker, frame);
				layouts.push_back(tracker.Update(view, frame % 3 == 0 ? nullptr : &readback));
				camera.Position += glm::vec3(0.07f, 0.0f, -0.05f);
			}
			return layouts;
		};
		const std::vector<ShadowFitLayout> first = run();
		const std::vector<ShadowFitLayout> second = run();
		REQUIRE(first.size() == second.size());
		for (size_t frame = 0; frame < first.size(); frame++)
		{
			CHECK(first[frame].Near == second[frame].Near);
			CHECK(first[frame].Far == second[frame].Far);
			for (uint32_t cascade = 0; cascade < first[frame].CascadeCount; cascade++)
			{
				CHECK(first[frame].Cascades[cascade].SliceFar == second[frame].Cascades[cascade].SliceFar);
				CHECK(first[frame].Cascades[cascade].Side == second[frame].Cascades[cascade].Side);
				CHECK(first[frame].Cascades[cascade].Center == second[frame].Cascades[cascade].Center);
			}
		}
	}
}
