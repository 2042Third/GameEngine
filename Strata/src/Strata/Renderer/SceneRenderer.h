#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/ErrorThrottle.h"
#include "Strata/Core/RadixSort.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Renderer/AssetResolveCache.h"
#include "Strata/Renderer/BufferReadback.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/SceneOverlay.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/ShadowAtlasAllocator.h"
#include "Strata/Renderer/TextureReadback.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata
{

	class FontAtlasCache;
	class Material;
	class Mesh;
	class RenderScene;
	class RenderSceneGPU;
	class Scene;
	class TextRenderer;
	class Texture;
	struct HeightFogComponent;
	struct PostProcessComponent;
	struct SkyLightComponent;
	struct Submesh;
	struct TextureHold;

	// The view a scene is rendered from.
	struct SceneCamera
	{
		glm::mat4 View = glm::mat4(1.0f);
		glm::mat4 Projection = glm::mat4(1.0f); // Strata clip space: reversed-Z, depth [0, 1]
		glm::vec3 Position = glm::vec3(0.0f);
		float Near = 0.1f;
		float Far = 1000.0f;
		// Vertical field of view in radians (perspective) or vertical extent in world units (orthographic); used
		// for level-of-detail selection.
		float VerticalFOV = glm::radians(60.0f);
		bool Orthographic = false;
		// Background where nothing is drawn and no sky is shown, as an sRGB-encoded color. It becomes part of the HDR
		// image (linearized), so exposure, tone mapping and grading apply to it like to the rest of the scene: it shows
		// unchanged only with neutral post-processing (manual exposure 0, no tone curve, no vignette).
		glm::vec4 ClearColor = { 0.05f, 0.05f, 0.07f, 1.0f };

		// Camera of a scene entity with a CameraComponent (view from its world transform).
		static SceneCamera FromEntity(const Scene& scene, Entity entity, float aspectRatio);
	};

	// How a renderer issues its draws (SceneRendererSpecification::IndirectMode).
	enum class IndirectDrawMode : uint8_t
	{
		Auto = 0, // What the device allows
		NoCount,  // Indirect draws without a draw count the GPU writes
		PerDraw   // A draw call per batch (the GPU-driven path: an indirect draw per draw slot)
	};

	// Whether the GPU culls and builds the draws (SceneRendererSpecification::GPUDriven).
	enum class GPUDrivenMode : uint8_t
	{
		Auto = 0, // Where the device can (indirect draws that set their first instance)
		Off       // The CPU path: culling, batching and draw lists on the CPU
	};

	// What a view chooses levels of detail by (SceneRenderer::GetLODCriteria, LODSelection::SelectLOD).
	struct LODCriteria
	{
		float LODThreshold = 0.5f;   // SceneRendererSpecification::LODThreshold: submeshes whose levels have no errors halve by it
		float ErrorThreshold = 0.0f; // LODSelection::GetErrorThreshold: the largest screen fraction times relative error drawn
		float ViewportHeight = 0.0f; // In pixels (LODSelection::GetErrorPixels)
	};

	struct SceneRendererSpecification
	{
		std::string DebugName = "SceneRenderer";
		// Levels of detail with errors (MeshLOD::Error, which generated levels have): the most simplified level whose error,
		// projected onto the screen, spans at most this many pixels of the viewport height is drawn. 0 draws full detail.
		float LODErrorPixels = 1.0f;
		// Levels without errors (meshes cooked before they had them, levels made by other means): screen fraction (projected
		// bounding sphere diameter / viewport height) below which each further level is used: LOD n is chosen while the
		// fraction is below LODThreshold / 2^(n-1).
		float LODThreshold = 0.5f;
		// A mesh renderer whose level of detail changes draws both levels for RenderData::c_LODFadeFrames frames, the new one
		// on more pixels each frame (complementary dither masks, LODSelection::UpdateLODFade), instead of popping. Camera cuts
		// switch at once. Renderers of single images (the editor's captures, thumbnails) turn it off.
		bool FadeLevelsOfDetail = true;
		// Point and spot lights rendered per frame (after frustum culling). When more are visible, the most relevant ones
		// are kept: lights whose range contains the camera, then by the projected size of their range.
		uint32_t MaxLights = 4096;
		// Clustered light assignment: the view is split into LightClusterGrid.x by .y screen tiles and .z depth slices
		// (exponentially spaced). Each cluster lists up to MaxLightsPerCluster lights, the most relevant ones when more
		// reach it. A 1x1x1 grid shades every pixel with every visible light.
		glm::uvec3 LightClusterGrid = { 16, 9, 24 };
		uint32_t MaxLightsPerCluster = 256;
		// The GPU-driven path (Auto, where the device has drawIndirectFirstInstance): compute shaders cull the proxies of the
		// render scene's GPU copy against the view and the shadow cascades, choose their levels of detail and build the draws
		// (Scene/Cull.comp, CullBuild.comp); the CPU draws a fixed number of indirect draws per pass and bucket, and only
		// transparent surfaces itself (sorted back to front). Off: the CPU path (culling, batching and draw lists on the CPU),
		// the reference the GPU path's pixels are compared with.
		GPUDrivenMode GPUDriven = GPUDrivenMode::Auto;
		// How batches are drawn. CPU path: Auto draws each bucket of batches (one pipeline state and vertex arena) with one
		// indirect call where the device has multiDrawIndirect and drawIndirectFirstInstance, else with a direct draw per
		// batch; PerDraw always draws per batch. GPU path: Auto draws each bucket with one drawIndexedIndirectCount where the
		// device has drawIndirectCount, NoCount with one drawIndexedIndirect over the bucket's draw slots (those the GPU did not
		// fill have no instances), PerDraw with one drawIndexedIndirect per slot (also without multiDrawIndirect). Tests force
		// each to compare them.
		IndirectDrawMode IndirectMode = IndirectDrawMode::Auto;
		// The most draws one indirect call issues; 0 (or more) means the device's limit (GraphicsDeviceInfo::
		// MaxDrawIndirectCount), which always bounds it. A bucket with more draws is drawn by several calls, and a GPU-driven
		// frame whose buckets could hold more draws than the limit draws without a GPU-written count (NoCount, split alike).
		// Tests set it low to check the splits.
		uint32_t MaxDrawsPerIndirectCall = 0;
		// Mesh renderers whose projected size (bounding sphere diameter) is below this many pixels of the viewport height are
		// not drawn in the view, by either path: they add moire and GPU time, not information. 0 draws them all. Shadow
		// cascades leave out casters below a texel (SceneRendererStats::SubTexelCasters).
		float MinProjectedPixels = 0.5f;
		uint32_t ShadowMapSize = 2048; // Resolution of each directional shadow cascade
		uint32_t ShadowCascades = 4;   // 1-4
		// The depth prepass writes the entity-ID buffer (a 32-bit render target of the viewport's size): picking (GetEntityAt,
		// ReadEntityIDAsync), the selection outline and entity tints read it. Renderers that do none of these (games) turn it
		// off; outlines and tints asked of them are then left out, with an error.
		bool EntityIDs = true;

		// Asset requests follow visibility: mesh renderers in the view request their mesh and materials at High priority
		// when their projected size (bounding sphere diameter / viewport height) exceeds this fraction, at Normal priority
		// otherwise; nothing else is requested for drawing.
		float HighPriorityScreenFraction = 0.05f;
		// Mesh renderers outside the view within this distance of the camera request their mesh and materials at Low
		// priority (prefetch), so turning the camera finds them loaded. 0 turns prefetching off. Shadow casters outside
		// the view and this distance are drawn into the shadow cascades only while their assets are resident.
		float PrefetchDistance = 100.0f;
		// Instances of a mesh share draw calls whatever their materials (shaders read the material of each instance):
		// batches are split only by pipeline state, mesh, submesh and level of detail. Off, the CPU path's batches are also
		// split by material, as renderers without per-instance materials draw; for comparisons and debugging (the GPU-driven
		// path always draws across materials).
		bool BatchAcrossMaterials = true;
		// Textures stream at the mip levels the screen needs (TextureStreamer): the renderer reports what it drew after each
		// frame (VisibleMaterialUse), and a texture whose mip tail is resident is drawn with what it has. Off, every texture
		// it draws is made resident at full resolution and counts as pending until it is (SceneRendererStats::PendingAssets):
		// exact-pixel tests and the baseline streaming is measured against.
		bool TextureStreaming = true;
		// Texture levels the streamer raises fade in over TextureTable::c_FadeInSeconds instead of popping in. Renderers of
		// single images that wait for the levels they need (the editor's captures, thumbnails) turn it off: they show the
		// finest resident levels at once.
		bool FadeTextureLevels = true;
		// The render scene's GPU copy (RenderSceneGPU, shared by the scene's renderers) uploads new proxies' records and
		// instances within this many bytes per frame, at least one proxy per frame; the others wait, undrawn
		// (SceneRendererStats::DeferredInstanceBytes). Changes of proxies already on the GPU always upload at once.
		uint64_t NewInstanceBytesPerFrame = 16ull << 20;
		// GPU-driven frames take their asset requests (and the screen sizes texture streaming follows) from the GPU's
		// visibility feedback of a frame two or three frames earlier instead of walking the view's proxies on the CPU: what
		// each mesh and material is used for, in the view (above HighPriorityScreenFraction or not), within PrefetchDistance
		// or casting shadows. The CPU's marks still run on a renderer's first frames, after camera cuts
		// (TextureStreamingPolicy::IsCameraCut), and after a frame whose draws overflowed the slots the feedback foresaw;
		// meshes of unknown bounds, transparent surfaces and skinned proxies are always the CPU's. Off: the CPU's marks every
		// frame.
		bool VisibilityFeedback = true;
		// GPU-driven frames leave out of the view what lies behind what the view draws, in two phases: the first draws what
		// was visible in the previous frame into the depth prepass, a depth pyramid of that (Scene/HZBBuild.comp) tests every
		// other proxy in the frustum, and the second phase draws those found visible (in the same frame: nothing appears a
		// frame late). Camera cuts and a renderer's first frame start without history (the first phase draws nothing).
		// The shadow cascades are culled alike, each against a pyramid of its own first phase's shadow map (a quarter of
		// ShadowMapSize each way): casters hidden from the light behind other casters are left out. That costs GPU time of
		// its own, so it runs while it pays: while the latest frame that culled the cascades found at least a sixteenth of
		// their casters hidden, otherwise once every 16 frames to look again (SceneRendererStats::ShadowOcclusionCulled).
		bool OcclusionCulling = true;
		// GPU-driven frames redraw a shadow cascade only when something that reaches it changed since the frame that drew it:
		// the camera (any part of SceneCamera or the viewport), the light or the cascade's square, a caster whose bounds before or
		// after its change reach the cascade (a caster's first change, the frames before which it was not followed, redraws
		// every cascade), or what the casters are drawn with (the scene's GPU copy's mesh and material tables, the geometry pool's
		// tables and, while materials are alpha-masked, the texture table's entries). Other cascades keep their shadow map
		// layer (SceneRendererStats::CachedCascadeMask): a still camera over a still or local change costs no shadow draws.
		bool CacheShadowCascades = true;
		// Scenes whose PostProcessComponent chooses AntiAliasingMode::TAA are drawn with temporal anti-aliasing: each frame
		// offsets its samples by a sub-pixel jitter (8 phases of a Halton (2, 3) sequence, from the history's latest reset on,
		// in the frame's copy of the camera only: culling, levels of detail, the shadow cascades and the overlays keep the
		// camera's own matrices) and blends with the renderer's history of earlier frames. The history is the renderer's own and
		// starts again (SceneRendererStats::HistoryResets) on its first frame, after a camera cut, with a render scene that is
		// neither the previous one nor a clone of it, after a resize, after a device frame (Renderer::GetFrameNumber) in which it
		// did not render, and after ResetHistory. Off: such scenes are drawn with FXAA (renderers of single images that do not
		// render a view several times, such as thumbnails).
		bool TemporalAntiAliasing = true;

		// Point and spot lights with CastShadows: the renderer's depth atlas of this many texels each way (a power of two from
		// 512 to 8192; made when the first such light is drawn, released with the render targets: 64 MiB at 4096). The
		// MaxShadowedLocalLights most relevant of them in view (at most RenderData::c_MaxShadowedLocalLights; ranked as the
		// light clusters rank lights) get tiles of it, a spot light one, a point light six (the faces of a cube), of 128 to
		// 1024 texels following how large the light's range looks on screen; when they do not fit, the less relevant lights'
		// tiles get smaller first (SceneRendererStats::LocalShadowLights).
		uint32_t LocalShadowAtlasSize = 4096;
		uint32_t MaxShadowedLocalLights = RenderData::c_MaxShadowedLocalLights;
		// A shadow view (a light's tile) is drawn again only when what it shows changed: the light (its position, direction,
		// range, cone, tile or filter), a shadow caster whose bounds before or after its change reach the light's range and
		// view, or what the casters are drawn with (the scene's GPU copy's mesh and material tables and new proxies, the
		// geometry pool's content and, while materials are alpha-masked, the texture table's entries). Off: every view of
		// every shadowed light is drawn every frame (perf tests measure that), and nothing fades (SpreadLightingUpdates).
		bool CacheLocalShadows = true;
		// Lighting work is spread over frames, for renderers that draw views continuously: at most LocalShadowViewsPerFrame
		// shadow views are drawn per frame, the nearest lights' first; a light whose views were not all drawn yet casts no
		// shadow, then fades its shadow in over c_LocalShadowFadeFrames frames, and the lights ranked just above the ones left
		// out (MaxShadowedLocalLights) fade by their margin over them, so shadows do not pop. Off (renderers of single images:
		// captures, thumbnails): each frame draws every view that is not up to date, and nothing fades
		// (SceneRendererStats::LocalShadowViewsPending is 0 after every frame). Reflection probes likewise: at most
		// SceneRenderer::c_ReflectionProbeConversionsPerFrame captures are converted per frame, else every one in view at once
		// (SceneRendererStats::ReflectionProbesPending then counts only captures still loading).
		bool SpreadLightingUpdates = true;
		uint32_t LocalShadowViewsPerFrame = 12;
		// Reflection probes (ReflectionProbeComponent): the renderer keeps the strongest this many in view (at most
		// RenderData::c_MaxReflectionProbes; by priority, then the smaller box), each capture converted into a slot of its cube
		// array (made with the first probe in view, released with the render targets or once the scene has none: about 1 MiB a
		// slot). 0: no probes.
		uint32_t MaxReflectionProbes = RenderData::c_MaxReflectionProbes;
	};

	// A material the last frame drew, for streaming its textures at the resolution the screen needs.
	struct VisibleMaterialUse
	{
		AssetHandle Material = UUID::Null();
		float PixelSize = 0.0f;  // Largest projected size (bounding sphere diameter, in pixels of viewport height) drawn with it
		float MaxUVScale = 1.0f; // Its largest texture coordinate tiling (MaterialProperties::UVTiling)
		// The most pixels one unit of the meshes' texture coordinates spans where it is drawn, at the surfaces' points nearest
		// the camera (TextureStreamingPolicy::GetWorldPixels, GetUVPixels): what its textures' mip levels follow. 0 for a
		// material without a loaded texture map, for which nothing streams and nothing is computed.
		float UVPixels = 0.0f;
	};

	// A loaded texture map of a material the last frame drew, as the frame resolved it, with that material's use
	// (VisibleMaterialUse): what the texture streamer needs of it (TextureStreamer::ReportView), so streaming looks nothing
	// up per material or texture.
	struct VisibleTextureUse
	{
		AssetHandle Texture = UUID::Null();
		uint32_t Size = 0;      // Its larger side at level 0, in texels
		uint32_t MipCount = 0;
		float PixelSize = 0.0f; // VisibleMaterialUse::PixelSize of its material
		float UVScale = 1.0f;   // VisibleMaterialUse::MaxUVScale
		float UVPixels = 0.0f;  // VisibleMaterialUse::UVPixels
	};

	// An entity tinted over the finished image (SceneRenderOptions::EntityTints).
	struct SceneEntityTint
	{
		Entity Target;
		glm::vec4 Color = glm::vec4(0.0f); // Display color (sRGB-encoded); alpha is the tint's opacity
	};

	// How a scene is shown, beyond its own data: mainly editor views. With the defaults a scene renders exactly as a game
	// shows it.
	struct SceneRenderOptions
	{
		// Editor preview lighting: a scene without any active light (directional, point, spot or sky light;
		// SceneRenderer::NeedsPreviewLighting) is lit by a preview sun (a default directional light shining from
		// SceneRenderer::GetPreviewSunDirection) and a procedural sky with the default SkyLightComponent colors, so unlit
		// content shows its shape. Scenes with any lighting of their own, also a dark one lit by a torch, render unchanged.
		// Never part of the scene (not saved, not in games).
		bool PreviewEnvironment = false;
		// The game's screen-space UI: the HUD text and screen-space canvases (the overlay passes, SceneOverlayRegistry). Editor
		// views turn it off so the HUD does not cover the scene being edited; world-space text and canvases are always drawn.
		bool DrawScreenSpaceText = true;
		// Where the game's screen lies in the image, for that screen-space UI: by default the whole image. The editor's 2D
		// canvas mode zooms and pans it (SceneScreenView).
		SceneScreenView ScreenView;
		// The scene whose overlay passes (the game UI) are drawn, when not the rendered scene's: GameRenderer shows a scene
		// without a camera as its UI over an empty frame. Must stay valid during Render.
		Scene* OverlayScene = nullptr;
		// Motion vectors: the frame writes each opaque or alpha-masked surface's motion since the renderer's previous frame into
		// the velocity target (SceneRenderer::GetVelocityTexture): its unjittered position in this frame's UV space minus its
		// position in the previous frame's (moved entities, skinned poses and the camera alike; nothing is written where nothing
		// is drawn). The renderer keeps its own copy of the render scene's instances and joint palettes as its latest frame drew
		// them (its motion history: GetPreviousInstanceBuffer, GetPreviousPaletteBuffer), made by the first frame that asks and
		// kept with the velocity target until the render targets are released: the size of the scene's instance and palette
		// buffers (64 bytes per instance slot, 48 per joint matrix). At the end of each such frame it copies what the scene's GPU
		// copy wrote since its previous frame (RenderSceneGPU::GetInstanceWrites), so a still scene costs no copy. Its previous
		// frame is the renderer's latest, whenever that was rendered (renderers of one scene keep their own); a frame after one
		// that did not ask, or of another render scene than the latest (but a clone of it), has none
		// (SceneRendererStats::MotionHistory) and no motion. An instance slot another entity held in the previous frame, and a new
		// instance, move with the camera only. The image is the same with and without motion vectors.
		bool MotionVectors = false;
		// Forces the frame's pre-exposure (SceneRendererStats::PreExposure): a power of two (another value is rounded to the
		// nearest one in stops; zero, negative or non-finite values to 1). Tests, bakes and renderers whose images are compared
		// with each other's set it, so their HDR values match without waiting for each one's metering.
		std::optional<float> PreExposure;

		// The overlays below are drawn over the finished image (after tone mapping and anti-aliasing) with exact display
		// colors; all of them are off by default.

		// Infinite ground grid on the y = 0 plane, hidden behind scene geometry: minor lines every GridSpacing world units,
		// a major line every GridMajorEvery lines, the X axis in GridAxisXColor and the Z axis in GridAxisZColor. It fades out
		// between half the fade distance and the fade distance from the camera.
		bool ShowGrid = false;
		float GridSpacing = 1.0f;
		uint32_t GridMajorEvery = 10;
		float GridFadeDistance = 100.0f;
		glm::vec4 GridMinorColor = { 0.5f, 0.5f, 0.5f, 0.35f };
		glm::vec4 GridMajorColor = { 0.6f, 0.6f, 0.6f, 0.6f };
		glm::vec4 GridAxisXColor = { 0.9f, 0.2f, 0.2f, 1.0f };
		glm::vec4 GridAxisZColor = { 0.2f, 0.35f, 0.95f, 1.0f };

		// Entities outlined as selected: OutlineWidth pixels around their visible silhouette. The outline follows the
		// entity-ID buffer, so it covers opaque and alpha-masked surfaces (not alpha-blended ones). Entities of other scenes
		// are ignored. The span must stay valid during Render.
		std::span<const Entity> SelectedEntities;
		glm::vec4 SelectionColor = { 1.0f, 0.55f, 0.1f, 1.0f };
		uint32_t OutlineWidth = 2; // 1-8 pixels

		// Debug lines to draw this frame (null for none); see DebugDraw.
		const DebugDraw* DebugShapes = nullptr;

		// Entities tinted first, under the other overlays (debug views such as the editor's streaming and LOD views): every
		// pixel whose entity-ID buffer entry is a listed entity is blended with its color, every other pixel stays exactly as
		// it was. Like the outline it follows the entity-ID buffer (opaque and alpha-masked surfaces). Entities of other
		// scenes and invalid ones are ignored; an entity listed twice keeps its first color. The span must stay valid during
		// Render.
		std::span<const SceneEntityTint> EntityTints;

		// The motion debug view, drawn first (Overlay/MotionView.frag): every pixel's motion since the renderer's previous frame
		// (MotionVectors, which it implies) as a color over the image, its hue the direction on screen (right red, up
		// yellow-green, left cyan, down violet) and its strength the speed, full at 16 pixels a frame; still pixels show the
		// image darkened. Where nothing was drawn (the sky) the motion is the camera's.
		bool ShowMotionVectors = false;
	};

	struct SceneRendererStats
	{
		static constexpr size_t c_LODStatCount = 8;

		bool Rendered = false; // False when the last Render call drew nothing (see Render)
		// The GPU culled and drew the opaque and alpha-masked instances and the shadows (SceneRendererSpecification::GPUDriven):
		// their counts arrive frames later in SceneRendererGPUStats, and Instances, Triangles, DrawCalls, OpaqueDrawCalls,
		// InstancesPerLOD, SkinnedInstances, ShadowCasters and ShadowDrawCalls count only what the CPU drew (transparent
		// surfaces). DrawCommands counts every draw call the CPU recorded.
		bool GPUDriven = false;
		// The frame's requests came from the GPU's visibility feedback (SceneRendererSpecification::VisibilityFeedback); its
		// VisibleProxies and CulledInstances then count only the proxies the CPU still looked at.
		bool FeedbackRequests = false;
		// The frame's cascades were occlusion culled (SceneRendererSpecification::OcclusionCulling, while it pays).
		bool ShadowOcclusionCulled = false;
		uint32_t DrawCalls = 0;       // Draw calls of the forward passes (opaque and transparent)
		uint32_t OpaqueDrawCalls = 0; // Of those, opaque batches: also the draw calls of the depth prepass
		uint32_t ShadowDrawCalls = 0; // Draw calls into the shadow cascades
		uint32_t DrawCommands = 0;    // Draw commands the CPU recorded for the scene's meshes in every pass (shadows, prepass, forward)
		uint32_t Instances = 0;       // Drawn submesh instances
		uint32_t CulledInstances = 0; // Mesh renderers outside the view frustum
		// CPU path: mesh renderers in the view smaller than SceneRendererSpecification::MinProjectedPixels (not drawn).
		uint32_t SizeCulledProxies = 0;
		// GPU-driven frames: mesh renderers in the frustum hidden behind what the view draws (OcclusionCulling; with
		// AddGPUCounts).
		uint32_t OccludedProxies = 0;
		// GPU-driven frames: casters hidden in a cascade behind what it draws, summed over cascades (OcclusionCulling; with
		// AddGPUCounts).
		uint32_t OccludedCasters = 0;
		uint64_t Triangles = 0;
		// Drawn submesh instances per selected level of detail; the last entry also counts all higher levels.
		std::array<uint32_t, c_LODStatCount> InstancesPerLOD = {};
		uint32_t Lights = 0;          // Rendered lights: the directional light plus the point and spot lights (the GPU-driven path's
		                              // point and spot lights are counted on the GPU: SceneRendererGPUStats::Lights, AddGPUCounts)
		uint32_t CulledLights = 0;    // Point and spot lights outside the view frustum
		uint32_t DroppedLights = 0;   // Visible point and spot lights beyond SceneRendererSpecification::MaxLights
		uint32_t Materials = 0;       // Distinct materials drawn
		// Of the materials drawn and casting shadows, those whose flags and texture maps the frame took over from an earlier
		// frame's (the same materials and textures used the same ways: still frames), with their assets looked up in one call
		// to the asset manager (AssetResolveCache::Replay) instead of one by one.
		uint32_t ReusedMaterials = 0;
		uint32_t PendingAssets = 0;   // Distinct meshes, materials and textures still loading (drawn with fallbacks or skipped)

		// The render scene (RenderScene) and what the frame cost.
		uint32_t VisibleProxies = 0;         // Mesh renderers whose bounds intersect the view frustum
		uint32_t ProxyUpdates = 0;           // Render proxies updated since this renderer's previous frame (changed components, transforms, mesh layouts)
		uint64_t InstanceBytesUploaded = 0;  // Instance data and draw lists uploaded this frame (nothing when nothing changed)
		uint32_t InstanceUploadRanges = 0;   // Separate ranges of instance data uploaded
		uint64_t ProxyBytesUploaded = 0;     // Culling records of the render scene's GPU copy uploaded this frame (RenderData::GPUProxy)
		// New proxies whose records and instances wait for a later frame's upload (NewInstanceBytesPerFrame): not drawn yet.
		uint64_t DeferredInstanceBytes = 0;
		uint32_t DeferredProxies = 0;
		// Material data the render scene's GPU copy uploaded in this frame's update (materials built again: loaded, edited,
		// dropped, or a texture of theirs was).
		uint64_t MaterialBytesUploaded = 0;
		float ExtractMs = 0.0f;              // CPU time of culling, asset resolution and gathering draws (view and shadows)
		float SortMs = 0.0f;                 // CPU time of sorting the draws into batches and draw lists
		uint32_t RequestsByVisibility = 0;   // Distinct meshes, materials and textures requested for drawing
		uint32_t PrefetchRequests = 0;       // Distinct meshes and materials requested at Low priority within the prefetch distance
		uint32_t GetAssetCalls = 0;          // Asset manager lookups: at most one per distinct asset
		// Meshes requested as if in view because their bounds are not known yet (RenderScene::GetMeshesWithoutBounds:
		// never loaded, none recorded), counted in RequestsByVisibility too.
		uint32_t MeshesWithoutBounds = 0;
		bool EnvironmentLighting = false; // A sky light's environment (map or procedural sky) lights the scene
		// Environment cubes computed this frame (with their irradiance and prefiltered maps): 1 when the environment map
		// or the procedural sky's parameters (colors, sun size and intensity, the sun's direction and color) changed. An
		// environment map's environment that another renderer computed is taken over, not computed (0).
		uint32_t EnvironmentUpdates = 0;
		// The atmosphere (SkyLightSource::Atmosphere): whether the frame's sky is it; its tables computed this frame (1 when
		// its settings or the sun's color changed: transmittance and multiple scattering) and its sky-view tables (when the
		// sun moved, the camera changed height band or the settings changed); the steps of an environment refresh recorded
		// this frame (SceneRenderer::c_AtmosphereRefreshSteps make one: the cube's six faces, its levels, the irradiance and
		// each prefiltered level; at most 1 a frame with SceneRendererSpecification::SpreadLightingUpdates), and whether one
		// is still under way. The directional light's color is multiplied by SunTransmittance, the share of its light that
		// reaches the ground through the atmosphere (1 without one).
		bool Atmosphere = false;
		uint32_t AtmosphereTableUpdates = 0;
		uint32_t AtmosphereSkyViewUpdates = 0;
		uint32_t EnvironmentRefreshSteps = 0;
		bool EnvironmentRefreshPending = false;
		glm::vec3 SunTransmittance = glm::vec3(1.0f);
		bool PreviewLighting = false;     // SceneRenderOptions::PreviewEnvironment lit the scene (it has no light of its own)
		bool HeightFog = false;           // The scene's height fog (SceneRenderer::FindHeightFog) was drawn
		uint32_t ShadowCasters = 0;       // Instances drawn into shadow cascades (summed over cascades)
		// GPU-driven frames: the cascades (a bit each) that kept their shadow map layer from the previous frame
		// (SceneRendererSpecification::CacheShadowCascades); the GPU's counts still count their casters.
		uint32_t CachedCascadeMask = 0;
		// Casters left out of cascades in which they would span less than a texel (summed over cascades): they would only flicker.
		uint32_t SubTexelCasters = 0;
		uint32_t SkinnedInstances = 0;    // Drawn instances of posed skinned meshes (view; also counted in Instances)
		// Animators whose meshes the frame drew (in view or by their shadows) with a pose older than their time: WhenVisible
		// ones that skipped their poses while unseen; the next frame shows them posed (a capture renders again for it).
		uint32_t StalePoses = 0;
		uint64_t PaletteBytesUploaded = 0; // Joint matrices uploaded this frame (only palettes that changed)
		uint32_t DebugLines = 0;          // Lines drawn from SceneRenderOptions::DebugShapes
		uint32_t OutlinedEntities = 0;    // Selected entities of the rendered scene
		uint32_t TintedEntities = 0;      // Entities of SceneRenderOptions::EntityTints in the rendered scene
		uint32_t Texts = 0;               // Text components drawn
		uint32_t ScreenSpaceTexts = 0;    // Of those, screen-space ones (the HUD)
		uint32_t HiddenScreenSpaceTexts = 0; // Screen-space text left out (SceneRenderOptions::DrawScreenSpaceText is off)
		uint32_t TextGlyphs = 0;
		uint32_t PendingTextGlyphs = 0;   // Glyphs waiting for rasterization (budgeted per frame), drawn on a later frame
		uint32_t RasterizedTextGlyphs = 0; // Glyphs added to font atlases this frame (see TextRenderer::c_FrameRasterBudget)
		SceneOverlayStats Overlays;       // The overlay passes (the game UI); their PendingAssets count in PendingAssets too
		// Motion (SceneRenderOptions::MotionVectors): whether the frame had the renderer's previous frame to compare with, and
		// what it copied into the motion history at its end: instances and joint matrices, all of them (MotionFullCopy) without
		// a history or when the scene's GPU copy wrote more since the previous frame than its write log remembers.
		bool MotionHistory = false;
		bool MotionFullCopy = false;
		uint32_t MotionCopiedInstances = 0;
		uint32_t MotionCopiedJoints = 0;
		// Temporal anti-aliasing (SceneRendererSpecification::TemporalAntiAliasing): whether the frame was drawn with it, the
		// frames its history holds (1 on a frame that reset it, 0 without it), the history's resets since the renderer was
		// made, and the frame's jitter in pixels (+x right, +y down; SceneRenderer::GetTemporalJitter of phase
		// (TemporalFrames - 1) modulo c_TemporalJitterPhases, 0 without it).
		bool TemporalAntiAliasing = false;
		uint32_t TemporalFrames = 0;
		uint32_t HistoryResets = 0;
		glm::vec2 Jitter = glm::vec2(0.0f);
		// Local light shadows (SceneRendererSpecification::LocalShadowAtlasSize): the point and spot lights with atlas tiles,
		// the shadow views (tiles) drawn this frame, and those of these lights still waiting to be drawn (their lights cast no
		// shadow until all of theirs are drawn, or keep the shadows drawn before they changed).
		uint32_t LocalShadowLights = 0;
		uint32_t LocalShadowViewsDrawn = 0;
		uint32_t LocalShadowViewsPending = 0;
		// Reflection probes (SceneRendererSpecification::MaxReflectionProbes): those the frame shaded with, the captures it
		// converted into the renderer's cube array, and the probes kept in view still waiting for their capture to load or be
		// converted (the sky lights their surfaces meanwhile).
		uint32_t ReflectionProbes = 0;
		uint32_t ReflectionProbeConversions = 0;
		uint32_t ReflectionProbesPending = 0;
		// Instances drawn into shadow views (summed over views) and their draw calls (the GPU-driven path's are counted on the
		// GPU: SceneRendererGPUStats::LocalShadowInstances, LocalShadowDraws, AddGPUCounts).
		uint32_t LocalShadowCasters = 0;
		uint32_t LocalShadowDrawCalls = 0;
		// The pre-exposure: the power of two every writer of the frame's HDR target multiplied its luminance by (shading, the sky,
		// fog and the clear color; the temporal history holds the same scale), so real luminance such as a 100,000 lux sun stays
		// within half floats (the exposure divides it out again). Forced (SceneRenderOptions::PreExposure); with manual exposure
		// the power of two nearest 2^Exposure; with automatic exposure the one nearest a quarter of the renderer's own metered
		// multiplier (two stops of headroom: the sun's disk), read back frames later without a stall, changed only when off by
		// more than a stop; 1 after ResetExposureAdaptation until a readback arrived. Settled when forced, with manual exposure,
		// or once a readback of the renderer's metering of a frame drawn with the current value arrived and kept it: until
		// then the brightest and darkest values of the frame may have overflowed or lost precision (captures wait for it).
		float PreExposure = 1.0f;
		bool PreExposureSettled = false;
	};

	// What the GPU did for a frame of a renderer, known frames later (SceneRenderer::GetGPUStats).
	struct SceneRendererGPUStats
	{
		bool HasTimes = false;
		uint64_t Frame = 0;    // The frame profiler's frame the times belong to (FrameSample::Index)
		float BusyMs = 0.0f;   // The renderer's GPU stages added up
		std::vector<GPUStageTime> StageMs; // Per GPU stage the renderer timed (FrameProfiler::Get()'s stage ids)

		// The GPU-driven path's counts (SceneRendererStats::GPUDriven) of the latest frame whose culling completed, and which
		// of the renderer's frames that was (its RenderCount).
		bool HasCounters = false;
		uint64_t CountersRender = 0;
		uint32_t Instances = 0;        // Opaque and alpha-masked instances drawn in the view
		uint32_t ShadowInstances = 0;  // Instances drawn into cascades (summed over cascades)
		uint32_t Draws = 0;            // Indirect draws with instances in the view
		uint32_t ShadowDraws = 0;
		// Of ShadowDraws, those of the cascades' second occlusion phase: of casters a cascade did not draw the frame before (new to
		// it, or found visible again). A record drawn in both phases is two draws where the CPU path makes one.
		uint32_t SecondPhaseShadowDraws = 0;
		uint32_t LocalShadowInstances = 0; // Instances drawn into the local shadow views (summed over views)
		uint32_t LocalShadowDraws = 0;
		uint32_t VisibleProxies = 0;   // Mesh renderers that passed the view's frustum and size tests
		uint32_t SizeCulledProxies = 0; // In the frustum, below SceneRendererSpecification::MinProjectedPixels
		uint32_t OccludedProxies = 0;  // In the frustum, hidden behind what the view draws (SceneRendererSpecification::OcclusionCulling)
		uint32_t OccludedCasters = 0;  // In a cascade's box, hidden behind what the cascade draws (summed over cascades)
		uint32_t SubTexelCasters = 0;
		uint32_t SkinnedInstances = 0;
		uint32_t Lights = 0;           // Point and spot lights in the view (all of them: no SceneRendererSpecification::MaxLights)
		uint32_t CulledLights = 0;     // Point and spot lights outside the view
		uint64_t Triangles = 0;        // Of the view's instances
		uint32_t DrawListEntries = 0;  // Instances in the draw list (view and cascades)
		uint32_t Overflow = 0;         // Instances there was no room for (not drawn): 0 unless the buffers could not grow
		std::array<uint32_t, 8> InstancesPerLOD = {};
	};

	// A frame's statistics with what the GPU drew added (for SceneRendererStats::GPUDriven frames, as far as the GPU's counts
	// are known: those of frame `gpu.CountersRender`, usually two or three frames older): instances, draws, triangles,
	// levels of detail, shadow casters and visible proxies as the CPU path counts them. Other frames' statistics unchanged.
	SceneRendererStats AddGPUCounts(const SceneRendererStats& stats, const SceneRendererGPUStats& gpu);

	// One instanced draw of a frame: instances of a submesh at one level of detail with one pipeline state.
	struct SceneDrawBatch
	{
		uint32_t MeshSlot = 0; // The render scene's mesh slot (RenderScene::GetMeshHandle)
		uint32_t SubmeshIndex = 0;
		uint32_t LOD = 0;
		bool DoubleSided = false;
		bool Mirrored = false;  // Instances with a mirroring transform: front faces wind clockwise
		bool Skinned = false;   // Posed skinned instances: the skinned pipelines blend their palettes
		bool AlphaMask = false; // Shadow batches: alpha-tested casters
		uint32_t Cascade = 0;   // Shadow batches
		uint32_t FirstInstance = 0; // Into the draw list (SceneRenderer::GetDrawList, GetShadowDrawList)
		uint32_t InstanceCount = 0;

		bool operator==(const SceneDrawBatch&) const = default;
	};

	// Renders a scene: depth/normal/entity-id prepass, ground-truth ambient occlusion, forward physically based
	// shading into an HDR target (directional light with cascaded soft shadows, clustered point and spot lights,
	// image-based lighting from a sky light's HDR environment map or procedural sky), the environment background and
	// transparent surfaces; then post-processing from the scene's PostProcessComponent: automatic exposure, bloom, tone
	// mapping, color grading and FXAA into a display target. Meshes, materials and textures come from the active asset
	// manager and render as soon as they are loaded (fallback textures and the default material are used meanwhile).
	// Editor views (preview lighting, hidden HUD, grid, selection outline, debug lines) are optional; see
	// SceneRenderOptions. Main thread only.
	class SceneRenderer
	{
	public:
		// Unit direction toward the sun of a procedural sky in a scene without a directional light: 45 degrees above the
		// horizon, between +X and +Z.
		static glm::vec3 GetDefaultSunDirection();
		// Unit direction toward the preview sun (SceneRenderOptions::PreviewEnvironment): 55 degrees above the horizon,
		// between +X and +Z, so the faces of a box seen from the editor camera's default direction (from +X+Z, above) get
		// clearly different amounts of light.
		static glm::vec3 GetPreviewSunDirection();
		// Whether SceneRenderOptions::PreviewEnvironment lights the scene: it has no active light of any kind.
		static bool NeedsPreviewLighting(Scene& scene);
		// The height fog the scene is rendered with: its first active HeightFogComponent, or null.
		static const HeightFogComponent* FindHeightFog(Scene& scene);

		explicit SceneRenderer(const SceneRendererSpecification& specification = {});
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Sizes the internal render targets. A size beyond the device's texture limit (or a failed allocation) is
		// logged, and Render then fails until a valid size is set.
		void SetViewportSize(uint32_t width, uint32_t height);
		glm::uvec2 GetViewportSize() const { return m_ViewportSize; }

		// Renders the scene into the output texture, or into `target` when given. A target must have the viewport size
		// (the renderer never rescales) and exactly one color attachment, single-sampled non-sRGB UNORM, e.g. the swapchain:
		// values are written sRGB-encoded. Returns false, with GetStats().Rendered false, when nothing was rendered: an
		// empty viewport, or an invalid target or render targets (both logged). Updates the scene's cached world
		// transforms. With overlays or text and a target, the image is composed in the output texture and then copied;
		// without them, a renderer that only renders into targets (a game) never allocates the output texture. The
		// meshes, materials and textures the frame used are let go when it returns, so a renderer that stops rendering
		// keeps none of them resident; only the sky light's environment map stays, in the environment cache, until the
		// environment changes (and the text renderer's glyph atlases keep their fonts).
		bool Render(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target = nullptr, const SceneRenderOptions& options = {});

		// RGBA8 UNORM with sRGB-encoded values (display-ready), valid after Render without a target (null until a frame
		// needed it, see Render).
		nvrhi::ITexture* GetOutputTexture() const { return m_OutputTexture; }
		// The frame's HDR scene color (RGBA16_FLOAT): luminance times the frame's pre-exposure (SceneRendererStats::PreExposure).
		nvrhi::ITexture* GetHDRTexture() const { return m_HDRTexture; }
		nvrhi::ITexture* GetDepthTexture() const { return m_DepthTexture; }
		nvrhi::ITexture* GetNormalTexture() const { return m_NormalTexture; }
		nvrhi::ITexture* GetEntityIDTexture() const { return m_EntityIDTexture; } // Null without one (SceneRendererSpecification::EntityIDs)
		nvrhi::ITexture* GetAmbientOcclusionTexture() const { return m_AOTexture; } // 1 = unoccluded
		// The atmosphere's transmittance and sky-view tables (RGBA16_FLOAT, Include/Atmosphere.glsl): null while the sky is not
		// the atmosphere.
		nvrhi::ITexture* GetAtmosphereTransmittanceTable() const { return m_Atmosphere ? m_Atmosphere->Transmittance.Get() : nullptr; }
		nvrhi::ITexture* GetAtmosphereSkyViewTable() const { return m_Atmosphere ? m_Atmosphere->SkyView.Get() : nullptr; }
		// The atmosphere's environment is computed again once the sun moved more than this many degrees, in this many steps.
		static constexpr float c_AtmosphereRefreshAngle = 0.5f;
		static constexpr uint32_t c_AtmosphereRefreshSteps = 6 + 2 + 6; // Faces, the cube's levels and irradiance, prefiltered levels
		// The sky-view table follows the camera's height above the ground in bands of this many meters.
		static constexpr float c_AtmosphereHeightBand = 250.0f;
		// Image-based lighting data of the current environment (null without one); the BRDF lookup table always exists.
		nvrhi::ITexture* GetEnvironmentCube() const { return m_Environment ? m_Environment->Cube.Get() : nullptr; }
		nvrhi::ITexture* GetIrradianceMap() const { return m_Environment ? m_Environment->Irradiance.Get() : nullptr; }
		nvrhi::ITexture* GetPrefilteredMap() const { return m_Environment ? m_Environment->Prefiltered.Get() : nullptr; }
		nvrhi::ITexture* GetBRDFLut() const { return m_BRDFLut; }

		nvrhi::ITexture* GetBloomTexture() const { return m_BloomTexture; } // Level 0 of the bloom chain (half resolution)
		// The exposure of the latest frame (Include/Exposure.glsl): the adapted log2 average luminance and the multiplier of
		// luminance the tone mapping applies.
		nvrhi::IBuffer* GetExposureBuffer() const { return m_ExposureBuffer; }
		// The view's depth pyramid occlusion culling tests against (R32_FLOAT, every level): null without the GPU-driven path.
		nvrhi::ITexture* GetDepthPyramid() const { return m_DepthPyramid; }
		// The motion vectors of the latest frame that asked for them (SceneRenderOptions::MotionVectors): RG16_FLOAT, in UV units
		// (current minus previous position), 0 where nothing was drawn. Null until a frame asks, and again once the render
		// targets are released.
		nvrhi::ITexture* GetVelocityTexture() const { return m_VelocityTexture; }
		// The motion history (SceneRenderOptions::MotionVectors): the render scene's instances (RenderData::InstanceData) and
		// joint matrices as this renderer's latest frame with motion drew them, indexed like the scene's GPU copy. Null until a
		// frame asks for motion, and again once the render targets are released.
		nvrhi::IBuffer* GetPreviousInstanceBuffer() const { return m_PreviousInstances; }
		nvrhi::IBuffer* GetPreviousPaletteBuffer() const { return m_PreviousPalette; }
		// The temporal anti-aliasing history's newest image (RGBA16_FLOAT, the resolved HDR scene color of the latest frame drawn
		// with it): null until a frame draws a scene with TAA (SceneRendererSpecification::TemporalAntiAliasing), and again
		// once the render targets are released. Two textures of the viewport's size, used in turn.
		nvrhi::ITexture* GetTemporalHistoryTexture() const { return m_HistoryTextures[m_HistoryIndex]; }
		// The HDR image the latest frame's post-processing read: the resolved one with temporal anti-aliasing, else the HDR target.
		nvrhi::ITexture* GetResolvedHDRTexture() const { return m_TemporalResolved ? m_HistoryTextures[m_HistoryIndex].Get() : m_HDRTexture.Get(); }
		// The local light shadows' depth atlas (SceneRendererSpecification::LocalShadowAtlasSize): null until a frame draws a
		// point or spot light that casts shadows, and again once the render targets are released.
		nvrhi::ITexture* GetLocalShadowAtlas() const { return m_LocalShadowAtlas; }
		// Frames over which a light's shadow fades in once all its views are drawn (SceneRendererSpecification::
		// SpreadLightingUpdates).
		static constexpr uint32_t c_LocalShadowFadeFrames = 8;
		// The reflection probes' cube array (RGBA16_FLOAT, RenderData::c_ReflectionProbeSize texels across, one level per
		// roughness step, a cube per slot of SceneRendererSpecification::MaxReflectionProbes): null until a frame has a probe in
		// view, and again once the render targets are released or a frame's scene has no probe.
		nvrhi::ITexture* GetReflectionProbeArray() const { return m_ReflectionProbes ? m_ReflectionProbes->Array.Get() : nullptr; }
		// Captures converted per frame with SceneRendererSpecification::SpreadLightingUpdates.
		static constexpr uint32_t c_ReflectionProbeConversionsPerFrame = 2;

		// Entity visible at a pixel of the last rendered frame (invalid when none, when no frame was rendered since the last
		// resize, or without an entity-ID buffer). Blocks until the GPU is done; meant for tests and tools (editors use
		// ReadEntityIDAsync).
		Entity GetEntityAt(Scene& scene, uint32_t x, uint32_t y);
		// Starts reading the entity ID at a pixel of the last rendered frame without waiting for the GPU. Null when no
		// frame was rendered since the last resize, the renderer writes no entity IDs, the pixel is outside the viewport or
		// the copy cannot be made. Once
		// the readback is ready, its single R32_UINT pixel resolves with GetEntityFromID (against the scene that was
		// rendered: entities destroyed meanwhile resolve to an invalid entity).
		Scope<TextureReadback> ReadEntityIDAsync(uint32_t x, uint32_t y);
		// The entity an entity-ID buffer value refers to; invalid for 0 (nothing drawn) or entities that no longer exist.
		static Entity GetEntityFromID(Scene& scene, uint32_t id);

		// Whether an overlay pass would draw screen-space content of the scene (its game UI). GameRenderer asks it of scenes
		// without a camera.
		bool HasOverlayContent(Scene& scene) const;

		// Automatic exposure adapts gradually; this makes the next frame jump to the scene's brightness (camera cuts, scene loads,
		// a capture's first frame) and starts the pre-exposure again from 1 (SceneRendererStats::PreExposure).
		void ResetExposureAdaptation()
		{
			m_ResetExposure = true;
			m_ResetPreExposure = true;
		}
		// Only the jump of ResetExposureAdaptation: the next frame's automatic exposure is the brightness it meters, and the
		// pre-exposure keeps its value (the later frames of a capture, each metered alone, wait for it to settle).
		void JumpExposureAdaptation() { m_ResetExposure = true; }
		// Stops of headroom the automatic pre-exposure keeps above the metered exposure (SceneRendererStats::PreExposure).
		static constexpr int32_t c_PreExposureHeadroomStops = 2;
		// The pre-exposure stays within 2^-c_MaxPreExposureStops and 2^c_MaxPreExposureStops.
		static constexpr int32_t c_MaxPreExposureStops = 24;
		// The power of two nearest 2^stops within that range (1 for a non-finite value).
		static float GetPreExposureFor(float stops);
		// Temporal anti-aliasing starts its history again with the next frame (scene loads; SceneRendererSpecification::
		// TemporalAntiAliasing).
		void ResetHistory() { m_ResetHistory = true; }
		// The sub-pixel offset of temporal anti-aliasing's samples in a phase (0 to c_TemporalJitterPhases - 1), in pixels (+x
		// right, +y down): a Halton (2, 3) point in (-0.5, 0.5).
		static constexpr uint32_t c_TemporalJitterPhases = 8;
		static glm::vec2 GetTemporalJitter(uint32_t phase);

		const SceneRendererSpecification& GetSpecification() const { return m_Specification; }
		// What the renderer chooses levels of detail by at its viewport size (LODSelection::SelectLOD): tools showing the level
		// it draws use these.
		LODCriteria GetLODCriteria() const;
		const SceneRendererStats& GetStats() const { return m_Stats; }
		// The GPU's side of the latest frame whose GPU work completed (2-3 frames late; Render resolves them without waiting).
		const SceneRendererGPUStats& GetGPUStats() const { return m_GPUStats; }
		// Resolves the GPU stats of the frames rendered so far; with `wait`, waits for the GPU first so the last frame's are
		// known (tests and tools).
		void ResolveGPUStats(bool wait);
		// Frames rendered (Render calls that returned true) since the renderer was created.
		uint64_t GetRenderCount() const { return m_RenderCount; }
		// Whether frames are culled and drawn by the GPU (SceneRendererSpecification::GPUDriven and the device).
		bool IsGPUDriven() const;

		// The materials the last frame drew, with the largest size on screen of what they cover (for mip streaming).
		std::span<const VisibleMaterialUse> GetVisibleMaterials() const { return m_VisibleMaterials; }
		// Their loaded texture maps, one entry per material and map (what the renderer reports to the texture streamer).
		std::span<const VisibleTextureUse> GetVisibleTextures() const { return m_VisibleTextures; }
		// What the last frame drew: its batches and draw lists, which hold the index of every drawn instance
		// (RenderScene::GetInstances) in batch order: opaque batches, then transparent ones back to front; shadow batches
		// cascade by cascade. The GPU-driven path builds its draws on the GPU: there the opaque and shadow batches are empty,
		// and the transparent batches and the draw list are the CPU's transparent draws.
		std::span<const SceneDrawBatch> GetOpaqueBatches() const { return m_OpaqueBatches; }
		std::span<const SceneDrawBatch> GetTransparentBatches() const { return m_TransparentBatches; }
		std::span<const SceneDrawBatch> GetShadowBatches() const { return m_ShadowBatches; }
		std::span<const uint32_t> GetDrawList() const { return m_DrawInstances; }
		std::span<const uint32_t> GetShadowDrawList() const { return m_ShadowDrawInstances; }
	private:
		enum class Pass : uint8_t
		{
			Prepass = 0,
			Opaque,
			Transparent,
			Shadow,
			ShadowMasked,
			SelectionMask, // The selected instances into the selection mask (temporal anti-aliasing's outline)
			LocalShadow,   // Into a point or spot light's shadow view (a tile of the local shadows' atlas)
			LocalShadowMasked
		};

		struct PipelineKey
		{
			Pass PassType = Pass::Opaque;
			bool DoubleSided = false;
			bool Mirrored = false; // Instances with a mirroring transform: front faces wind clockwise
			bool Skinned = false;  // The skinned vertex shaders and the skin vertex stream
			// The prepass and the forward pass of a frame with motion vectors: the velocity vertex shaders (both passes, so their
			// depths agree), and the prepass writes the velocity target.
			bool Velocity = false;
			bool operator==(const PipelineKey&) const = default;
		};

		struct ShadowLight
		{
			bool Enabled = false;
			glm::vec3 Direction = { 0.0f, -1.0f, 0.0f };
			float Distance = 80.0f;
			float Softness = 1.0f;
			float Bias = 1.0f;
			float NormalBias = 1.5f;
			float CascadeBlend = 0.1f; // DirectionalLightComponent::CascadeBlend, within [0, 0.5]
		};

		// A transparent submesh instance, drawn on its own, back to front.
		struct TransparentDraw
		{
			float ViewDepth = 0.0f;
			uint32_t EntityID = 0;     // Breaks ties of equal depth, with the submesh
			uint32_t SubmeshIndex = 0;
			uint32_t Instance = 0;
			uint32_t MeshSlot = 0;
			uint32_t LOD = 0;
			bool DoubleSided = false;
			bool Mirrored = false;
			bool Skinned = false;
		};

		// A render proxy found by the shadow cascade queries.
		struct ShadowCaster
		{
			uint32_t Proxy = 0;
			uint32_t CascadeMask = 0; // Cascades whose box it overlaps
			float LightNearZ = 0.0f;  // Its nearest point toward the light (light view space)
		};

		// A cascade: a box in light view space (looking along the light: -Z points away from it). Its square covers the
		// bounding sphere of a view frustum slice; its depth range is fitted to the casters.
		// A cascade's square and depth in light view space: absolute (the CPU's caster queries and tests), and relative to the
		// camera's light-space position (what the GPU culls and fits with, Docs/Architecture.md, "Camera-relative rendering").
		struct CascadeBox
		{
			bool operator==(const CascadeBox&) const = default;

			glm::vec2 Min = glm::vec2(0.0f);
			glm::vec2 Max = glm::vec2(0.0f);
			float FarZ = 0.0f;        // Far side of the slice's bounding sphere
			float SphereNearZ = 0.0f; // Near side (toward the light)
			float WorldPerTexel = 0.0f;
			float Radius = 0.0f;
			glm::vec2 RelativeMin = glm::vec2(0.0f);
			glm::vec2 RelativeMax = glm::vec2(0.0f);
			float RelativeFarZ = 0.0f;
			float RelativeSphereNearZ = 0.0f;
		};

		// How large a material slot's draws are on screen (VisibleMaterialUse): the largest projected size, and the most
		// pixels a unit of texture coordinates spans.
		struct MaterialScreenUse
		{
			uint32_t Slot = 0;
			float PixelSize = 0.0f;
			float UVPixels = 0.0f;
		};

		// A loaded texture map of a material the frame draws, as ResolveAssets resolved it (for VisibleTextureUse), and a
		// material slot's range of them.
		struct ResolvedTexture
		{
			AssetHandle Handle;
			uint32_t Size = 0;
			uint32_t MipCount = 0;
		};
		struct ResolvedTextureRange
		{
			uint32_t First = 0;
			uint32_t Count = 0;
		};

		// The materials a frame resolved one by one (ResolveMaterials), for later frames that use the same material slots in
		// the same ways (ReuseResolvedMaterials): when the materials and their textures are still the loaded objects that
		// frame found (materials are not changed in place: edits publish new objects), the materials' flags and texture maps
		// are what that frame found. Their GPU data is the render scene's GPU copy's (RenderSceneGPU), which follows content
		// changes by itself.
		struct MaterialResolution
		{
			struct Slot
			{
				uint32_t Index = 0; // The material slot
				uint32_t Usage = 0; // How the frame used it (usage bits)
				AssetHandle Handle;
				uint32_t Flags = 0; // m_MaterialFlags
			};

			bool Valid = false;
			uint64_t RenderSceneId = 0;
			std::vector<Slot> Slots; // In the order of m_UsedMaterialSlots
			AssetResolveCache::Recording Lookups;        // The materials and textures, replayed by later frames
		};

		// What one chunk of the parallel extraction produced; chunks are merged in order, so the result does not depend on
		// how they were spread over threads.
		struct ExtractionChunk
		{
			std::vector<SortItem> Draws;
			std::vector<TransparentDraw> Transparent;
			std::vector<std::pair<uint32_t, uint32_t>> MeshMarks;      // (mesh slot, usage bits), runs merged
			std::vector<std::pair<uint32_t, uint32_t>> MaterialMarks;  // (material slot, usage bits), runs merged
			std::vector<MaterialScreenUse> MaterialScreenUses;         // Runs of a material slot merged
			std::array<uint32_t, SceneRendererStats::c_LODStatCount> InstancesPerLOD = {};
			std::array<float, RenderData::c_MaxCascades> CasterNearZ = {};
			uint32_t Skipped = 0; // Submeshes beyond what a sort key can address
			std::vector<entt::entity> Animators; // Animators of the chunk's visible Skin proxies (WhenVisible culling), or of its casters
			std::vector<ShadowCaster> Casters;   // The chunk's shadow casters (FindShadowCasters)
			uint32_t SubTexelCasters = 0;
			uint32_t SizeCulled = 0;             // Visible proxies below SceneRendererSpecification::MinProjectedPixels
			AABB Receivers; // The world bounds of the chunk's visible proxies (scenes with Skin proxies only)
			uint32_t SkinnedInstances = 0;
		};

		struct LightCandidate
		{
			RenderData::LightData Data;
			glm::vec4 Bounds; // World-space bounding sphere of the lit region (xyz: center, w: radius)
			float Relevance = 0.0f;
			uint32_t Order = 0; // Registry order, which breaks ties between equally relevant lights
		};

		// Local light shadows (SceneRendererLocalShadows.cpp). What a shadow view shows, in world space (the camera is not part
		// of it: the matrices that draw and sample a view are made relative to the frame's camera from it).
		struct LocalShadowViewKey
		{
			glm::vec3 Position = glm::vec3(0.0f);  // The light's
			glm::vec3 Direction = glm::vec3(0.0f); // The view's axis: the spot light's, or a cube face's
			float Range = 0.0f;                    // The far plane
			float Near = 0.0f;
			float TanHalfAngle = 0.0f;             // Of the view's field of view, widened by the tile's guard band
			ShadowAtlasTile Tile;
			bool operator==(const LocalShadowViewKey&) const = default;
		};
		// A shadow view's tile: what it should show this frame, what it holds, and whether a caster's change reached what it holds.
		struct LocalShadowView
		{
			LocalShadowViewKey Key;
			LocalShadowViewKey DrawnKey;      // Valid while HasContent
			std::array<glm::vec4, 6> DrawnPlanes = {}; // DrawnKey's frustum in world space (what changes it)
			uint64_t DrawnEpoch = 0;          // m_LocalShadowEpoch when it was drawn
			bool HasContent = false;
			bool Dirty = false;
		};
		// A point or spot light with shadows: its tiles and views, kept from frame to frame while it stays among the shadowed
		// lights, with the atlas tiles it holds.
		struct LocalShadowLight
		{
			entt::entity Entity = entt::null; // With Type, which light it is
			uint32_t Type = 0;
			uint32_t Record = 0;              // RenderLights' record this frame
			float Relevance = 0.0f;
			float RankFade = 1.0f;            // Its margin over the most relevant light left out (SpreadLightingUpdates)
			uint32_t WantedSize = 0;          // The tile size its size on screen asks for (with hysteresis)
			uint32_t TileSize = 0;            // The size of its tiles (0: none)
			uint32_t ViewCount = 0;
			std::array<ShadowAtlasTile, RenderData::c_LocalShadowFaces> Tiles = {};
			std::array<LocalShadowView, RenderData::c_LocalShadowFaces> Views = {};
			glm::vec3 ShadowSettings = glm::vec3(0.0f); // LightShadowSettings: depth bias, normal bias, filter radius
			uint32_t FadeFrames = 0;          // Frames its views have all held content since they last did not
			bool Complete = false;            // This frame: every view holds content (or is drawn)
			float Strength = 0.0f;            // This frame's (RenderData::LocalShadowData::Parameters.w)
		};
		// A shadow view the frame draws: its light (shadow slot) and face, the casters the CPU found in it, and its batches.
		struct LocalShadowDrawView
		{
			uint32_t Light = 0;
			uint32_t Face = 0;
			std::array<glm::vec4, 6> Planes = {}; // Its frustum, camera-relative (as the culling's positions are)
			LODCriteria LOD;                      // Levels of detail chosen as for a viewport of its tile's height
			uint32_t FirstCaster = 0;             // m_LocalShadowCasters
			uint32_t CasterCount = 0;
			uint32_t FirstBatch = 0;              // m_LocalShadowBatches
			uint32_t BatchCount = 0;
			uint32_t FirstCandidate = 0;          // m_LocalShadowCandidates: the proxies within its light's range
			uint32_t CandidateCount = 0;
		};
		// A shadow caster the CPU found in a view, with its screen fraction there (projected diameter over the view's height).
		struct LocalShadowCaster
		{
			uint32_t Proxy = 0;
			float Fraction = 0.0f;
		};
		// What the shadow views' content depends on beyond the casters' placement (see SceneRendererSpecification::
		// CacheLocalShadows): a change redraws every view.
		struct LocalShadowContent
		{
			uint64_t RenderSceneId = 0;
			uint64_t TablesVersion = 0;
			uint64_t PoolContentVersion = 0;
			uint64_t TextureEntryVersion = 0;  // While the scene has alpha-masked materials (0 otherwise)
			uint64_t DeferredFrame = 0;        // The frame while the scene's GPU copy defers new proxies (0 otherwise)
			bool operator==(const LocalShadowContent&) const = default;
		};

		// What an atmosphere's tables are computed for (SkyLightSource::Atmosphere): its settings and the light's color (not
		// its intensity) over its largest component (the tables hold the sky per lux of it).
		struct AtmosphereSettings
		{
			float Density = 1.0f;
			float Haze = 1.0f;
			float GroundAlbedo = 0.3f;
			glm::vec3 SunColor = glm::vec3(1.0f);

			bool operator==(const AtmosphereSettings&) const = default;
		};
		struct EnvironmentMaps
		{
			Ref<Texture> Source; // Equirectangular HDR texture the maps were computed from (null for a procedural sky)
			std::optional<RenderData::ProceduralSkyParameters> Procedural; // The procedural sky the maps were computed from
			// The atmosphere the maps were computed from, and its sun's direction (toward it) then
			std::optional<AtmosphereSettings> Atmosphere;
			glm::vec3 AtmosphereSun = glm::vec3(0.0f);
			nvrhi::TextureHandle Cube;        // Radiance cubemap with mips (background, irradiance source)
			nvrhi::TextureHandle Irradiance;  // Diffuse irradiance
			nvrhi::TextureHandle Prefiltered; // Specular radiance, one mip per roughness level
		};
		// The atmosphere (SceneRendererAtmosphere.cpp): made with the first frame whose sky uses it, released with the first that
		// does not. Its tables (Include/Atmosphere.glsl) are computed when what they depend on changes; its environment is
		// double-buffered: while the sun moved more than c_AtmosphereRefreshAngle since the current one was computed, or the
		// settings changed, the other set of maps is computed in c_AtmosphereRefreshSteps steps (one per frame with
		// SceneRendererSpecification::SpreadLightingUpdates, else all at once) and then takes over; changes meanwhile wait for
		// the next refresh, which starts once it is done.
		struct AtmosphereState
		{
			nvrhi::BindingLayoutHandle Layout;
			nvrhi::ComputePipelineHandle TransmittancePipeline;
			nvrhi::ComputePipelineHandle MultipleScatteringPipeline;
			nvrhi::ComputePipelineHandle SkyViewPipeline;
			nvrhi::ComputePipelineHandle CubePipeline;
			nvrhi::SamplerHandle Sampler; // Linear, repeating along u (the sky view's azimuth), clamped along v
			nvrhi::BufferHandle Constants; // RenderData::AtmosphereConstants
			nvrhi::TextureHandle Transmittance;
			nvrhi::TextureHandle MultipleScattering;
			nvrhi::TextureHandle SkyView;
			nvrhi::BindingSetHandle TransmittanceSet;
			nvrhi::BindingSetHandle MultipleScatteringSet;
			nvrhi::BindingSetHandle SkyViewSet;
			std::optional<AtmosphereSettings> TableSettings; // What the transmittance and multiple-scattering tables hold
			glm::vec3 SkyViewSun = glm::vec3(0.0f);          // What the sky-view table holds, with TableSettings
			int32_t SkyViewBand = -1;
			bool SkyViewValid = false;
			// The sun's transmittance to the ground (Atmosphere::ComputeSunTransmittance) for these, computed once per change.
			std::optional<AtmosphereSettings> SunSettings;
			glm::vec3 SunDirection = glm::vec3(0.0f);
			float SunRadius = 0.0f;
			glm::vec3 SunTransmittance = glm::vec3(1.0f);
			// The environment being computed (its settings and sun in Atmosphere and AtmosphereSun) and its next step; the
			// maps of the set not in use, which a refresh writes into.
			std::optional<EnvironmentMaps> Refresh;
			uint32_t RefreshStep = 0;
			std::optional<EnvironmentMaps> Spare;
		};
		// The environments computed from environment map textures, by texture, while a renderer lights with them
		// (FindSharedEnvironment); defined with the functions that use it.
		struct SharedEnvironments;
		// Reflection probes (SceneRendererReflectionProbes.cpp): the cube array, a slot per capture, and what converts captures
		// into it. A slot holds the capture it was converted from (a reload of it converts again) and is given to another capture
		// only when the frame does not use it, least recently used first.
		struct ReflectionProbeSlot
		{
			AssetHandle Capture = 0;             // 0: free
			std::weak_ptr<const Texture> Source; // The texture object converted into it (expired or another: not converted)
			uint64_t LastUsed = 0;               // The latest frame (m_RenderCount) that used it
		};
		struct ReflectionProbeState
		{
			nvrhi::TextureHandle Array;   // The slots' prefiltered cubes
			nvrhi::TextureHandle Scratch; // A capture's cube with its levels (RenderData::c_ReflectionProbeSize), filtered into a slot
			std::vector<ReflectionProbeSlot> Slots;
		};
		// A capture the frame converts (its texture, held by the frame) into a slot.
		struct ReflectionProbeConversion
		{
			uint32_t Slot = 0;
			Ref<Texture> Source;
		};

		void CreateResources();
		void CreateIBLResources();
		// Recreates the viewport-sized targets; false (targets released, error logged) when they cannot be created. The
		// output texture is not among them: EnsureOutputTarget creates it once a frame needs it.
		bool CreateRenderTargets();
		void ReleaseRenderTargets();
		// The output texture, with its framebuffers and its copy binding: created at the viewport size by the first frame
		// rendered without a target or composing overlays or text, and kept until the targets are recreated or released.
		// False (error logged) when it cannot be created.
		bool EnsureOutputTarget();
		bool ValidateTarget(nvrhi::IFramebuffer* target);
		// Finds the scene's sky light (or the preview sky); (re)computes the environment maps when its environment texture
		// or its procedural sky's parameters changed.
		void UpdateEnvironment(Scene& scene, bool preview, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList);
		// The frame's fog constants from the scene's height fog (FindHeightFog), after the lights and the environment, whose
		// light it scatters.
		void SetHeightFog(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame);
		// Keeps the sky light's environment map whole in the active asset manager's texture streamer until this renderer has
		// computed (or taken over) its environment, also through frames it does not draw (TextureStreamer::Hold).
		void HoldEnvironmentMap(AssetHandle map);
		// Reports what the frame drew to the active asset manager's texture streamer, for the levels its textures need.
		void ReportTextureUse(const SceneCamera& camera);
		// New environment textures (radiance cube with mips, irradiance, prefiltered).
		EnvironmentMaps CreateEnvironmentMaps();
		// The environment maps of an equirectangular texture (new textures, so maps of the previous source are never
		// overwritten while it may still be in use), shared with the renderers that start lighting with the same texture.
		void ProcessEnvironment(nvrhi::ICommandList* commandList, const Ref<Texture>& source);
		// The environment another renderer computed from `source` and still lights with, or null. Shared, so a renderer
		// that starts drawing a scene lit by a map (a capture renderer, a second view) neither waits for the map's levels,
		// which drop to its tail once the environment is computed, nor computes it again. Maps written in place (procedural
		// skies) are never shared.
		static Ref<const EnvironmentMaps> FindSharedEnvironment(const Texture& source);
		static void ShareEnvironment(const Ref<const EnvironmentMaps>& environment);
		static SharedEnvironments& GetSharedEnvironments();
		// The environment maps of a procedural sky, computed into the current procedural sky's textures when there are
		// some (parameter changes, such as a moving sun, then allocate nothing).
		void GenerateProceduralSky(nvrhi::ICommandList* commandList, const RenderData::ProceduralSkyParameters& parameters);
		// Mips of the radiance cube from its mip 0, then the irradiance and prefiltered maps from the cube: all of
		// c_EnvironmentFilterSteps steps, or one of them (the cube's mips, the irradiance, each prefiltered level).
		void FilterEnvironment(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps);
		void FilterEnvironmentStep(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps, uint32_t step);
		// The atmosphere's tables, light and environment for the frame (SceneRendererAtmosphere.cpp): false (reported) when its
		// resources could not be made, and the scene then has no environment.
		bool UpdateAtmosphere(const SkyLightComponent& sky, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList);
		bool EnsureAtmosphereResources();
		void WriteAtmosphereConstants(nvrhi::ICommandList* commandList, const AtmosphereSettings& settings, const glm::vec3& towardSun, int32_t band);
		// Step `step` of computing an atmosphere environment into `maps`: a face of the cube's level 0 from the sky-view table
		// (steps 0 to 5), then FilterEnvironmentStep.
		void RecordAtmosphereEnvironmentStep(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps, uint32_t step);
		RenderData::ProceduralSkyParameters GetProceduralSkyParameters(const SkyLightComponent& sky, const RenderData::FrameConstants& frame) const;
		void DispatchIBL(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::ITexture* source, const nvrhi::TextureSubresourceSet& sourceSubresources,
			nvrhi::TextureDimension sourceDimension, nvrhi::ITexture* output, uint32_t outputMip, nvrhi::TextureDimension outputDimension,
			const RenderData::IBLParameters& parameters, uint32_t layers, uint32_t outputFirstLayer = 0);
		bool CreateSceneBindingSets();
		// What the scene binding sets bind as the sky-view table: the atmosphere's in its frames, else a black texture.
		nvrhi::ITexture* GetBoundSkyView() const;
		// What the scene binding sets bind as the reflection probes' cube array: a one-slot black array without probes.
		nvrhi::ITexture* GetBoundReflectionProbes() const;
		// The frame's reflection probes (SceneRendererReflectionProbes.cpp), on the CPU after the lights: the strongest in view,
		// their captures resolved and held whole until converted, the conversions the frame records and the probes it shades
		// with (those whose capture is converted, or is this frame).
		// False (reported) when its resources cannot be made: the scene is then lit without probes.
		bool GatherReflectionProbes(Scene& scene, const SceneCamera& camera);
		// The cube array and the conversion's pipeline: false (reported) when they cannot be made.
		bool EnsureReflectionProbeResources();
		// Writes the frame's probe list into the probe buffer (when it has probes, or the previous upload had some).
		void UploadReflectionProbes(nvrhi::ICommandList* commandList);
		// Converts the frame's captures into their slots (GPU.Reflections): each into a cube with its levels, prefiltered into
		// the slot's levels, and projected into the slot's irradiance coefficients.
		void ConvertReflectionProbes(nvrhi::ICommandList* commandList);
		void ReleaseReflectionProbes();
		// The prepass's framebuffer of the frame: with the velocity target in a frame with motion vectors.
		nvrhi::IFramebuffer* GetPrepassFramebuffer() const { return m_MotionFrame ? m_MotionPrepassFramebuffer.Get() : m_PrepassFramebuffer.Get(); }
		// The motion history's buffers (SceneRenderOptions::MotionVectors) at the size of the scene's GPU copy's, and the velocity
		// target with its prepass framebuffer; with `keepContents` a buffer that grows keeps what it held (copied at the frame's
		// start, RecordMotionGrowth). False (logged) when they cannot be created.
		bool EnsureMotionResources(bool keepContents);
		void RecordMotionGrowth(nvrhi::ICommandList* commandList);
		// After the frame's draws: copies into the motion history what the scene's GPU copy wrote since the renderer's previous
		// frame, or all of it with `full`.
		void RecordMotionCopy(nvrhi::ICommandList* commandList, const RenderScene& renderScene, bool full);
		// The temporal anti-aliasing history's textures and the binding sets that read them, made by the first frame with TAA;
		// false (reported) when they cannot be created.
		bool EnsureTemporalResources();
		// Blends the frame's HDR image into the history's other texture, which becomes its newest (PostProcess/TAA.comp).
		void ResolveTemporal(nvrhi::ICommandList* commandList);
		void RenderAmbientOcclusion(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		// Meters the HDR image (automatic exposure) through `bindingSet` (the HDR target's or the resolved image's) or writes the
		// manual exposure into the exposure buffer.
		void UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* bindingSet);
		// The frame's pre-exposure (m_PreExposure) from the options, the settings and the readbacks of earlier frames' metering.
		void ChoosePreExposure(const PostProcessComponent& settings, const SceneRenderOptions& options);
		// After the executed command list of a frame that recorded the exposure's copy: the copy is pending.
		void SubmitExposureReadback();
		// The bloom chain of the image `prefilterSet` reads (the first downsample: the HDR target's or the resolved image's).
		void RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* prefilterSet);
		// The scene's first active post-processing settings, or the defaults.
		static PostProcessComponent GetPostProcessSettings(Scene& scene);
		nvrhi::IGraphicsPipeline* GetScenePipeline(const PipelineKey& key);
		// Fullscreen-triangle pipeline for a pixel shader and target format, cached per format.
		nvrhi::IGraphicsPipeline* GetFullscreenPipeline(std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>>& cache, nvrhi::IShader* pixelShader,
			nvrhi::IBindingLayout* layout, const nvrhi::FramebufferInfo& framebufferInfo);
		// Grows a structured (or vertex) buffer to hold elementCount elements (the replaced buffer is released once the GPU
		// is done with it); false when the allocation failed.
		enum class BufferKind : uint8_t
		{
			Structured = 0,
			Vertex,
			IndirectArguments,
			StructuredUAV,       // Written by compute shaders too
			IndirectArgumentsUAV // Indirect arguments compute shaders write (a structured buffer of words)
		};
		bool EnsureBufferCapacity(nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize, const char* name, bool& outRecreated,
			BufferKind kind = BufferKind::Structured);

		// The directional light (the preview sun with `preview`), ambient light and the visible point and spot lights.
		// The directional light, the sky's ambient and, on the CPU path (or without GPU light culling), the point and spot
		// lights of the render scene's records in view, ranked and capped at MaxLights.
		void CollectLights(Scene& scene, const RenderScene& renderScene, const SceneCamera& camera, bool preview, RenderData::FrameConstants& frame);

		// The GPU timer record of this frame (see m_GPURecords).
		GPUTimerRecord& BeginGPURecord();
		// Render without letting go of the frame's assets (Render does that, whatever way the frame ends).
		bool RenderFrame(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target, const SceneRenderOptions& options);

		// Extraction (SceneRendererExtraction.cpp): what the frame draws, from the scene's RenderScene.
		// Sizes the per-slot state for the render scene; a render scene other than the last frame's starts it afresh.
		void PrepareExtraction(RenderScene& renderScene);
		// Lets go of the meshes, materials and textures the frame resolved.
		void ReleaseFrameAssets();
		// The proxies in the view frustum and in the prefetch distance, and the assets they need; meshes whose bounds are
		// unknown are needed as if in view.
		void CullView(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum);
		// The marks of m_VisibleProxies (in the view): screen fractions, request priorities, animators and receivers.
		void MarkVisibleProxies(RenderScene& renderScene, const SceneCamera& camera);
		// Meshes whose bounds are unknown, requested as if in view while any of their proxies is active.
		void MarkMeshesWithoutBounds(RenderScene& renderScene);
		// The direction a camera looks in, and whether it jumped since the previous frame (no previous frame counts as a cut).
		static glm::vec3 GetViewDirection(const SceneCamera& camera);
		bool IsCameraCut(const SceneCamera& camera) const;
		// Whether the frame takes its requests from the GPU's visibility feedback (SceneRendererSpecification::
		// VisibilityFeedback): a GPU-driven frame with recent feedback of the render scene, culled after the latest camera cut
		// with the mesh and material tables its GPU copy holds now (assets loaded or dropped since then were not seen), and
		// no overflow since the CPU's marks last ran.
		bool UsesFeedback(const RenderScene& renderScene) const;
		// CullView's work on a feedback frame: the feedback's marks and screen sizes, and the proxies the CPU still looks at in
		// the view (users of transparent materials, skinned proxies).
		void ApplyFeedback(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum);
		// Computes the cascades and, with `findCasters`, finds the shadow casters of each (feedback frames leave that to the
		// GPU); leaves ShadowParams.w at 0 without shadows. Reports the animators of skinned casters out of view whose shadows
		// can fall on what the view shows (CanShadowView).
		void FindShadowCasters(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum, RenderData::FrameConstants& frame, bool findCasters);
		// The cascades a caster's light view bounds fall into (bits; casters below a texel of a cascade left out of it, and
		// counted in `subTexel`).
		uint32_t GetCascadeMask(const AABB& lightBounds, uint32_t& subTexel) const;
		// Whether a caster's shadow can fall on what the view shows within the shadow distance: the caster's box swept along
		// the light, as far as the visible receivers' bounds reach (m_VisibleReceivers, clipped to the shadowed part of the
		// view), is not separated from the view by any of `planes`. Conservative: it never rejects a shadow that shows.
		bool CanShadowView(const AABB& caster, const glm::vec3& lightDirection, const AABB& receivers, std::span<const glm::vec4> planes) const;
		// Resolves every mesh and material the frame uses, once each, at the priority its use asks for.
		void ResolveAssets(RenderScene& renderScene);
		// The materials of m_UsedMaterialSlots, one by one, recorded in m_MaterialResolution for later frames.
		void ResolveMaterials(RenderScene& renderScene);
		// Takes the materials' flags and texture maps over from m_MaterialResolution when the frame uses the same material
		// slots the same ways and nothing it depends on changed (see MaterialResolution). Returns false, having resolved
		// nothing, otherwise.
		bool ReuseResolvedMaterials(const RenderScene& renderScene);
		// The view's draws in batches: opaque ones sorted by pipeline state and mesh (only with `opaqueDraws`: the GPU-driven
		// path draws them itself), transparent ones back to front; and what the drawn materials need of their textures.
		void GatherDraws(RenderScene& renderScene, const SceneCamera& camera, bool opaqueDraws);
		// The cascades' fit (m_CascadeFit: to the drawn casters the CPU found, or with `casterDraws` false to those the GPU's
		// culling finds) and with `casterDraws` the casters' draws per cascade.
		void GatherShadowDraws(RenderScene& renderScene, const SceneCamera& camera, RenderData::FrameConstants& frame, bool casterDraws);
		// The selection mask's draws (m_SelectionBatches): the selected entities' opaque and alpha-masked submeshes the frame
		// resolved, at the level of detail the view chooses, built on the CPU on both paths as transparent surfaces are.
		void GatherSelectionDraws(RenderScene& renderScene, const SceneCamera& camera, Scene& scene, std::span<const Entity> selection);
		// Records Scene/CascadeFit.comp: the cascades' projections into m_CascadeBuffer, before anything draws or samples them.
		void FitCascades(nvrhi::ICommandList* commandList);
		void MarkMesh(uint32_t slot, uint32_t usage);
		void MarkMaterial(uint32_t slot, uint32_t usage);
		// Sizes the per-material-slot state for `count` slots.
		void EnsureMaterialSlots(uint32_t count);
		// Resolves a material's texture maps for the frame (requested, or with residentOnly only looked up), appending the
		// loaded ones to `resolved` when it is given (the materials the frame draws, for streaming their levels); returns how
		// many it appended. The material's GPU data is the render scene's GPU copy's.
		uint32_t ResolveMaterialTextures(const Material& material, bool residentOnly, std::vector<ResolvedTexture>* resolved);
		// Sorted draws into batches and the draw list (instance indices in batch order).
		void BuildBatches(std::vector<SortItem>& draws, std::vector<SceneDrawBatch>& outBatches, std::vector<uint32_t>& outDrawList);
		// Uploads what changed of the draw lists (the render scene's GPU copy uploads the instances, palettes and material data).
		void UploadSceneData(nvrhi::ICommandList* commandList, bool drawListsRecreated);
		// Tells the scene's ISkinningPoses which animators' meshes the frame drew (or would have, unposed).
		void ReportVisibleAnimators(Scene& scene);
		// Uploads the range of a draw list that differs from the list the buffer holds (`uploaded`), which then holds the new one.
		uint64_t UploadDrawList(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, std::vector<uint32_t>& list, std::vector<uint32_t>& uploaded, bool full);
		// The same for any list of plain records (compared bytewise).
		template<typename T>
		uint64_t UploadChangedRange(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, std::vector<T>& list, std::vector<T>& uploaded, bool full);

		bool EnsureShadowMap();
		// Local light shadows (SceneRendererLocalShadows.cpp). Chooses the shadowed point and spot lights of the frame and their
		// tiles, follows what changed what their views hold, chooses the views the frame draws (m_LocalShadowViews) and finds
		// their casters: with `exactCasters` each view's (the CPU path; their meshes and materials marked with `markCasters`),
		// else only what bounds the GPU's culling of them.
		void FindLocalShadowCasters(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum, bool exactCasters, bool markCasters);
		// The CPU path's draws of the views (m_LocalShadowBatches), after the frame's assets are resolved (none with
		// `casterDraws` false: the GPU's culling draws them).
		void GatherLocalShadowDraws(RenderScene& renderScene, bool casterDraws);
		// The shaders' data of the shadowed lights (m_LocalShadowData), relative to the frame's camera.
		void BuildLocalShadowData(const SceneCamera& camera);
		// Creates the atlas with its framebuffer once a frame has shadowed lights; false (reported) when that fails.
		bool EnsureLocalShadowAtlas();
		// Clears the views' tiles and draws their casters (the CPU's draws).
		void DrawLocalShadows(nvrhi::ICommandList* commandList);
		// The GPU-driven path's: the views in batches of RenderData::c_CullMaxLocalShadowViews, the first culled with the
		// cascades (RunGPUCulling), each later one culled after the previous one's draws (RunLocalShadowCulling).
		void DrawGPULocalShadows(nvrhi::ICommandList* commandList);
		void RunLocalShadowCulling(nvrhi::ICommandList* commandList, uint32_t firstView, uint32_t viewCount, bool lastBatch);
		// The culling's mode bits of a batch of local shadow views (Scene/Cull.comp).
		static uint32_t GetLocalShadowCullMode(uint32_t firstView, uint32_t viewCount);
		// Clears a view's tile (a quad at depth 0: the atlas's other tiles keep their shadows).
		void ClearLocalShadowTile(nvrhi::ICommandList* commandList, const ShadowAtlasTile& tile);
		// After the frame's command list was executed: its views hold what their keys describe.
		void CommitLocalShadows();
		// Forgets every shadowed light and what the atlas holds (another render scene, released targets).
		void ResetLocalShadows();
		void FreeLocalShadowTiles(LocalShadowLight& light);
		// A view's matrix from camera-relative world space to its clip space (reversed-Z), relative to `origin` (in double).
		static glm::mat4 GetLocalShadowViewProjection(const LocalShadowViewKey& key, const glm::dvec3& origin);
		// The level drawn of a submesh of a mesh at a screen fraction, by the frame's m_LODCriteria (LODSelection::SelectLOD).
		uint32_t SelectLOD(const Mesh& mesh, const Submesh& submesh, float screenFraction) const;
		static float GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius);
		void DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent);
		void DrawShadows(nvrhi::ICommandList* commandList, uint32_t cascadeCount);
		// Whether batches are drawn by one indirect call per bucket (SceneRendererSpecification::IndirectMode and the device).
		bool UsesIndirectDraws() const;
		// The indirect arguments of the frame's batches (m_DrawArgumentsBuild): opaque, transparent, then shadow batches.
		void BuildDrawArguments();
		// Draws batches in buckets: runs of batches with one pipeline state, vertex arena and cascade share one state (the
		// geometry pool's buffers) and one indirect call (or a direct draw per batch). `firstArgument` is the first batch's
		// entry in the argument buffer. `prepareState` completes a bucket's state (its framebuffer), `afterState` follows it
		// (push constants).
		void DrawBuckets(nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, std::span<const SceneDrawBatch> batches, uint32_t firstArgument,
			const std::function<PipelineKey(const SceneDrawBatch&)>& getKey, const std::function<void(const SceneDrawBatch&, nvrhi::GraphicsState&)>& prepareState,
			const std::function<void(const SceneDrawBatch&)>& afterState);
		void BuildLightClusters(nvrhi::ICommandList* commandList);

		// The GPU-driven path (SceneRendererGPUDriven.cpp; SceneRendererSpecification::GPUDriven).
		enum class GPUIndirectPath : uint8_t
		{
			Count = 0, // drawIndexedIndirectCount per bucket
			NoCount,   // drawIndexedIndirect over a bucket's draw slots
			PerDraw    // drawIndexedIndirect per draw slot
		};
		// How the frame's GPU draws are issued, for buckets of `argumentsPerBucket` draw slots: a count the GPU writes only while
		// no bucket can hold more draws than one call may issue.
		GPUIndirectPath GetGPUIndirectPath(uint32_t argumentsPerBucket) const;
		GPUIndirectPath GetGPUIndirectPath() const { return GetGPUIndirectPath(m_CullConstants.ArgumentsPerBucket); }
		// The most draws one indirect call issues (SceneRendererSpecification::MaxDrawsPerIndirectCall and the device).
		uint32_t GetMaxDrawsPerIndirectCall() const;
		// The culling pipelines and the buffers of fixed size; leaves the path unavailable (logged) when they fail.
		void CreateCullResources();
		// Lets go of everything CreateCullResources made (the path is unavailable then).
		void ReleaseCullResources();
		// The frame's culling inputs: the mesh and material tables, the buckets the frame can have and their draw slots, and
		// the constants (after the extraction resolved the frame's assets and the cascades).
		void PrepareGPUCulling(const RenderScene& renderScene, const SceneCamera& camera);
		// The camera's view without its translation: what scene shaders transform camera-relative positions with.
		static glm::mat4 GetRelativeView(const SceneCamera& camera);
		// Sizes the culling buffers for the frame; `outRecreated` when one was replaced (the binding sets follow).
		bool EnsureCullBuffers(bool& outRecreated);
		bool CreateCullBindingSet();
		// Uploads the tables and constants that changed, and the geometry pool's tables.
		bool UploadCullTables(nvrhi::ICommandList* commandList);
		// Counts, builds the draws and writes the draw list (Scene/Cull.comp, CullBuild.comp) of the cascades and of the view's
		// first phase (all of the view without occlusion culling).
		void RunGPUCulling(nvrhi::ICommandList* commandList, bool shadows);
		// The GPU-driven path's point and spot lights: culls the scene's light records into the light buffer (every light in
		// view, in record order) and its count (Scene/LightCull.comp, LightCompact.comp), before the light clusters.
		void CreateLightCullResources();
		bool EnsureLightCullBindingSets();
		void RunGPULightCulling(nvrhi::ICommandList* commandList, const SceneCamera& camera);
		// Writes what changed of the light records' shadow slots (the frame's shadowed lights) into their table.
		void UploadLightShadowSlots(nvrhi::ICommandList* commandList);
		// After the first phase's prepass: the depth pyramid, the view's second phase (what the first did not draw that is
		// visible against it) and its prepass draws; then copies the counters for the statistics.
		void RunOcclusionPhase(nvrhi::ICommandList* commandList);
		// The counters' (and the feedback's) copy into the frame's readback.
		void RecordCullReadback(nvrhi::ICommandList* commandList);
		// Makes each culling pass see the previous one's writes.
		void CullBarrier(nvrhi::ICommandList* commandList);
		// Sizes the visibility buffer for the proxies; a new one starts without history.
		bool EnsureVisibilityBuffer(bool& outRecreated);
		bool EnsureLODFadeBuffer(bool& outRecreated);
		// After the command list with the culling was executed: the counters' copy is under way.
		void SubmitCullReadback();
		// The view's dense buckets [first, end) in the pass.
		void DrawGPUView(nvrhi::ICommandList* commandList, Pass pass, uint32_t firstDense, uint32_t endDense);
		// Whether the frame's view is occlusion culled (SceneRendererSpecification::OcclusionCulling, with a depth pyramid).
		bool IsOcclusionCulled() const;
		bool CreateDepthPyramid();
		// The cascades' draws: the first phase, and with occlusion culling the cascades' pyramids, second phase and its draws.
		void DrawGPUShadows(nvrhi::ICommandList* commandList);
		// The cascades' dense buckets [first, end).
		void DrawGPUShadowBuckets(nvrhi::ICommandList* commandList, uint32_t firstDense, uint32_t endDense);
		// Creates the cascades' depth pyramids once the shadow map exists (GPU-driven renderers); false when that failed.
		bool EnsureShadowPyramid();
		void RunShadowOcclusionPhase(nvrhi::ICommandList* commandList);
		// The cascades the frame keeps from the previous one (SceneRendererSpecification::CacheShadowCascades), after the frame's
		// uploads; the cache holds nothing until CommitShadowCache, once the frame's command list was executed.
		uint32_t PrepareShadowCache(const RenderScene& renderScene, const SceneCamera& camera);
		void CommitShadowCache();
		// The cascades (a bit each) a world-space box reaches, as GPU culling places casters in them, with a margin.
		uint32_t GetCascadesReached(const AABB& worldBounds, const glm::vec3& cameraPosition) const;
		// The indirect draws of a dense bucket, as GetGPUIndirectPath says: its `slots` (without a GPU-written count), or as many
		// as the GPU counted.
		void DrawGPUBucket(nvrhi::ICommandList* commandList, uint32_t dense, uint32_t slots);
		// The draw slots of a bucket (m_CullViewSlots, m_CullShadowSlots).
		uint32_t GetBucketSlots(uint32_t bucket) const;
		// Whether the frame draws the mesh slot in its view (requested at Normal or High priority), or casts shadows with it.
		bool IsMeshInView(uint32_t slot) const;
		bool IsMeshCaster(uint32_t slot) const;
		// Reads the culling counters of completed frames into m_GPUStats (with `wait`, the latest frame's), and their visibility
		// feedback into m_Feedback.
		void ResolveCullCounters(bool wait);
		void CreateOverlayResources();
		static bool HasOverlays(const SceneRenderOptions& options);
		// Draws the selection mask (m_SelectionBatches) of a frame with temporal anti-aliasing; false (reported) when its target
		// cannot be created.
		bool DrawSelectionMask(nvrhi::ICommandList* commandList);
		// Draws the overlays into the output texture (which must hold the finished image); false when GPU resources failed.
		bool RenderOverlays(nvrhi::ICommandList* commandList, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, bool drawText,
			bool drawOverlayPasses);
		// Prepare of every overlay pass (SceneOverlayRegistry); whether any draws.
		bool PrepareOverlayPasses(Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, nvrhi::ICommandList* commandList,
			GPUPassTimer& gpuTimer);
		void DrawDebugLines(nvrhi::ICommandList* commandList, DebugDrawDepth depth, uint32_t firstVertex, uint32_t vertexCount);
		// The motion debug view (SceneRenderOptions::ShowMotionVectors) into the output texture; false when GPU resources failed.
		bool DrawMotionView(nvrhi::ICommandList* commandList, const nvrhi::Viewport& viewport);
		// The tints of SceneRenderOptions::EntityTints (StatusTint.frag); false when GPU resources failed.
		bool DrawEntityTints(nvrhi::ICommandList* commandList, Scene& scene, std::span<const SceneEntityTint> tints, const nvrhi::Viewport& viewport);
	private:
		SceneRendererSpecification m_Specification;
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::CommandListHandle m_CommandList;
		glm::uvec2 m_ViewportSize = { 0, 0 };
		LODCriteria m_LODCriteria; // The frame's (GetLODCriteria, set as it starts)
		// Level-of-detail cross-fades: the frame's number (LODSelection::UpdateLODFade), which skips one at camera cuts and
		// scene changes so that they start without history, and the CPU path's state per instance (the GPU-driven path's are
		// m_LODFadeBuffer).
		uint32_t m_LODFadeFrame = 0;
		std::vector<RenderData::LODFadeState> m_LODFades;
		bool m_TargetsValid = false;
		bool m_HasRenderedFrame = false; // Since the render targets were (re)created
		// The camera of the previous frame, to tell the texture streamer when the view jumped (a camera cut).
		bool m_HasPreviousCamera = false;
		glm::vec3 m_PreviousCameraPosition = glm::vec3(0.0f);
		glm::vec3 m_PreviousCameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
		ErrorThrottle m_Errors; // Rendering failures, logged once until a frame renders successfully again
		bool m_WarnedLightOverflow = false;
		SceneRendererStats m_Stats;
		SceneRendererGPUStats m_GPUStats;
		// The GPU passes this renderer timed in its latest frames (one record per GPUProfiler frame slot), newest at
		// m_GPURecordIndex, read once their timestamps are written (ResolveGPUStats).
		static constexpr uint32_t c_GPURecordCount = 6;
		std::array<GPUTimerRecord, c_GPURecordCount> m_GPURecords;
		uint32_t m_GPURecordIndex = 0;
		uint64_t m_ResolvedGPURecord = 0; // The slot of the record m_GPUStats came from
		std::vector<GPUStageTime> m_GPUStageScratch;

		nvrhi::ShaderHandle m_MeshVertexShader;
		nvrhi::ShaderHandle m_SkinnedMeshVertexShader;
		nvrhi::ShaderHandle m_SkinnedShadowVertexShader;
		nvrhi::InputLayoutHandle m_SkinnedInputLayout; // m_InputLayout and the skin stream (joints, weights)
		nvrhi::ShaderHandle m_PrepassPixelShader;
		nvrhi::ShaderHandle m_ForwardPixelShader;
		nvrhi::ShaderHandle m_ForwardTransparentPixelShader;
		nvrhi::ShaderHandle m_FullscreenVertexShader;
		nvrhi::ShaderHandle m_TonemapPixelShader;
		nvrhi::ShaderHandle m_SkyboxPixelShader;
		nvrhi::ShaderHandle m_AtmosphereSkyboxPixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::GraphicsPipelineHandle m_SkyboxPipeline;
		nvrhi::GraphicsPipelineHandle m_AtmosphereSkyboxPipeline;

		// Image-based lighting
		Ref<const EnvironmentMaps> m_Environment; // Null without an environment; a map's is shared (FindSharedEnvironment)
		Ref<TextureHold> m_EnvironmentHold; // While the environment map's environment is still to be computed (HoldEnvironmentMap)
		nvrhi::ITexture* m_BoundEnvironment = nullptr; // Environment cube referenced by m_SceneBindingSet
		Scope<AtmosphereState> m_Atmosphere; // While the sky is the atmosphere
		bool m_AtmosphereFrame = false;      // The frame's sky is the atmosphere
		float m_AtmosphereSunIlluminance = 0.0f; // Its sun's illuminance above it: the largest component of the light's color times intensity
		nvrhi::ITexture* m_BoundSkyView = nullptr; // The sky-view table (or a black texture) referenced by m_SceneBindingSet
		nvrhi::ITexture* m_BoundReflectionProbes = nullptr; // The probes' cube array (or the black one) referenced by m_SceneBindingSet
		nvrhi::BindingLayoutHandle m_IBLBindingLayout;
		nvrhi::ComputePipelineHandle m_EquirectToCubePipeline;
		nvrhi::BindingLayoutHandle m_ProceduralSkyBindingLayout;
		nvrhi::ComputePipelineHandle m_ProceduralSkyPipeline;
		nvrhi::ComputePipelineHandle m_CubeDownsamplePipeline;
		nvrhi::ComputePipelineHandle m_IrradiancePipeline;
		nvrhi::ComputePipelineHandle m_PrefilterPipeline;
		nvrhi::TextureHandle m_BRDFLut;
		nvrhi::SamplerHandle m_LinearClampSampler;
		nvrhi::SamplerHandle m_EquirectSampler;

		// Directional light and shadows
		ShadowLight m_ShadowLight;
		// Color of the directional light the scene is lit by (the procedural sun's tint; the atmosphere's sun color)
		glm::vec3 m_SunColor = glm::vec3(1.0f);
		// The frame's procedural sky's horizon radiance (its HorizonColor times its intensity), which lights height fog; unset
		// when no procedural sky lights the scene (UpdateEnvironment).
		std::optional<glm::vec3> m_SkyHorizon;
		nvrhi::TextureHandle m_ShadowMap;      // Depth array, one layer per cascade (created on first use)
		nvrhi::TextureHandle m_DummyShadowMap; // Bound while no shadows are rendered
		nvrhi::ITexture* m_BoundShadowMap = nullptr;
		std::vector<nvrhi::FramebufferHandle> m_ShadowFramebuffers;
		nvrhi::BufferHandle m_ShadowDrawListBuffer; // Instance indices of the shadow draws
		nvrhi::BindingSetHandle m_ShadowBindingSet; // Scene bindings with the shadow draw list
		nvrhi::BindingLayoutHandle m_ShadowPushLayout;
		nvrhi::BindingSetHandle m_ShadowPushBindingSet;
		nvrhi::SamplerHandle m_ShadowCompareSampler;
		nvrhi::ShaderHandle m_ShadowVertexShader;
		nvrhi::ShaderHandle m_ShadowMaskPixelShader;
		std::vector<SceneDrawBatch> m_ShadowBatches;
		// Local light shadows (SceneRendererSpecification::LocalShadowAtlasSize): the atlas and what hands out its tiles, the
		// shadowed lights of the frame (in order of relevance: their shadow slots), the views it draws with their casters and
		// batches, the shaders' data per slot, and how the views' content is followed (the render scene, the content they were
		// drawn with, the proxy log's version read, and the bounds of the proxies within their lights' ranges when they were
		// drawn, whose changes redraw what they reach; complete for views drawn in m_LocalShadowEpoch).
		ShadowAtlasAllocator m_LocalShadowAllocator;
		nvrhi::TextureHandle m_LocalShadowAtlas;
		nvrhi::FramebufferHandle m_LocalShadowFramebuffer;
		nvrhi::TextureHandle m_DummyLocalShadowAtlas; // Bound while there is no atlas
		nvrhi::ITexture* m_BoundLocalShadowAtlas = nullptr;
		bool m_LocalShadowAtlasCleared = false;
		nvrhi::BufferHandle m_LocalShadowBuffer;      // RenderData::LocalShadowData per slot
		nvrhi::BufferHandle m_ReflectionProbeBuffer;  // RenderData::ReflectionProbeBuffer
		// Reflection probes: the cube array and its slots (null without probes), the conversion's layout and pipeline (made with
		// the first probe), the holds that keep captures whole until converted (by capture), the frame's list and conversions,
		// and how many probes the latest upload listed.
		std::optional<ReflectionProbeState> m_ReflectionProbes;
		nvrhi::BindingLayoutHandle m_ProbeIrradianceLayout;
		nvrhi::ComputePipelineHandle m_ProbeIrradiancePipeline;
		std::unordered_map<AssetHandle, Ref<TextureHold>> m_ReflectionProbeHolds;
		std::vector<RenderData::ReflectionProbeData> m_ReflectionProbeList;
		std::vector<ReflectionProbeConversion> m_ReflectionProbeConversions;
		uint32_t m_UploadedReflectionProbes = 0;
		nvrhi::GraphicsPipelineHandle m_LocalShadowClearPipeline;
		nvrhi::ShaderHandle m_LocalShadowVertexShader;
		nvrhi::ShaderHandle m_SkinnedLocalShadowVertexShader;
		std::vector<LocalShadowLight> m_LocalShadowLights;
		std::vector<LocalShadowLight> m_LocalShadowScratch;
		std::vector<LocalShadowDrawView> m_LocalShadowViews;
		std::vector<LocalShadowCaster> m_LocalShadowCasters;
		std::vector<uint32_t> m_LocalShadowCandidates;
		std::vector<SceneDrawBatch> m_LocalShadowBatches;
		std::vector<RenderData::LocalShadowData> m_LocalShadowData;
		uint64_t m_LocalShadowSceneId = 0;
		LocalShadowContent m_LocalShadowContent;
		uint64_t m_LocalShadowProxyVersion = 0;
		uint64_t m_LocalShadowEpoch = 1;
		uint64_t m_LocalShadowDrawEpoch = 0; // The epoch of the frame's drawn views: 0 when their proxies could not all be followed
		std::unordered_map<uint32_t, AABB> m_LocalShadowBounds;
		std::vector<uint32_t> m_LocalShadowChanges;
		std::array<CascadeBox, RenderData::c_MaxCascades> m_Cascades = {};
		glm::mat4 m_LightView = glm::mat4(1.0f);
		uint32_t m_CascadeCount = 0;

		// Clustered lighting
		nvrhi::BindingLayoutHandle m_ClusterBindingLayout;
		nvrhi::BindingSetHandle m_ClusterBindingSet;
		nvrhi::ComputePipelineHandle m_ClusterPipeline;
		nvrhi::BufferHandle m_LightBoundsBuffer; // View-space bounding spheres, parallel to m_LightBuffer
		nvrhi::BufferHandle m_ClusterBuffer;     // Per cluster: light count, then light indices (Include/Clusters.glsl)

		nvrhi::BindingLayoutHandle m_SceneBindingLayout;
		nvrhi::BindingLayoutHandle m_TonemapBindingLayout;
		nvrhi::BindingSetHandle m_SceneBindingSet;
		nvrhi::BindingSetHandle m_TonemapBindingSet;
		std::vector<nvrhi::SamplerHandle> m_MaterialSamplers;
		nvrhi::SamplerHandle m_PointClampSampler;

		nvrhi::BufferHandle m_FrameConstantBuffer;
		nvrhi::BufferHandle m_DrawListBuffer; // Instance indices of the view's draws
		// The render scene's GPU copy (instances, palettes, culling records, mesh and material tables) during a frame, and the
		// buffer version
		// (RenderSceneGPU::GetBufferVersion) the binding sets were made for.
		RenderSceneGPU* m_SceneGPU = nullptr;
		uint64_t m_BoundSceneGPUVersion = 0;
		uint64_t m_BoundTextureTableVersion = 0; // TextureTable::GetBufferVersion the binding sets were made for
		nvrhi::BufferHandle m_LightBuffer;
		nvrhi::BufferHandle m_LightCountBuffer; // The lights in m_LightBuffer where the GPU culled them (LightCounts.y)
		// The shadow cascades' projections (RenderData::CascadeData), fitted on the GPU every frame with shadows
		// (Scene/CascadeFit.comp) from m_CascadeFit and, on the GPU-driven path, the nearest casters its culling found.
		nvrhi::BufferHandle m_CascadeBuffer;
		nvrhi::BufferHandle m_CasterDepthBuffer; // Per cascade: the nearest caster's light view z (Cull.comp, encoded)
		nvrhi::BufferHandle m_CascadeFitConstantBuffer;
		nvrhi::BindingLayoutHandle m_CascadeFitBindingLayout;
		nvrhi::BindingSetHandle m_CascadeFitBindingSet;
		nvrhi::ComputePipelineHandle m_CascadeFitPipeline;
		RenderData::CascadeFitConstants m_CascadeFit;
		glm::dvec3 m_CameraLightPosition = glm::dvec3(0.0); // The camera's position in light view space (FindShadowCasters)
		std::vector<DebugLineVertex> m_DebugLineScratch; // The frame's debug lines, camera-relative

		nvrhi::TextureHandle m_DepthTexture;
		nvrhi::TextureHandle m_NormalTexture;
		nvrhi::TextureHandle m_EntityIDTexture;
		nvrhi::TextureHandle m_HDRTexture;
		nvrhi::TextureHandle m_OutputTexture;
		nvrhi::TextureHandle m_AORawTexture; // Noisy GTAO output
		nvrhi::TextureHandle m_AOTexture;    // Blurred
		nvrhi::Format m_AOFormat = nvrhi::Format::R32_FLOAT; // Of both: R16_FLOAT where the device stores it (CreateResources)
		nvrhi::BindingLayoutHandle m_AOBindingLayout;
		nvrhi::BindingSetHandle m_GTAOBindingSet;
		nvrhi::BindingSetHandle m_AOBlurBindingSet;
		nvrhi::ComputePipelineHandle m_GTAOPipeline;
		nvrhi::ComputePipelineHandle m_AOBlurPipeline;
		nvrhi::FramebufferHandle m_PrepassFramebuffer;
		nvrhi::FramebufferHandle m_ForwardFramebuffer;
		nvrhi::FramebufferHandle m_OutputFramebuffer;

		// Post-processing
		nvrhi::BindingLayoutHandle m_ExposureBindingLayout;
		nvrhi::BindingSetHandle m_ExposureBindingSet;
		nvrhi::ComputePipelineHandle m_HistogramPipeline;
		nvrhi::ComputePipelineHandle m_ExposurePipeline;
		nvrhi::BufferHandle m_HistogramBuffer;
		nvrhi::BufferHandle m_ExposureBuffer; // See Include/Exposure.glsl
		bool m_ResetExposure = true;
		// The pre-exposure (SceneRendererStats::PreExposure): the value frames are drawn with; whether it settled; a reset asked
		// for; the readbacks of the exposure buffer after the automatic exposure of a frame (its RenderCount + 1, the epoch and
		// pre-exposure it was drawn with: readbacks of an earlier epoch, before a reset or a stretch of frames without automatic
		// exposure, are ignored), in turn, and the newest frame whose readback was read.
		float m_PreExposure = 1.0f;
		bool m_PreExposureSettled = false;
		bool m_ResetPreExposure = false;
		bool m_PreExposureAutomatic = false; // The latest frame's came from automatic exposure
		uint64_t m_PreExposureEpoch = 1;
		struct ExposureReadback
		{
			Scope<BufferReadback> Readback;
			uint64_t Render = 0;
			uint64_t Epoch = 0;
			float PreExposure = 1.0f;
		};
		std::array<ExposureReadback, c_GPURecordCount> m_ExposureReadbacks;
		uint32_t m_ExposureReadbackIndex = 0;
		bool m_ExposureReadbackRecorded = false; // The frame being recorded copies the exposure (SubmitExposureReadback)
		uint64_t m_ExposureReadbackSeen = 0;
		float m_HistoryPreExposure = 1.0f; // The pre-exposure of the temporal history's newest image
		// Temporal anti-aliasing (SceneRendererSpecification::TemporalAntiAliasing): the frame being rendered uses it; the frames
		// the history holds (0: none); the frame that resolved it last (RenderCount + 1; 0: none, or not since a frame failed),
		// the device frame, render scene and jitter (clip units) of that frame; a reset asked for, and the resets so far.
		bool m_TemporalFrame = false;
		uint32_t m_TemporalFrames = 0;
		uint64_t m_TemporalRender = 0;
		uint64_t m_TemporalDeviceFrame = 0;
		uint64_t m_TemporalSceneId = 0;
		glm::vec2 m_TemporalJitterClip = glm::vec2(0.0f);
		bool m_ResetHistory = false;
		uint32_t m_HistoryResets = 0;
		// The history's two textures, used in turn: the newest image is m_HistoryIndex's, whose binding sets of the exposure, the
		// bloom's first downsample and the tone mapping read it; the resolve's set of an index reads its texture and writes the
		// other one. Whether the latest frame's post-processing read the resolved image.
		std::array<nvrhi::TextureHandle, 2> m_HistoryTextures;
		uint32_t m_HistoryIndex = 0;
		bool m_TemporalResolved = false;
		nvrhi::BindingLayoutHandle m_TemporalBindingLayout;
		nvrhi::ComputePipelineHandle m_TemporalPipeline;
		std::array<nvrhi::BindingSetHandle, 2> m_TemporalResolveSets;
		std::array<nvrhi::BindingSetHandle, 2> m_TemporalExposureSets;
		std::array<nvrhi::BindingSetHandle, 2> m_TemporalBloomSets;
		std::array<nvrhi::BindingSetHandle, 2> m_TemporalTonemapSets;
		std::optional<std::chrono::steady_clock::time_point> m_LastExposureTime;
		nvrhi::BindingLayoutHandle m_BloomBindingLayout;
		nvrhi::ComputePipelineHandle m_BloomPipeline;
		nvrhi::TextureHandle m_BloomTexture;
		uint32_t m_BloomLevels = 0;
		std::vector<nvrhi::BindingSetHandle> m_BloomDownsampleSets; // [0]: scene color -> level 0, [n]: level n-1 -> n
		std::vector<nvrhi::BindingSetHandle> m_BloomUpsampleSets;   // [n]: level n+1 -> n
		nvrhi::ShaderHandle m_FXAAPixelShader;
		nvrhi::BindingLayoutHandle m_FXAABindingLayout;
		nvrhi::BindingSetHandle m_FXAABindingSet;
		nvrhi::TextureHandle m_LDRTexture; // Tone-mapped image before anti-aliasing
		nvrhi::FramebufferHandle m_LDRFramebuffer;

		std::vector<std::pair<PipelineKey, nvrhi::GraphicsPipelineHandle>> m_ScenePipelines;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_TonemapPipelines;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_FXAAPipelines;

		// Overlays (SceneRenderOptions)
		nvrhi::ShaderHandle m_GridPixelShader;
		nvrhi::ShaderHandle m_OutlinePixelShader;
		nvrhi::ShaderHandle m_DebugLineVertexShader;
		nvrhi::ShaderHandle m_DebugLinePixelShader;
		nvrhi::ShaderHandle m_CopyPixelShader;
		nvrhi::BindingLayoutHandle m_GridBindingLayout;
		nvrhi::BindingSetHandle m_GridBindingSet;
		nvrhi::GraphicsPipelineHandle m_GridPipeline;
		nvrhi::BindingLayoutHandle m_OutlineBindingLayout;
		nvrhi::BindingSetHandle m_OutlineBindingSet; // Entity IDs and selection; recreated with either
		nvrhi::GraphicsPipelineHandle m_OutlinePipeline;
		nvrhi::BufferHandle m_SelectionBuffer;
		// The selection the buffer holds: kept while the same entities of the same scene stay selected and the scene's
		// hierarchy does not change (entities created or destroyed).
		std::vector<uint32_t> m_SelectionIDs;
		std::vector<Entity> m_SelectionEntities;
		uint64_t m_SelectionSceneId = 0;
		uint64_t m_SelectionHierarchyVersion = 0;
		bool m_SelectionUploaded = false;
		// The selection outline of frames with temporal anti-aliasing: the selected instances drawn again with the camera's own
		// matrices into an R8 mask (made with the first such outline, released with the targets), whose outline reads it.
		std::vector<SceneDrawBatch> m_SelectionBatches; // After the shadow batches in the draw arguments
		std::vector<uint32_t> m_SelectionProxies;
		nvrhi::ShaderHandle m_SelectionMaskPixelShader;
		nvrhi::ShaderHandle m_OutlineMaskPixelShader;
		nvrhi::TextureHandle m_SelectionMaskTexture;
		nvrhi::FramebufferHandle m_SelectionMaskFramebuffer; // The mask and the scene depth (read-only)
		nvrhi::BindingSetHandle m_OutlineMaskBindingSet;     // The mask and the selection buffer
		nvrhi::GraphicsPipelineHandle m_OutlineMaskPipeline;
		nvrhi::ShaderHandle m_StatusTintPixelShader;
		nvrhi::BindingLayoutHandle m_StatusTintBindingLayout;
		nvrhi::BindingSetHandle m_StatusTintBindingSet; // Entity IDs and tints; recreated with either
		nvrhi::GraphicsPipelineHandle m_StatusTintPipeline;
		nvrhi::BufferHandle m_TintBuffer;
		std::vector<glm::uvec2> m_TintEntries; // (entity id + 1, packed color), sorted by id
		nvrhi::BindingLayoutHandle m_DebugLineBindingLayout;
		nvrhi::BindingSetHandle m_DebugLineBindingSet;
		nvrhi::InputLayoutHandle m_DebugLineInputLayout;
		std::array<nvrhi::GraphicsPipelineHandle, 2> m_DebugLinePipelines; // Indexed by DebugDrawDepth
		nvrhi::GraphicsPipelineHandle m_JitteredDebugLinePipeline; // Tested against a jittered depth (SetJitteredDepthBias)
		nvrhi::BufferHandle m_DebugLineBuffer;
		nvrhi::FramebufferHandle m_OverlayFramebuffer; // Output texture with the scene depth (read-only)
		nvrhi::BindingSetHandle m_CopyBindingSet;      // Output texture, copied into external targets
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_CopyPipelines;
		Scope<FontAtlasCache> m_FontAtlases; // Glyph atlases of the text and the overlay passes (one per font)
		Scope<TextRenderer> m_TextRenderer; // TextComponents, drawn with the overlays
		std::vector<Scope<ISceneOverlayPass>> m_OverlayPasses; // One per SceneOverlayRegistry factory, drawn after the text

		// Lights gathered per frame (kept between frames to reuse the allocations)
		std::vector<RenderData::LightData> m_Lights;
		std::vector<RenderData::LightBounds> m_LightBounds;
		std::vector<uint32_t> m_LightRecordIndices; // Per entry of m_Lights: its RenderLights record
		std::vector<LightCandidate> m_LightCandidates;

		// Extraction (see SceneRendererExtraction.cpp). Slots are those of the render scene the state belongs to.
		AssetResolveCache m_Assets;     // Holds the frame's meshes, materials and textures while the frame is rendered
		uint64_t m_RenderSceneId = 0;   // The render scene the per-slot state and the GPU copies below belong to
		uint64_t m_ProxyUpdatesScene = 0; // The render scene whose RenderSceneStats::TotalUpdates this renderer saw last
		uint64_t m_ProxyUpdatesSeen = 0;
		bool m_FullUpload = true;       // The draw lists hold nothing of that render scene yet
		std::vector<entt::entity> m_VisibleAnimators; // Animators of the frame's visible Skin proxies, reported to ISkinningPoses
		AABB m_VisibleReceivers; // The world bounds of the frame's visible proxies (scenes with Skin proxies only)
		uint32_t m_FrameStamp = 0;
		std::vector<uint32_t> m_ProxyStamps;        // Per proxy: m_FrameStamp when it is in the view this frame
		std::vector<uint32_t> m_CasterStamps;       // Per proxy: m_FrameStamp when it is a shadow caster candidate
		std::vector<uint32_t> m_VisibleProxies;
		std::vector<float> m_VisibleFractions;      // Screen fraction of each visible proxy
		std::vector<uint32_t> m_QueryScratch;
		std::array<std::vector<uint32_t>, RenderData::c_MaxCascades> m_CascadeQueries; // Per cascade: what its query found
		std::vector<uint32_t> m_CasterCandidates;   // The cascades' query results, each proxy once, in cascade order
		std::vector<ShadowCaster> m_ShadowCasters;
		std::vector<uint32_t> m_MeshUsage;          // Per mesh slot: how the frame uses it (usage bits)
		std::vector<uint32_t> m_UsedMeshSlots;
		std::vector<const Mesh*> m_FrameMeshes;     // Per mesh slot: the loaded mesh uploaded to the geometry pool, while the frame holds it
		std::vector<GeometryAllocation> m_FrameGeometry; // Per mesh slot: where m_FrameMeshes' geometry is this frame
		std::vector<uint32_t> m_MaterialUsage;      // Per material slot
		std::vector<uint32_t> m_UsedMaterialSlots;
		std::vector<uint32_t> m_MaterialFlags;      // Per material slot: how the frame draws it (material flags)
		std::vector<float> m_MaterialPixelSizes;    // Per material slot: the frame's VisibleMaterialUse::PixelSize
		std::vector<float> m_MaterialUVPixels;      // Per material slot: the frame's VisibleMaterialUse::UVPixels
		std::vector<float> m_MaterialUVScales;
		std::vector<ResolvedTexture> m_ResolvedTextures;          // The loaded texture maps of the materials the frame draws (kept when it reuses them)
		std::vector<ResolvedTextureRange> m_MaterialTextures;     // Per material slot: its range of m_ResolvedTextures this frame
		MaterialResolution m_MaterialResolution;                 // The latest frame that resolved its materials one by one
		std::vector<VisibleMaterialUse> m_VisibleMaterials;
		std::vector<VisibleTextureUse> m_VisibleTextures;
		std::optional<uint32_t> m_DefaultMaterialFlags; // The default material's flags, resolved once per frame (0: not loaded)
		std::vector<ExtractionChunk> m_Chunks;
		std::vector<SortItem> m_Draws;
		std::vector<SortItem> m_ShadowDraws;
		std::vector<SortItem> m_SortScratch;
		std::vector<SortItem> m_SlotSortItems; // The used slots' sort (ResolveAssets)
		std::vector<TransparentDraw> m_Transparent;
		std::vector<SceneDrawBatch> m_OpaqueBatches;
		std::vector<SceneDrawBatch> m_TransparentBatches;
		std::vector<uint32_t> m_DrawInstances;          // The view's draw list (what m_DrawListBuffer holds after the upload)
		std::vector<uint32_t> m_DrawInstancesBuild;     // The draw list being built
		std::vector<uint32_t> m_ShadowDrawInstances;
		std::vector<uint32_t> m_ShadowDrawInstancesBuild;
		nvrhi::BufferHandle m_DrawArgumentBuffer;      // The batches' indirect arguments (BuildDrawArguments), uploaded where they changed
		std::vector<nvrhi::DrawIndexedIndirectArguments> m_DrawArguments;      // What the buffer holds
		std::vector<nvrhi::DrawIndexedIndirectArguments> m_DrawArgumentsBuild; // This frame's

		// The GPU-driven path (SceneRendererGPUDriven.cpp).
		uint64_t m_RenderCount = 0;
		bool m_GPUDrivenFrame = false;  // The frame being rendered culls on the GPU
		nvrhi::BindingLayoutHandle m_CullBindingLayout;
		nvrhi::ComputePipelineHandle m_CullViewPipeline;
		nvrhi::ComputePipelineHandle m_CullShadowPipeline;
		nvrhi::ComputePipelineHandle m_CullBuildPipeline;
		nvrhi::BufferHandle m_CullConstantBuffer;
		nvrhi::BufferHandle m_BucketTableBuffer;     // Per bucket: its dense index (RenderData::c_CullNoBucket)
		nvrhi::BufferHandle m_CullCountBuffer;       // Per (dense bucket, LOD record): instances
		nvrhi::BufferHandle m_CullOffsetBuffer;      // Per (dense bucket, LOD record): its range of the draw list
		nvrhi::BufferHandle m_CullArgumentBuffer;    // Per dense bucket: ArgumentsPerBucket indirect draws
		nvrhi::BufferHandle m_CullBucketCountBuffer; // Per dense bucket: its draws (the count of drawIndexedIndirectCount)
		nvrhi::BufferHandle m_CullCounterBuffer;     // RenderData::CullCounters

		// GPU light culling (RunGPULightCulling)
		nvrhi::BindingLayoutHandle m_LightCullLayout;
		nvrhi::BindingLayoutHandle m_LightCompactLayout;
		nvrhi::ComputePipelineHandle m_LightCullPipeline;
		nvrhi::ComputePipelineHandle m_LightCompactPipeline;
		nvrhi::BindingSetHandle m_LightCullBindingSet;
		nvrhi::BindingSetHandle m_LightCompactBindingSet;
		nvrhi::BufferHandle m_LightVisibleBuffer;    // Per light record: whether it is in view
		nvrhi::BufferHandle m_LightGroupCountBuffer; // Per work group of records: its lights in view
		std::array<const nvrhi::IBuffer*, 7> m_LightCullBound = {}; // The buffers the binding sets name
		// Per light record its shadow slot plus one (Scene/LightCompact.comp): what the buffer holds, the records set in it, and
		// how many of its first entries are up to date (the records of the latest frame that culled lights on the GPU).
		nvrhi::BufferHandle m_LightShadowSlotBuffer;
		std::vector<uint32_t> m_LightShadowSlots;
		std::vector<uint32_t> m_LightShadowSlotRecords;
		uint32_t m_LightShadowSlotsWritten = 0;
		bool m_GPULightsFrame = false;               // The frame's point and spot lights are culled on the GPU
		uint32_t m_GPULightRecords = 0;              // The light records the frame's culling reads
		nvrhi::BufferHandle m_GPUDrawListBuffer;     // The draw list culling writes (view and cascades)
		nvrhi::BindingSetHandle m_CullBindingSet;
		nvrhi::BindingSetHandle m_GPUSceneBindingSet;  // The scene bindings with the GPU's draw list
		nvrhi::BindingSetHandle m_GPUShadowBindingSet;
		uint64_t m_CullBoundSceneVersion = 0;        // What m_CullBindingSet was made for
		uint64_t m_CullBoundTableVersion = 0;
		RenderData::CullConstants m_CullConstants;
		// Per bucket its dense index (RenderData::c_CullBuckets entries), then per dense bucket its draw slots.
		std::vector<uint32_t> m_CullBucketTable;
		std::vector<uint32_t> m_CullBucketTableUploaded;
		bool m_CullTablesUploaded = false;               // The table buffers hold their vectors' previous contents
		std::vector<uint32_t> m_CullBuckets;             // Per dense index: its bucket (view buckets first)
		// The draw slots of the buckets (all of which a path without a GPU-written count draws): the LOD records of the meshes
		// the frame can draw there, by arena (0: Static, 1: Skinned): the meshes in view for the view's buckets, each
		// cascade's casters' meshes for its buckets. The argument storage per bucket holds the records of all the loaded meshes
		// of an arena (RenderSceneGPU::GetArenaRecords).
		std::array<uint32_t, 2> m_CullViewSlots = {};
		std::array<std::array<uint32_t, 2>, RenderData::c_CullMaxCascades> m_CullShadowSlots = {};
		std::array<std::vector<uint32_t>, RenderData::c_CullMaxCascades> m_CullCasterStamps; // Per mesh slot: counted for the cascade
		uint32_t m_CullCasterStamp = 0;
		// The local shadow views' draw slots by their place in a batch (the most any batch's view there can draw), the views as
		// the culling reads them, and per mesh slot the view it was counted for last.
		std::array<std::array<uint32_t, 2>, RenderData::c_CullMaxLocalShadowViews> m_CullLocalSlots = {};
		std::vector<RenderData::CullLocalShadowView> m_CullLocalViews;
		nvrhi::BufferHandle m_CullLocalViewBuffer;
		std::vector<uint32_t> m_CullLocalStamps;
		uint32_t m_CullLocalStamp = 0;
		struct CounterReadback
		{
			Scope<BufferReadback> Readback;
			uint64_t Render = 0; // The renderer's frame whose counters it holds (GetRenderCount)
			// The frame's visibility feedback (RenderData::FeedbackRecord) when it wrote any, with the render scene and the
			// version of its GPU copy's tables it was culled with, and the record places of its mesh and material parts.
			Scope<BufferReadback> Feedback;
			bool HasFeedback = false;
			uint64_t RenderSceneId = 0;
			uint64_t TablesVersion = 0;
			uint32_t MeshSlots = 0;
			uint32_t MaterialSlots = 0;
			bool ShadowOcclusion = false; // The frame culled its cascades by occlusion with history (its counters say whether that paid)
		};
		std::array<CounterReadback, c_GPURecordCount> m_CounterReadbacks;
		uint32_t m_CounterReadbackIndex = 0;
		nvrhi::ComputePipelineHandle m_CullFeedbackPipeline;
		nvrhi::BufferHandle m_FeedbackMeshBuffer;     // Per mesh slot: pixels, use
		nvrhi::BufferHandle m_FeedbackMaterialBuffer; // Per material slot: pixels, UV pixels, use, unused
		nvrhi::BufferHandle m_FeedbackListBuffer;     // The touched slots (RenderData::c_FeedbackListHeader, then the lists)
		nvrhi::BufferHandle m_FeedbackRecordBuffer;   // The packed records the readback copies
		// Occlusion culling: per proxy two bits of visibility, alternating frame by frame (VisibilityParity: the previous
		// frame's), cleared when the view has no history; and the depth pyramid with a binding set per level.
		nvrhi::BufferHandle m_VisibilityBuffer;
		uint32_t m_VisibilityParity = 0;
		bool m_ResetVisibility = true;
		// Per render-scene instance its cross-fade state (RenderData::LODFadeState), cleared when created.
		nvrhi::BufferHandle m_LODFadeBuffer;
		bool m_ClearLODFades = true;
		uint32_t m_CullInstanceCount = 0; // The render scene's instances (PrepareGPUCulling)
		nvrhi::TextureHandle m_DepthPyramid;
		glm::uvec2 m_DepthPyramidSize = glm::uvec2(0); // Level 0
		uint32_t m_DepthPyramidLevels = 0;
		std::vector<nvrhi::BindingSetHandle> m_DepthPyramidSets;
		nvrhi::BindingLayoutHandle m_DepthPyramidLayout;
		nvrhi::ComputePipelineHandle m_DepthPyramidPipeline;
		nvrhi::SamplerHandle m_DepthPyramidSampler; // A minimum filter where the device has one (samplerFilterMinmax)
		// The cascades' depth pyramids (a layer each, level 0 a quarter of the shadow map each way) with a binding set per level
		// (over every layer), and what the culling binds while there is none.
		nvrhi::TextureHandle m_ShadowPyramid;
		nvrhi::ComputePipelineHandle m_ShadowPyramidPipeline; // Scene/HZBBuildArray.comp
		nvrhi::TextureHandle m_DummyShadowPyramid;
		nvrhi::ITexture* m_CullBoundShadowPyramid = nullptr;
		uint32_t m_ShadowPyramidSize = 0; // Level 0
		uint32_t m_ShadowPyramidLevels = 0;
		std::vector<nvrhi::BindingSetHandle> m_ShadowPyramidSets;
		// Whether the latest frame whose counters arrived and that culled the cascades by occlusion found enough of their
		// casters hidden (true until one did), and the latest frame that culled them (RenderCount + 1 while rendering).
		bool m_ShadowOcclusionPays = true;
		uint64_t m_ShadowOcclusionRender = 0;
		bool m_ShadowOcclusionMeasured = false; // The frame culls its cascades by occlusion, with history
		// Shadow cascade caching (SceneRendererSpecification::CacheShadowCascades): what each cascade's layer was drawn for
		// (the camera, the light and the cascade's square), what the casters were drawn with, the proxy log's version read, and
		// the world bounds of the proxies that changed since the cache began following them (their next change's old bounds).
		struct ShadowCacheKey
		{
			SceneCamera Camera;
			glm::uvec2 ViewportSize = glm::uvec2(0);
			glm::mat4 LightView = glm::mat4(1.0f);
			CascadeBox Box;
			uint32_t CascadeCount = 0;
			uint32_t ShadowMapSize = 0;
			LODCriteria LOD;
			bool operator==(const ShadowCacheKey& other) const;
		};
		struct ShadowCacheContent
		{
			uint64_t RenderSceneId = 0;
			uint64_t TablesVersion = 0;
			uint64_t PoolContentVersion = 0;
			uint64_t TextureUploadVersion = 0; // While the scene has alpha-masked materials (0 otherwise)
			nvrhi::ITexture* ShadowMap = nullptr;
			bool operator==(const ShadowCacheContent&) const = default;
		};
		std::array<ShadowCacheKey, RenderData::c_MaxCascades> m_ShadowCacheKeys = {};
		std::array<ShadowCacheKey, RenderData::c_MaxCascades> m_ShadowCachePendingKeys = {};
		ShadowCacheContent m_ShadowCacheContent;
		ShadowCacheContent m_ShadowCachePendingContent;
		uint32_t m_ShadowCacheValid = 0;      // The layers that hold what their keys describe (committed)
		bool m_ShadowCachePending = false;    // The frame drew GPU shadows: CommitShadowCache takes the pending state
		uint32_t m_CachedCascadeMask = 0;     // The frame's
		uint64_t m_ShadowCacheProxyVersion = 0;
		std::unordered_map<uint32_t, AABB> m_ShadowCacheBounds;
		std::vector<uint32_t> m_ShadowCacheChanges;
		// The newest visibility feedback read back (ResolveCullCounters), which frames apply until a newer one arrives.
		struct VisibilityFeedback
		{
			bool Valid = false;
			uint64_t Render = 0;
			uint64_t RenderSceneId = 0;
			uint64_t TablesVersion = 0;
			std::vector<RenderData::FeedbackRecord> Meshes;
			std::vector<RenderData::FeedbackRecord> Materials;
		};
		VisibilityFeedback m_Feedback;
		bool m_FeedbackFrame = false;      // The frame being rendered takes its requests from m_Feedback
		uint64_t m_MarksRender = 0;        // The latest frame (RenderCount + 1 while rendering) that ran the CPU's marks
		uint64_t m_CutRender = 0;          // The latest frame that followed a camera cut
		std::vector<uint32_t> m_UserScratch;

		// The motion history (SceneRenderOptions::MotionVectors): the frame being rendered has motion; the buffers, and those they
		// replaced whose contents the frame copies over (RecordMotionGrowth); the render scene and the write version of its GPU
		// copy they hold, and the frame that copied them last (RenderCount + 1; 0: none, or not since a frame failed).
		bool m_MotionFrame = false;
		nvrhi::BufferHandle m_PreviousInstances;
		nvrhi::BufferHandle m_PreviousPalette;
		nvrhi::BufferHandle m_GrownPreviousInstances;
		nvrhi::BufferHandle m_GrownPreviousPalette;
		uint64_t m_MotionSceneId = 0;
		uint64_t m_MotionWriteVersion = 0;
		uint64_t m_MotionRender = 0;
		// The camera of the frame that copied the history last (rotation-only view, projection without jitter).
		struct MotionCamera
		{
			glm::mat4 RelativeView = glm::mat4(1.0f);
			glm::mat4 Projection = glm::mat4(1.0f);
			glm::vec3 Position = glm::vec3(0.0f);
		};
		MotionCamera m_MotionCamera;
		nvrhi::TextureHandle m_VelocityTexture;
		nvrhi::FramebufferHandle m_MotionPrepassFramebuffer; // The prepass's attachments and the velocity target
		nvrhi::ShaderHandle m_MeshVelocityVertexShader;
		nvrhi::ShaderHandle m_SkinnedMeshVelocityVertexShader;
		nvrhi::ShaderHandle m_PrepassVelocityPixelShader;
		// The motion debug view (DrawMotionView): its binding set names the velocity target and the depth.
		nvrhi::BindingLayoutHandle m_MotionViewLayout;
		nvrhi::BindingSetHandle m_MotionViewBindingSet;
		nvrhi::GraphicsPipelineHandle m_MotionViewPipeline;
		nvrhi::BindingLayoutHandle m_MotionCopyLayout;
		nvrhi::ComputePipelineHandle m_MotionCopyPipeline;
		nvrhi::BufferHandle m_MotionCopyListBuffer; // The elements a frame copies: the instances', then the joints'
		std::vector<uint32_t> m_MotionCopyList;
		std::vector<std::pair<uint32_t, uint32_t>> m_MotionRanges;
		// Per buffer (instances, palette) the copy's binding set and the buffers it names (source, list, destination).
		std::array<nvrhi::BindingSetHandle, 2> m_MotionCopySets;
		std::array<std::array<const nvrhi::IBuffer*, 3>, 2> m_MotionCopyBound = {};
		// The buffers the scene binding sets name at SRV 20 and 21: the history, or the scene's own without one.
		const nvrhi::IBuffer* m_BoundPreviousInstances = nullptr;
		const nvrhi::IBuffer* m_BoundPreviousPalette = nullptr;
	};

}
