#pragma once

#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>

namespace Strata
{

	// What a frame lays the sun's shadow cascades out for (SceneRenderer::FindShadowCasters): the camera, the light and the
	// cascades' settings.
	struct ShadowFitView
	{
		glm::dvec3 CameraPosition = glm::dvec3(0.0);
		// From view space to world space (the inverse view's rotation), in the single precision the slices' spheres have always
		// been computed in.
		glm::mat3 CameraRotation = glm::mat3(1.0f);
		// From world space to light view space (the light view's rotation).
		glm::dmat3 LightRotation = glm::dmat3(1.0);
		float Near = 0.1f;               // The camera's near distance (at least 0.01)
		float ShadowDistance = 80.0f;    // D: where the last cascade ends (beyond Near)
		bool Orthographic = false;
		float TanHalfFOV = 0.0f;         // Perspective cameras: tan(vertical field of view / 2)
		float OrthographicHeight = 0.0f; // Orthographic cameras: the view's height
		float Aspect = 1.0f;
		uint32_t CascadeCount = 4;
		uint32_t MapSize = 2048;         // The cascades' texels each way
		float CascadeBlend = 0.1f;       // DirectionalLightComponent::CascadeBlend
		float Distribution = 0.9f;       // DirectionalLightComponent::CascadeDistribution: a fitted layout's logarithmic share
		// The view distance of the nearest transparent surface the previous frame drew (transparent surfaces are not in the depth
		// the readbacks measure); infinity without one.
		float NearestTransparent = std::numeric_limits<float>::infinity();
		float Seconds = 0.0f; // Since the previous Update, from SceneRenderer::StepSeconds: what the tracker's holds count
		// False: Strata 0.7.0's rules (growths at once, shrinks after c_ShrinkReadbacks readbacks), for renderers of single
		// images (SceneRendererSpecification::ShadowFitHolds).
		bool Holds = true;
	};

	// Scene/ShadowFit.comp's result for a frame, with the camera that frame was drawn from: its view-space boxes are re-projected
	// through that camera into any later camera and light.
	struct ShadowFitReadback
	{
		uint64_t Render = 0;        // The frame it measured
		uint64_t Epoch = 0;         // ShadowFitTracker::GetEpoch when that frame was drawn
		uint64_t LayoutVersion = 0; // ShadowFitTracker::GetLayoutVersion of the layout that frame was drawn with
		glm::dvec3 CameraPosition = glm::dvec3(0.0);
		glm::dmat3 CameraRotation = glm::dmat3(1.0); // View space to world space
		RenderData::ShadowFitResult Result = {};
	};

	// A cascade of a layout: its slice of view distances (the near end includes the band in which the previous cascade hands
	// over), the bounding sphere its square covers unless the square is fitted to the receivers, and the fitted square.
	struct ShadowFitCascade
	{
		float SliceNear = 0.0f;
		float SliceFar = 0.0f;                    // Its split
		glm::vec3 SphereCenter = glm::vec3(0.0f); // Relative to the camera, along the world's axes
		float Radius = 0.0f;                      // Rounded up to 1/16
		bool Fitted = false;                      // The square below; else the sphere's
		float Side = 0.0f;                        // The fitted square's side (world units)
		glm::dvec2 Center = glm::dvec2(0.0);      // Its center in absolute light view space, on whole texels of the square
	};

	struct ShadowFitLayout
	{
		bool Fitted = false;        // Laid out over a readback's receivers; else Strata 0.6.0's layout
		bool SquaresFitted = false; // Some cascade's square is fitted
		float Near = 0.0f;          // The range the slices divide: the first one starts at Near; Far is where the other cascades end
		float Far = 0.0f;           // when they do not reach the shadow distance (the last cascade always does)
		uint32_t CascadeCount = 0;
		std::array<ShadowFitCascade, RenderData::c_MaxCascades> Cascades = {};
		// A slice, a sphere or a square's side differs from the previous frame's layout (a center moving texel by texel with the
		// camera does not count).
		bool Changed = false;
	};

	// The sun's cascades fitted to what the camera sees (Docs/DeveloperGuide.md, "Sun shadows"): each frame's prepass depth is
	// reduced on the GPU (Scene/ShadowFit.comp) to the receivers' nearest and farthest view distance and, per quarter stop of
	// distance, a view-space box of them, read back frames later. From the newest readback the tracker lays out the slices
	// over [N, F] (the receivers' range with margins, in quarter stops) and fits every cascade's square but the last one's to
	// the receivers re-projected through the readback's camera into this frame's camera and light; the last cascade keeps its
	// slice's sphere and reaches the shadow distance, so a receiver the readback did not see still has a cascade (shaders select
	// cascades by containment: Include/Shadows.glsl, SelectShadowCascade). The far bound grows at once; the near bound grows at
	// once for receivers holding c_NearNowShare of the pixels, for a transparent surface and in narrow views, else only after
	// asking for c_GrowSeconds (c_GrowSecondsMoving on a moving camera); squares grow at once on a moving camera and after
	// c_GrowSeconds on a still one, keeping the previous square meanwhile; everything shrinks after c_ShrinkReadbacks readbacks
	// and c_ShrinkSeconds. Without holds (ShadowFitView::Holds, renderers of single images) growths are made at once and
	// shrinks after c_ShrinkReadbacks readbacks, as in Strata 0.7.0. Until the first readback of an epoch (Reset: a camera cut,
	// another scene, other cascades) and without receivers, the layout is Strata 0.6.0's. Pure C++: SceneRenderer feeds it.
	class ShadowFitTracker
	{
	public:
		// Readbacks in a row that must ask for less before a bound or side shrinks.
		static constexpr uint32_t c_ShrinkReadbacks = 8;
		// The logarithmic share of Strata 0.6.0's layout (unfitted frames).
		static constexpr float c_UnfittedDistribution = 0.8f;
		// A fitted far bound from this share of the shadow distance on is the shadow distance.
		static constexpr float c_FarToDistance = 0.7f;
		// The nearest receivers are left out of the fit while together they hold less than this share of the receiver pixels (a
		// leaf or a tree top at a flying camera would spend the first cascades on distances almost nothing occupies); containment
		// shades them from the last cascade, whose sphere then holds every point of the view nearer than the shadow distance.
		// Where it would not (fields of view so narrow that the near bound they give lies beyond D (tan^2(fovY / 2) +
		// tan^2(fovX / 2)), and orthographic views) they are kept.
		static constexpr double c_NearOutlierShare = 0.01;
		// Receivers nearer than the near bound holding at least this share of the receiver pixels move it at once (a wall, a
		// dive); fewer must keep asking for c_GrowSeconds (c_GrowSecondsMoving while the camera moves), so a ball passing the
		// camera re-cuts no slice. 6.85% for the Physics Playground's ball at spawn; 15.6% for its dive (shadow-jump audit).
		static constexpr double c_NearNowShare = 0.10;
		// How long a growth must keep asking before it is made: the near bound's and a square's on a still camera (a square grows
		// at once on a moving one), the near bound's on a moving camera. ShadowFitView::Seconds, never frames or readbacks.
		static constexpr float c_GrowSeconds = 0.5f;
		static constexpr float c_GrowSecondsMoving = 0.12f;
		// A bound or side shrinks after c_ShrinkReadbacks readbacks in a row asked for less and at least this long.
		static constexpr float c_ShrinkSeconds = 0.5f;
		// A fitted square is the receivers' box grown by this factor, and by the filter's reach on each side in its own texels
		// (the shaders' containment margin).
		static constexpr double c_SquareGrowth = 1.05;
		static constexpr uint32_t c_FilterReachTexels = 33;
		// Sides and bounds as quarter stops (2^(q / 4)); these stand for the slice's sphere and for the shadow distance.
		static constexpr int32_t c_SphereQuarters = std::numeric_limits<int32_t>::max();
		static constexpr int32_t c_DistanceQuarters = std::numeric_limits<int32_t>::max();

		// Starts a new epoch: readbacks of earlier frames are ignored, and the layout is Strata 0.6.0's until one of the new epoch
		// arrives, whose receivers then set the layout at once.
		void Reset();
		uint64_t GetEpoch() const { return m_Epoch; }
		// Changes whenever the layout does (and with Reset): what a readback records, so it can confirm the layout.
		uint64_t GetLayoutVersion() const { return m_LayoutVersion; }
		// Lays out a frame's cascades. `readback`: one of this epoch that arrived since the previous call (null: none; others are
		// ignored). The result lives until the next call.
		const ShadowFitLayout& Update(const ShadowFitView& view, const ShadowFitReadback* readback);
		const ShadowFitLayout& GetLayout() const { return m_Layout; }
		// No readback is due that may change the layout: since the latest change or Reset, a readback of a frame drawn with the
		// current layout arrived and left it unchanged.
		bool IsSettled() const { return m_Settled; }
		// A growth of the near bound or of a square waits (never without ShadowFitView::Holds); the layout is not settled meanwhile.
		bool IsHolding() const { return m_Holding; }
		// Frames whose layout changed since the tracker was made.
		uint64_t GetChanges() const { return m_Changes; }

		// Strata 0.6.0's layout: c_UnfittedDistribution of logarithmic splits from the near distance to the shadow distance,
		// every square its slice's sphere.
		static ShadowFitLayout GetUnfittedLayout(const ShadowFitView& view);
		// The split (far end) of slice `index` of `count` over [rangeNear, rangeFar] with a logarithmic share `distribution`.
		static float GetSplit(float rangeNear, float rangeFar, float distribution, uint32_t index, uint32_t count);
		// Quarter stops: floor and ceil of 4 log2(value), and back.
		static int32_t QuartersBelow(double value);
		static int32_t QuartersAbove(double value);
		static double FromQuarters(int32_t quarters);
	private:
		struct Hysteresis
		{
			uint32_t Count = 0;   // Readbacks in a row that asked for less
			int32_t Wanted = 0;   // Shrinking: the least shrunk of what they asked for; growing: the least grown
			float Seconds = 0.0f; // How long it has asked
			bool Growing = false; // It asks for more, and waits
		};
		struct ReprojectedBin
		{
			glm::dvec2 Min = glm::dvec2(0.0); // Light view space, relative to the camera
			glm::dvec2 Max = glm::dvec2(0.0);
			double Nearest = 0.0;             // View distances along this frame's view direction
			double Farthest = 0.0;
		};

		// The readback's nearest receiver distance once the outliers nearer than it are left out (c_NearOutlierShare, where the
		// view's last cascade still holds them); 0 without receivers.
		double GetFittedNearest(const ShadowFitView& view) const;
		// The fitted layout's bounds from the readback (false: no receivers within the shadow distance).
		bool GetBoundTargets(const ShadowFitView& view, int32_t& nearQuarters, int32_t& farQuarters) const;
		void Reproject(const ShadowFitView& view);
		// The square cascade `cascade` of `layout` wants, as quarter stops of its side (c_SphereQuarters: its sphere), and the box of
		// the receivers it must hold (with the turn's margin) relative to the camera in light view space.
		int32_t GetSideTarget(const ShadowFitView& view, const ShadowFitLayout& layout, uint32_t cascade, glm::dvec2& boxMin, glm::dvec2& boxMax) const;
		// Grows at once when `growNow`, else once it kept asking for `growSeconds`, to the least grown of what it asked for;
		// shrinks after c_ShrinkReadbacks readbacks in a row and `shrinkSeconds` (c_ShrinkSeconds; 0 without holds), to the
		// least shrunk; `larger` orders the targets. `seconds`: the frame's ShadowFitView::Seconds.
		static void Follow(int32_t target, int32_t& current, Hysteresis& state, bool newReadback, bool larger, float seconds, bool growNow,
			float growSeconds, float shrinkSeconds);
		// The share of the readback's receiver pixels nearer than `nearBound` (whole bins: a bin straddling it counts).
		double GetNearerShare(double nearBound) const;

		uint64_t m_Epoch = 1;
		uint64_t m_LayoutVersion = 1;
		uint64_t m_Changes = 0;
		bool m_Settled = false;
		bool m_HasReadback = false;
		ShadowFitReadback m_Readback;
		std::array<ReprojectedBin, RenderData::c_ShadowFitBins> m_Bins = {};
		uint32_t m_BinCount = 0;
		bool m_Fitted = false; // The current layout came from a readback
		int32_t m_NearQuarters = 0;
		int32_t m_FarQuarters = c_DistanceQuarters;
		Hysteresis m_NearState;
		Hysteresis m_FarState;
		std::array<int32_t, RenderData::c_MaxCascades> m_SideQuarters = {};
		std::array<Hysteresis, RenderData::c_MaxCascades> m_SideStates = {};
		ShadowFitLayout m_Layout;
		bool m_Holding = false;
		// The previous Update's camera and light: a frame whose three equal them is still (TAA's jitter is in the projection).
		bool m_HasLastView = false;
		glm::dvec3 m_LastCameraPosition = glm::dvec3(0.0);
		glm::mat3 m_LastCameraRotation = glm::mat3(1.0f);
		glm::dmat3 m_LastLightRotation = glm::dmat3(1.0);
	};

}
