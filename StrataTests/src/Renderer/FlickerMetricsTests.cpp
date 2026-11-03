#include "Renderer/FlickerMetrics.h"

#include "Strata/Math/Math.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <vector>

// The wave's flicker meter (Renderer/FlickerMetrics.h) on synthetic frames: a camera looking along -Z at surfaces whose depth,
// normal and value the case chooses per pixel.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;
	constexpr float c_FieldOfView = 60.0f;

	struct SurfacePoint
	{
		float Depth = 10.0f;                         // Along the view; 0: nothing drawn
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f); // World space
	};

	// A frame of a camera at `camera` looking along -Z, its pixels' surfaces from `surfaceAt` and values from `valueAt` (of the
	// pixel's world point).
	FlickerFrame MakeFrame(const glm::dvec3& camera, const std::function<SurfacePoint(uint32_t, uint32_t)>& surfaceAt,
		const std::function<float(const glm::dvec3&, uint32_t, uint32_t)>& valueAt)
	{
		FlickerFrame frame;
		frame.Width = c_Size;
		frame.Height = c_Size;
		frame.CameraPosition = camera;
		frame.CameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
		frame.ViewProjection = Math::PerspectiveReverseZ(glm::radians(c_FieldOfView), 1.0f, 0.1f, 1000.0f); // The view has no rotation
		const float tangent = std::tan(glm::radians(c_FieldOfView) * 0.5f);
		const size_t pixels = static_cast<size_t>(c_Size) * c_Size;
		frame.Value.resize(pixels);
		frame.ViewDepth.resize(pixels);
		frame.Normal.resize(pixels);
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const size_t pixel = static_cast<size_t>(y) * c_Size + x;
				const SurfacePoint surface = surfaceAt(x, y);
				const double ndcX = 2.0 * (x + 0.5) / c_Size - 1.0;
				const double ndcY = 1.0 - 2.0 * (y + 0.5) / c_Size;
				const glm::dvec3 point = camera + static_cast<double>(surface.Depth) * glm::dvec3(ndcX * tangent, ndcY * tangent, -1.0);
				frame.ViewDepth[pixel] = surface.Depth;
				frame.Normal[pixel] = surface.Normal;
				frame.Value[pixel] = valueAt(point, x, y);
			}
		}
		return frame;
	}

	SurfacePoint Plane(uint32_t, uint32_t)
	{
		return SurfacePoint {};
	}

	// The world size of a pixel on the plane 10 m in front of the camera.
	double PixelMeters()
	{
		return 2.0 * 10.0 * std::tan(glm::radians(static_cast<double>(c_FieldOfView)) * 0.5) / c_Size;
	}

	FlickerFrame ConstantFrame(float value, const std::function<SurfacePoint(uint32_t, uint32_t)>& surfaceAt = Plane)
	{
		return MakeFrame(glm::dvec3(0.0, 0.0, 10.0), surfaceAt, [value](const glm::dvec3&, uint32_t, uint32_t) { return value; });
	}

	constexpr size_t c_InteriorPixels = static_cast<size_t>(c_Size - 4) * (c_Size - 4);

}

TEST_SUITE("Renderer.FlickerMetrics")
{
	TEST_CASE("A steady drift scores zero")
	{
		// A plane's values change linearly over it and in time, while the camera strafes 3.4 pixels a frame: what each world
		// point shows changes steadily, which is the view's real change, not flicker.
		std::vector<FlickerFrame> frames;
		for (int t = 0; t < 3; t++)
		{
			const glm::dvec3 camera(3.4 * PixelMeters() * t, 0.0, 10.0);
			frames.push_back(MakeFrame(camera, Plane, [t](const glm::dvec3& point, uint32_t, uint32_t)
			{
				return static_cast<float>(1.0 + 0.02 * point.x + 0.01 * point.y + 2.0 * t * 0.01);
			}));
		}
		const FlickerResult result = MeasureFlicker(frames[0], frames[1], frames[2]);
		CAPTURE(result.Mean);
		CAPTURE(result.Blocks);
		CHECK(result.MeasuredShare > 0.8);
		CHECK(result.Blocks < 1e-6);
		CHECK(result.Mean < 1e-5);
		CHECK(result.Frames == 1u);
	}

	TEST_CASE("An alternating value scores its amplitude")
	{
		const FlickerResult result = MeasureFlicker(ConstantFrame(1.0f), ConstantFrame(1.1f), ConstantFrame(1.0f));
		const double expected = 0.2 / (3.1 / 3.0);
		CHECK(result.Mean == doctest::Approx(expected).epsilon(1e-6));
		CHECK(result.P99 == doctest::Approx(expected).epsilon(1e-6));
		CHECK(result.Blocks == doctest::Approx(expected).epsilon(1e-6));
		CHECK(result.BlocksP99 == doctest::Approx(expected).epsilon(1e-6));
		CHECK(result.MeasuredShare == doctest::Approx(static_cast<double>(c_InteriorPixels) / (c_Size * c_Size)));
	}

	TEST_CASE("Depth and normal edges are left out")
	{
		// A still camera: every interior pixel of a plane is measured. A step in depth, or a crease in the normal, at the middle
		// column leaves out the pixels whose four texels straddle it (one or two columns) and no others.
		const FlickerResult flat = MeasureFlicker(ConstantFrame(1.0f), ConstantFrame(1.0f), ConstantFrame(1.0f));
		CHECK(flat.MeasuredShare == doctest::Approx(static_cast<double>(c_InteriorPixels) / (c_Size * c_Size)));
		const auto step = [](uint32_t x, uint32_t) { return SurfacePoint { x < c_Size / 2 ? 10.0f : 20.0f, glm::vec3(0.0f, 0.0f, 1.0f) }; };
		const auto crease = [](uint32_t x, uint32_t)
		{
			return SurfacePoint { 10.0f, x < c_Size / 2 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::normalize(glm::vec3(0.5f, 0.0f, 0.866f)) };
		};
		for (const auto& surface : { std::function<SurfacePoint(uint32_t, uint32_t)>(step), std::function<SurfacePoint(uint32_t, uint32_t)>(crease) })
		{
			const FlickerResult result = MeasureFlicker(ConstantFrame(1.0f, surface), ConstantFrame(1.0f, surface), ConstantFrame(1.0f, surface));
			const double leftOut = (flat.MeasuredShare - result.MeasuredShare) * (c_Size * c_Size);
			CAPTURE(leftOut);
			CHECK(leftOut >= (c_Size - 4) - 0.5);
			CHECK(leftOut <= 2.0 * (c_Size - 4) + 0.5);
		}
	}

	TEST_CASE("A block of half its pixels measured is left out")
	{
		// The block from (32, 32) alternates; the newest frame draws only `drawn` of its 1,024 pixels. With half of them the
		// block does not count (every other block is steady: Blocks 0), with one more it does (one block of 16 alternates).
		const auto inBlock = [](uint32_t x, uint32_t y) { return x >= 32 && x < 64 && y >= 32 && y < 64; };
		const auto alternating = [&inBlock](float value)
		{
			return MakeFrame(glm::dvec3(0.0, 0.0, 10.0), Plane, [&inBlock, value](const glm::dvec3&, uint32_t x, uint32_t y) { return inBlock(x, y) ? value : 1.0f; });
		};
		const double amplitude = 0.2 / (3.1 / 3.0);
		for (const uint32_t drawn : { 512u, 513u })
		{
			CAPTURE(drawn);
			FlickerFrame newest = alternating(1.0f);
			uint32_t left = 1024 - drawn;
			for (uint32_t y = 32; y < 64 && left > 0; y++)
			{
				for (uint32_t x = 32; x < 64 && left > 0; x++, left--)
					newest.ViewDepth[static_cast<size_t>(y) * c_Size + x] = 0.0f;
			}
			const FlickerResult result = MeasureFlicker(alternating(1.0f), alternating(1.1f), newest);
			if (drawn == 512u)
				CHECK(result.Blocks < 1e-5);
			else
				CHECK(result.Blocks == doctest::Approx(amplitude / 16.0).epsilon(1e-3));
		}
	}

	TEST_CASE("The reprojection follows the camera")
	{
		// A plane textured by its world position, a camera moving 3 pixels a frame: every point keeps its value.
		const auto texture = [](const glm::dvec3& point, uint32_t, uint32_t)
		{
			return static_cast<float>(1.0 + 0.5 * std::sin(point.x * 3.0) * std::cos(point.y * 2.0));
		};
		std::vector<FlickerFrame> frames;
		for (int t = 0; t < 3; t++)
			frames.push_back(MakeFrame(glm::dvec3(3.0 * PixelMeters() * t, 0.0, 10.0), Plane, texture));
		const FlickerResult result = MeasureFlicker(frames[0], frames[1], frames[2]);
		CAPTURE(result.Mean);
		CHECK(result.MeasuredShare > 0.8);
		CHECK(result.Mean < 1e-4);
		// The same frames read as if the camera stood still measure the texture's change instead.
		for (FlickerFrame& frame : frames)
			frame.CameraPosition = glm::dvec3(0.0, 0.0, 10.0);
		CHECK(MeasureFlicker(frames[0], frames[1], frames[2]).Mean > 1e-2);
	}

	TEST_CASE("A sequence keeps three frames")
	{
		std::vector<FlickerFrame> frames;
		for (int t = 0; t < 6; t++)
		{
			frames.push_back(MakeFrame(glm::dvec3(2.5 * PixelMeters() * t, 0.0, 10.0), Plane, [t](const glm::dvec3& point, uint32_t, uint32_t)
			{
				return static_cast<float>(1.0 + 0.3 * std::sin(point.x * 2.0 + t * t * 0.1));
			}));
		}
		FlickerSequence sequence(2);
		for (const FlickerFrame& frame : frames)
			sequence.Add(frame);
		FlickerResult sum;
		for (size_t last = 2; last < frames.size(); last++)
		{
			const FlickerResult triple = MeasureFlicker(frames[last - 2], frames[last - 1], frames[last]);
			sum.Mean += triple.Mean;
			sum.P99 += triple.P99;
			sum.Blocks += triple.Blocks;
			sum.BlocksP99 += triple.BlocksP99;
			sum.MeasuredShare += triple.MeasuredShare;
		}
		const FlickerResult result = sequence.GetResult();
		CHECK(result.Frames == 4u);
		CHECK(result.Mean == sum.Mean / 4.0);
		CHECK(result.P99 == sum.P99 / 4.0);
		CHECK(result.Blocks == sum.Blocks / 4.0);
		CHECK(result.BlocksP99 == sum.BlocksP99 / 4.0);
		CHECK(result.MeasuredShare == sum.MeasuredShare / 4.0);
		CHECK(result.Mean > 0.0);
	}

	TEST_CASE("A display frame decodes sRGB")
	{
		const auto curve = [](double encoded) { return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4); };
		CHECK(GetDisplayLuminance(0, 0, 0) == 0.0f);
		CHECK(std::abs(GetDisplayLuminance(128, 128, 128) - curve(128.0 / 255.0)) < 1e-6);
		CHECK(std::abs(GetDisplayLuminance(255, 255, 255) - 1.0) < 1e-6);
		// Rec. 709's weights of the decoded channels.
		CHECK(std::abs(GetDisplayLuminance(255, 0, 0) - 0.2126) < 1e-6);
		CHECK(std::abs(GetDisplayLuminance(0, 128, 0) - 0.7152 * curve(128.0 / 255.0)) < 1e-6);
	}
}
