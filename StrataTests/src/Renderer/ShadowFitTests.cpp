#include <doctest/doctest.h>

#include "Strata/Renderer/ShadowFit.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
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

	// The grid of GetGroundReceivers with a ball of `radius` at `center` in front of the ground: what the depth holds with a
	// ball in the air (perspective cameras).
	std::vector<glm::vec3> GetReceiversWithBall(const TestCamera& camera, float maxDistance, float halfBoard, const glm::vec3& center, float radius)
	{
		std::vector<glm::vec3> points;
		const float tanY = std::tan(camera.VerticalFOV * 0.5f);
		const float tanX = tanY * camera.Aspect;
		for (int row = 0; row < 90; row++)
		{
			for (int column = 0; column < 160; column++)
			{
				const glm::vec2 ndc((2.0f * (static_cast<float>(column) + 0.5f) / 160.0f - 1.0f), (1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / 90.0f));
				const glm::vec3 direction = glm::normalize(camera.Rotation * glm::vec3(ndc.x * tanX, ndc.y * tanY, -1.0f));
				const glm::vec3 toCenter = center - camera.Position;
				const float along = glm::dot(toCenter, direction);
				const float offAxis = glm::dot(toCenter, toCenter) - along * along;
				if (along > 0.0f && offAxis < radius * radius)
				{
					points.push_back(camera.Position + direction * (along - std::sqrt(radius * radius - offAxis)));
					continue;
				}
				if (!(direction.y < -1e-4f))
					continue;
				const glm::vec3 point = camera.Position + direction * (camera.Position.y / -direction.y);
				if (camera.ViewDepth(point) > maxDistance || std::abs(point.x) > halfBoard || std::abs(point.z) > halfBoard)
					continue;
				points.push_back(point);
			}
		}
		return points;
	}

	// Receivers on a patch of the screen (`ndcMin` to `ndcMax`), `columns` x `rows` of them, their view depth from `nearDepth`
	// on the first row to `farDepth` on the last (a slope facing the camera).
	std::vector<glm::vec3> GetScreenPatch(const TestCamera& camera, float nearDepth, float farDepth, glm::vec2 ndcMin, glm::vec2 ndcMax, int columns, int rows)
	{
		std::vector<glm::vec3> points;
		const float tanY = std::tan(camera.VerticalFOV * 0.5f);
		for (int row = 0; row < rows; row++)
		{
			for (int column = 0; column < columns; column++)
			{
				const glm::vec2 ndc = glm::mix(ndcMin, ndcMax, glm::vec2((static_cast<float>(column) + 0.5f) / static_cast<float>(columns),
					(static_cast<float>(row) + 0.5f) / static_cast<float>(rows)));
				const float depth = glm::mix(nearDepth, farDepth, (static_cast<float>(row) + 0.5f) / static_cast<float>(rows));
				const glm::vec2 halfSize = camera.Orthographic ? 0.5f * camera.OrthographicHeight * glm::vec2(camera.Aspect, 1.0f)
					: depth * tanY * glm::vec2(camera.Aspect, 1.0f);
				points.push_back(camera.Position + camera.Rotation * glm::vec3(ndc * halfSize, -depth));
			}
		}
		return points;
	}

	// The share of `points` nearer to `camera` than `depth` (view depth).
	double ShareNearer(const TestCamera& camera, const std::vector<glm::vec3>& points, float depth)
	{
		size_t nearer = 0;
		for (const glm::vec3& point : points)
			nearer += camera.ViewDepth(point) < depth ? 1 : 0;
		return static_cast<double>(nearer) / static_cast<double>(std::max<size_t>(points.size(), 1));
	}

	// `points` with receivers added at `depth` in front of the camera, so that they hold `share` of the result.
	std::vector<glm::vec3> WithNearReceivers(const TestCamera& camera, std::vector<glm::vec3> points, float depth, double share)
	{
		const size_t count = static_cast<size_t>(std::ceil(share / (1.0 - share) * static_cast<double>(points.size())));
		for (size_t index = 0; index < count; index++)
			points.push_back(camera.Position + camera.Rotation * glm::vec3(0.001f * static_cast<float>(index % 100), 0.001f * static_cast<float>(index / 100), -depth));
		return points;
	}

	// The Physics Playground's default view: from (0, 22, 30) toward the origin.
	TestCamera PlaygroundDefaultCamera()
	{
		TestCamera camera;
		camera.Position = glm::vec3(0.0f, 22.0f, 30.0f);
		camera.LookAt(glm::vec3(0.0f));
		return camera;
	}

	// An epoch's first readback and three confirming ones of `receivers`.
	void Settle(ShadowFitTracker& tracker, const ShadowFitView& view, const TestCamera& camera, const std::vector<glm::vec3>& receivers, uint64_t& render)
	{
		tracker.Update(view, nullptr);
		for (int frame = 0; frame < 4; frame++)
		{
			const ShadowFitReadback readback = MakeReadback(camera, receivers, tracker, render++);
			tracker.Update(view, &readback);
		}
	}

	// Updates of `seconds` each, every one with a readback of `receivers`, until `duration` passed.
	void Hold(ShadowFitTracker& tracker, const ShadowFitView& view, const TestCamera& camera, const std::vector<glm::vec3>& receivers, uint64_t& render,
		float duration)
	{
		const ShadowFitReadback base = MakeReadback(camera, receivers, tracker, render);
		const int updates = static_cast<int>(std::lround(duration / view.Seconds));
		for (int update = 0; update < updates; update++)
		{
			ShadowFitReadback readback = base;
			readback.Render = render++;
			readback.Epoch = tracker.GetEpoch();
			readback.LayoutVersion = tracker.GetLayoutVersion();
			tracker.Update(view, &readback);
		}
	}

	bool SameLayout(const ShadowFitLayout& a, const ShadowFitLayout& b)
	{
		if (a.Fitted != b.Fitted || a.Near != b.Near || a.Far != b.Far || a.CascadeCount != b.CascadeCount)
			return false;
		for (uint32_t cascade = 0; cascade < a.CascadeCount; cascade++)
		{
			const ShadowFitCascade& first = a.Cascades[cascade];
			const ShadowFitCascade& second = b.Cascades[cascade];
			if (first.SliceNear != second.SliceNear || first.SliceFar != second.SliceFar || first.Radius != second.Radius || first.Fitted != second.Fitted
				|| first.Side != second.Side || first.Center != second.Center)
				return false;
		}
		return true;
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

	TEST_CASE("Without holds the tracker keeps Strata 0.7.0's rules")
	{
		// Renderers of single images (ShadowFitView::Holds false): every growth at once, shrinks at the eighth readback in a row,
		// however little time passed.
		const TestCamera camera;
		ShadowFitView view = MakeView(camera);
		view.Holds = false;
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
		CHECK_FALSE(tracker.IsHolding());
	}

	TEST_CASE("Far bounds grow at once; bounds shrink after eight readbacks and half a second, to the least shrunk of them")
	{
		const TestCamera camera;
		ShadowFitView view = MakeView(camera);
		// The loop's 15 updates before the eighth readback last 0.75 s: past c_ShrinkSeconds.
		view.Seconds = 0.05f;
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

		// A closer receiver on 2% of the pixels (no outlier, but below c_NearNowShare) waits half a second on a still camera,
		// then lowers the near bound.
		const float nearBefore = tracker.GetLayout().Near;
		std::vector<glm::vec3> closer = near20;
		for (size_t pixel = 0; pixel < near20.size() / 50; pixel++)
			closer.push_back(glm::vec3(0.001f * static_cast<float>(pixel), 0.0f, -1.0f));
		readback = MakeReadback(camera, closer, tracker, render++);
		CHECK(tracker.Update(view, &readback).Near == nearBefore);
		CHECK(tracker.IsHolding());
		Hold(tracker, view, camera, closer, render, ShadowFitTracker::c_GrowSeconds);
		CHECK(tracker.GetLayout().Near < nearBefore);
		CHECK_FALSE(tracker.IsHolding());

		// One on 11.1% of them (near20.size() / 8 pixels: the share counts them in the total) lowers it at once.
		ShadowFitTracker wall;
		Settle(wall, view, camera, near20, render);
		const float wallBefore = wall.GetLayout().Near;
		std::vector<glm::vec3> large = near20;
		for (size_t pixel = 0; pixel < near20.size() / 8; pixel++)
			large.push_back(glm::vec3(0.001f * static_cast<float>(pixel), 0.0f, -1.0f));
		readback = MakeReadback(camera, large, wall, render++);
		CHECK(wall.Update(view, &readback).Near < wallBefore);
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

	TEST_CASE("Receivers left out of the near end stay inside the last cascade's sphere in narrow and orthographic views")
	{
		// A slope 57-79 m away fills the view, and a post 12-15 m away near its corner covers 0.5% of the pixels: under the
		// outliers' share. The last cascade's sphere holds what no square holds only while the near end lies within D k (20
		// degrees at 16:9: k = 0.13, D k = 10.3 m; orthographic: 0), so these views keep the post in the fit.
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			TestCamera camera;
			camera.VerticalFOV = glm::radians(20.0f);
			camera.Orthographic = orthographic;
			camera.OrthographicHeight = 20.0f;
			const ShadowFitView view = MakeView(camera);
			std::vector<glm::vec3> points = GetScreenPatch(camera, 57.0f, 79.0f, glm::vec2(-1.0f), glm::vec2(1.0f), 160, 90);
			const std::vector<glm::vec3> post = GetScreenPatch(camera, 12.0f, 15.0f, glm::vec2(-0.95f), glm::vec2(-0.85f, 0.0f), 4, 18);
			REQUIRE(post.size() * 200 <= points.size()); // At most 0.5%
			points.insert(points.end(), post.begin(), post.end());
			ShadowFitTracker tracker;
			tracker.Update(view, nullptr);
			const ShadowFitReadback readback = MakeReadback(camera, points, tracker, 1);
			const ShadowFitLayout& layout = tracker.Update(view, &readback);
			REQUIRE(layout.Fitted);
			CHECK(layout.Near <= 12.0f / std::sqrt(2.0f));
			const ShadowFitCascade& last = layout.Cascades[layout.CascadeCount - 1];
			for (const glm::vec3& point : post)
				CHECK(glm::length(point - camera.Position - last.SphereCenter) <= last.Radius);
		}
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

	TEST_CASE("A ball shot from the camera leaves the layout alone at any frame rate")
	{
		// The Physics Playground's ball (0.25 m) leaves the camera at 1.5 m along the view, at full and half power: at spawn it
		// holds about 7% of the receivers, under c_NearNowShare, and it is gone before c_GrowSeconds.
		const TestCamera camera = PlaygroundDefaultCamera();
		const glm::vec3 forward = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 70.0f, 20.0f);
		for (const float framesPerSecond : { 60.0f, 240.0f, 1000.0f })
		{
			for (const float speed : { 24.0f, 12.0f })
			{
				CAPTURE(framesPerSecond);
				CAPTURE(speed);
				ShadowFitView view = MakeView(camera, c_Sun, 70.0f);
				view.MapSize = 2048;
				view.Seconds = 1.0f / framesPerSecond;
				ShadowFitTracker tracker;
				uint64_t render = 1;
				Settle(tracker, view, camera, ground, render);
				REQUIRE(tracker.GetLayout().Fitted);
				const uint64_t changes = tracker.GetChanges();
				const float nearBound = tracker.GetLayout().Near;
				const int frames = static_cast<int>(std::lround(framesPerSecond));
				for (int frame = 0; frame < frames; frame++)
				{
					const glm::vec3 ball = camera.Position + forward * (1.5f + speed * static_cast<float>(frame) * view.Seconds);
					const ShadowFitReadback readback = MakeReadback(camera, GetReceiversWithBall(camera, 70.0f, 20.0f, ball, 0.25f), tracker, render++);
					tracker.Update(view, &readback);
				}
				CHECK(tracker.GetChanges() == changes);
				CHECK(tracker.GetLayout().Near == nearBound);
				CHECK_FALSE(tracker.IsHolding());
			}
		}
	}

	TEST_CASE("A receiver that stays near moves the near bound once it persisted, one that fills a tenth of the view at once")
	{
		const TestCamera camera = PlaygroundDefaultCamera();
		const glm::vec3 forward = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		const std::vector<glm::vec3> ground = GetGroundReceivers(camera, 70.0f, 20.0f);
		ShadowFitView view = MakeView(camera, c_Sun, 70.0f);
		view.Seconds = 1.0f / 60.0f;
		ShadowFitTracker tracker;
		uint64_t render = 1;
		Settle(tracker, view, camera, ground, render);
		const float nearBound = tracker.GetLayout().Near;

		// Parked 2 m away, on about 4% of the receivers: for 0.4 s nothing moves, after 0.6 s the bound lies below the ball.
		const std::vector<glm::vec3> parked = GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 2.0f, 0.25f);
		const double share = ShareNearer(camera, parked, nearBound);
		CAPTURE(share);
		REQUIRE(share > ShadowFitTracker::c_NearOutlierShare);
		REQUIRE(share < ShadowFitTracker::c_NearNowShare);
		Hold(tracker, view, camera, parked, render, 0.4f);
		CHECK(tracker.GetLayout().Near == nearBound);
		CHECK(tracker.IsHolding());
		Hold(tracker, view, camera, parked, render, 0.2f);
		CHECK(tracker.GetLayout().Near < 1.75f);
		CHECK_FALSE(tracker.IsHolding());

		// A 2 m sphere 3 m away, on a tenth of the receivers or more, moves it in its first readback.
		ShadowFitTracker wall;
		Settle(wall, view, camera, ground, render);
		const std::vector<glm::vec3> large = GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 3.0f, 2.0f);
		REQUIRE(ShareNearer(camera, large, nearBound) >= ShadowFitTracker::c_NearNowShare);
		const ShadowFitReadback readback = MakeReadback(camera, large, wall, render++);
		CHECK(wall.Update(view, &readback).Near < 2.0f);
	}

	TEST_CASE("A moving camera waits an eighth of a second for the near bound, a still one half a second")
	{
		TestCamera camera = PlaygroundDefaultCamera();
		const glm::vec3 forward = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		const auto makeView = [](const TestCamera& from)
		{
			ShadowFitView view = MakeView(from, c_Sun, 70.0f);
			view.Seconds = 1.0f / 60.0f;
			return view;
		};
		ShadowFitTracker tracker;
		uint64_t render = 1;
		Settle(tracker, makeView(camera), camera, GetGroundReceivers(camera, 70.0f, 20.0f), render);
		const float nearBound = tracker.GetLayout().Near;

		// The parked ball moves with the camera, 1 mm a frame: c_GrowSecondsMoving (0.12 s) is not over after 0.1 s, and over
		// after 0.15 s.
		const auto step = [&]()
		{
			camera.Position.x += 0.001f;
			const std::vector<glm::vec3> parked = GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 2.0f, 0.25f);
			const ShadowFitReadback readback = MakeReadback(camera, parked, tracker, render++);
			return tracker.Update(makeView(camera), &readback).Near;
		};
		for (int frame = 0; frame < 6; frame++)
			CHECK(step() == nearBound);
		for (int frame = 0; frame < 3; frame++)
			step();
		CHECK(tracker.GetLayout().Near < 1.75f);

		// A still camera waits c_GrowSeconds: nothing after 0.4 s (the case above shows it moving after 0.6 s).
		ShadowFitTracker still;
		const ShadowFitView view = makeView(camera);
		Settle(still, view, camera, GetGroundReceivers(camera, 70.0f, 20.0f), render);
		Hold(still, view, camera, GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 2.0f, 0.25f), render, 0.4f);
		CHECK(still.GetLayout().Near == nearBound);
	}

	TEST_CASE("The nearest transparent surface moves the near bound at once while an opaque growth waits")
	{
		const TestCamera camera = PlaygroundDefaultCamera();
		const glm::vec3 forward = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		ShadowFitView view = MakeView(camera, c_Sun, 70.0f);
		view.Seconds = 1.0f / 60.0f;
		ShadowFitTracker tracker;
		uint64_t render = 1;
		Settle(tracker, view, camera, GetGroundReceivers(camera, 70.0f, 20.0f), render);
		const float nearBound = tracker.GetLayout().Near;

		// The parked ball (its nearest point 1.75 m away, about 4% of the receivers) waits.
		const std::vector<glm::vec3> parked = GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 2.0f, 0.25f);
		Hold(tracker, view, camera, parked, render, 0.2f);
		CHECK(tracker.GetLayout().Near == nearBound);
		CHECK(tracker.IsHolding());

		// Glass 2.5 m away, nearer than the bound but farther than the ball, in no depth a readback measures: the bound reaches
		// it at once, in a frame without a readback, and stays above the ball's bound, whose growth still waits.
		view.NearestTransparent = 2.5f;
		const float withGlass = tracker.Update(view, nullptr).Near;
		CHECK(withGlass <= 2.5f / std::sqrt(2.0f));
		CHECK(withGlass > 1.75f / std::sqrt(2.0f));
		CHECK(tracker.IsHolding());
		Hold(tracker, view, camera, parked, render, ShadowFitTracker::c_GrowSeconds);
		CHECK(tracker.GetLayout().Near <= 1.75f / std::sqrt(2.0f));
		CHECK_FALSE(tracker.IsHolding());
	}

	TEST_CASE("Narrow and orthographic views grow the near bound at once")
	{
		// A slope 57-79 m away fills a 20 degree view at 16:9 (D k = 10.3 m at D = 80) or an orthographic one: the near bound
		// lies beyond D k, where the last cascade's sphere would not hold a receiver nearer than it, so one on 4% of the
		// receivers moves it in its first readback.
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			TestCamera camera;
			camera.VerticalFOV = glm::radians(20.0f);
			camera.Orthographic = orthographic;
			camera.OrthographicHeight = 20.0f;
			ShadowFitView view = MakeView(camera);
			view.Seconds = 1.0f / 60.0f;
			const std::vector<glm::vec3> slope = GetScreenPatch(camera, 57.0f, 79.0f, glm::vec2(-1.0f), glm::vec2(1.0f), 160, 90);
			ShadowFitTracker tracker;
			uint64_t render = 1;
			Settle(tracker, view, camera, slope, render);
			const float nearBound = tracker.GetLayout().Near;
			const double tanY = std::tan(0.5 * static_cast<double>(camera.VerticalFOV));
			const double reach = orthographic ? 0.0 : tanY * tanY * (1.0 + static_cast<double>(camera.Aspect) * static_cast<double>(camera.Aspect));
			REQUIRE(static_cast<double>(nearBound) > static_cast<double>(view.ShadowDistance) * reach);
			const ShadowFitReadback readback = MakeReadback(camera, WithNearReceivers(camera, slope, 20.0f, 0.04), tracker, render++);
			CHECK(tracker.Update(view, &readback).Near <= 20.0f / std::sqrt(2.0f));
			CHECK_FALSE(tracker.IsHolding());
		}
	}

	TEST_CASE("A still camera keeps a square whole while its growth waits; a moving camera grows it at once")
	{
		TestCamera camera = BoardCamera();
		ShadowFitView view = MakeView(camera);
		view.Seconds = 1.0f / 60.0f;
		const std::vector<glm::vec3> board = GetGroundReceivers(camera, 1000.0f, 10.0f);
		ShadowFitTracker tracker;
		uint64_t render = 1;
		Settle(tracker, view, camera, board, render);
		const ShadowFitLayout settled = tracker.GetLayout();
		uint32_t cascade = 0;
		while (cascade + 1 < settled.CascadeCount && !settled.Cascades[cascade].Fitted)
			cascade++;
		CAPTURE(cascade);
		REQUIRE(cascade + 1 < settled.CascadeCount);
		const ShadowFitCascade before = settled.Cascades[cascade];

		// Floor beside the board within that cascade's slice and the board's range of depths widens its box: no bound moves.
		float nearest = std::numeric_limits<float>::max();
		float farthest = 0.0f;
		for (const glm::vec3& point : board)
		{
			nearest = std::min(nearest, camera.ViewDepth(point));
			farthest = std::max(farthest, camera.ViewDepth(point));
		}
		std::vector<glm::vec3> wider = board;
		for (const glm::vec3& point : GetGroundReceivers(camera, 1000.0f, 20.0f))
		{
			const float depth = camera.ViewDepth(point);
			if (std::abs(point.x) > 10.0f && std::abs(point.z) <= 10.0f && depth >= std::max(nearest, before.SliceNear) && depth <= std::min(farthest, before.SliceFar))
				wider.push_back(point);
		}
		REQUIRE(wider.size() > board.size());
		const uint64_t changes = tracker.GetChanges();
		Hold(tracker, view, camera, wider, render, 0.4f);
		CHECK(tracker.GetLayout().Cascades[cascade].Fitted);
		CHECK(tracker.GetLayout().Cascades[cascade].Side == before.Side);
		CHECK(tracker.GetLayout().Cascades[cascade].Center == before.Center);
		CHECK(tracker.GetChanges() == changes);
		CHECK(tracker.IsHolding());
		Hold(tracker, view, camera, wider, render, 0.2f);
		const ShadowFitCascade grown = tracker.GetLayout().Cascades[cascade];
		CHECK((!grown.Fitted || grown.Side > before.Side));
		CHECK(tracker.GetLayout().Near == settled.Near);
		CHECK_FALSE(tracker.IsHolding());

		// A camera moved 1 cm grows it in its first readback.
		ShadowFitTracker moving;
		Settle(moving, view, camera, board, render);
		camera.Position.x += 0.01f;
		ShadowFitView moved = MakeView(camera);
		moved.Seconds = view.Seconds;
		const ShadowFitReadback readback = MakeReadback(camera, wider, moving, render++);
		const ShadowFitCascade movedSquare = moving.Update(moved, &readback).Cascades[cascade];
		CHECK((!movedSquare.Fitted || movedSquare.Side > before.Side));
	}

	TEST_CASE("Shrinks wait for eight readbacks and half a second at any frame rate")
	{
		const TestCamera camera;
		const std::vector<glm::vec3> near20 = GetGroundReceivers(camera, 20.0f);
		const std::vector<glm::vec3> near24 = GetGroundReceivers(camera, 24.0f);
		const std::vector<glm::vec3> near30 = GetGroundReceivers(camera, 30.0f);
		ShadowFitTracker reference;
		reference.Update(MakeView(camera), nullptr);
		const ShadowFitReadback only24 = MakeReadback(camera, near24, reference, 1);
		const float far24 = reference.Update(MakeView(camera), &only24).Far;
		for (const float seconds : { 0.001f, 1.0f / 60.0f })
		{
			CAPTURE(seconds);
			ShadowFitView view = MakeView(camera);
			view.Seconds = seconds;
			ShadowFitTracker tracker;
			uint64_t render = 1;
			Settle(tracker, view, camera, near30, render);
			const float far30 = tracker.GetLayout().Far;
			REQUIRE(far24 < far30);
			// Eight readbacks asking for less (one of them 24 m's), in less than half a second: no shrink.
			for (uint32_t index = 0; index < ShadowFitTracker::c_ShrinkReadbacks; index++)
			{
				const ShadowFitReadback readback = MakeReadback(camera, index == 3 ? near24 : near20, tracker, render++);
				CHECK(tracker.Update(view, &readback).Far == far30);
			}
			// Asking on: the shrink comes once half a second passed, to the least shrunk of what they asked for.
			float elapsed = static_cast<float>(ShadowFitTracker::c_ShrinkReadbacks) * seconds;
			const ShadowFitReadback asking = MakeReadback(camera, near20, tracker, render);
			while (tracker.GetLayout().Far == far30 && elapsed < 1.0f)
			{
				ShadowFitReadback readback = asking;
				readback.Render = render++;
				readback.LayoutVersion = tracker.GetLayoutVersion();
				tracker.Update(view, &readback);
				elapsed += seconds;
			}
			CHECK(elapsed >= ShadowFitTracker::c_ShrinkSeconds - 0.002f);
			CHECK(elapsed <= ShadowFitTracker::c_ShrinkSeconds + seconds + 0.002f);
			CHECK(tracker.GetLayout().Far == far24);
		}
	}

	TEST_CASE("A waiting growth keeps the fit unsettled")
	{
		const TestCamera camera = PlaygroundDefaultCamera();
		const glm::vec3 forward = camera.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		ShadowFitView view = MakeView(camera, c_Sun, 70.0f);
		view.Seconds = 1.0f / 60.0f;
		ShadowFitTracker tracker;
		uint64_t render = 1;
		Settle(tracker, view, camera, GetGroundReceivers(camera, 70.0f, 20.0f), render);
		CHECK(tracker.IsSettled());
		CHECK_FALSE(tracker.IsHolding());
		const std::vector<glm::vec3> parked = GetReceiversWithBall(camera, 70.0f, 20.0f, camera.Position + forward * 2.0f, 0.25f);
		Hold(tracker, view, camera, parked, render, 0.3f);
		CHECK(tracker.IsHolding());
		CHECK_FALSE(tracker.IsSettled());
		// The growth is made, and a readback of a frame drawn with the new layout settles it.
		Hold(tracker, view, camera, parked, render, 0.3f);
		CHECK_FALSE(tracker.IsHolding());
		Hold(tracker, view, camera, parked, render, view.Seconds);
		CHECK(tracker.IsSettled());
	}

	TEST_CASE("Settled layouts are the same with and without holds")
	{
		// What the holds keep: wave 7's density. Once nothing waits, a tracker with holds lays out what one without them does.
		struct Framing
		{
			TestCamera Camera;
			std::vector<glm::vec3> Receivers;
		};
		std::vector<Framing> framings(2);
		framings[0].Camera = BoardCamera();
		framings[0].Receivers = GetGroundReceivers(framings[0].Camera, 1000.0f, 10.0f);
		framings[1].Receivers = GetGroundReceivers(framings[1].Camera, 1000.0f);
		for (const Framing& framing : framings)
		{
			ShadowFitView held = MakeView(framing.Camera);
			held.Seconds = 1.0f / 60.0f;
			ShadowFitView unheld = MakeView(framing.Camera);
			unheld.Holds = false;
			ShadowFitTracker withHolds;
			ShadowFitTracker withoutHolds;
			uint64_t render = 1;
			Settle(withHolds, held, framing.Camera, framing.Receivers, render);
			Settle(withoutHolds, unheld, framing.Camera, framing.Receivers, render);
			CHECK(withHolds.IsSettled());
			CHECK(withoutHolds.IsSettled());
			CHECK(withHolds.GetLayout().SquaresFitted == withoutHolds.GetLayout().SquaresFitted);
			CHECK(SameLayout(withHolds.GetLayout(), withoutHolds.GetLayout()));
		}
	}
}
