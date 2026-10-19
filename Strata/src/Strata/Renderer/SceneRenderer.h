#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/ErrorThrottle.h"
#include "Strata/Core/RadixSort.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Renderer/AssetResolveCache.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/SceneOverlay.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/TextureReadback.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Strata
{

	class FontAtlasCache;
	class Material;
	class Mesh;
	class RenderScene;
	class Scene;
	class TextRenderer;
	class Texture;
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

	struct SceneRendererSpecification
	{
		std::string DebugName = "SceneRenderer";
		// Screen fraction (projected bounding sphere diameter / viewport height) below which each further mesh LOD is
		// used: LOD n is chosen while the fraction is below LODThreshold / 2^(n-1).
		float LODThreshold = 0.5f;
		// Point and spot lights rendered per frame (after frustum culling). When more are visible, the most relevant ones
		// are kept: lights whose range contains the camera, then by the projected size of their range.
		uint32_t MaxLights = 4096;
		// Clustered light assignment: the view is split into LightClusterGrid.x by .y screen tiles and .z depth slices
		// (exponentially spaced). Each cluster lists up to MaxLightsPerCluster lights, the most relevant ones when more
		// reach it. A 1x1x1 grid shades every pixel with every visible light.
		glm::uvec3 LightClusterGrid = { 16, 9, 24 };
		uint32_t MaxLightsPerCluster = 256;
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
		// batches are split only by pipeline state, mesh, submesh and level of detail. Off, batches are also split by
		// material, as renderers without per-instance materials draw; for comparisons and debugging.
		bool BatchAcrossMaterials = true;
		// Textures stream at the mip levels the screen needs (TextureStreamer): the renderer reports what it drew after each
		// frame (VisibleMaterialUse), and a texture whose mip tail is resident is drawn with what it has. Off, every texture
		// it draws is made resident at full resolution and counts as pending until it is (SceneRendererStats::PendingAssets):
		// exact-pixel tests and the baseline streaming is measured against.
		bool TextureStreaming = true;
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
	};

	struct SceneRendererStats
	{
		static constexpr size_t c_LODStatCount = 8;

		bool Rendered = false; // False when the last Render call drew nothing (see Render)
		uint32_t DrawCalls = 0;       // Draw calls of the forward passes (opaque and transparent)
		uint32_t OpaqueDrawCalls = 0; // Of those, opaque batches: also the draw calls of the depth prepass
		uint32_t ShadowDrawCalls = 0; // Draw calls into the shadow cascades
		uint32_t Instances = 0;       // Drawn submesh instances
		uint32_t CulledInstances = 0; // Mesh renderers outside the view frustum
		uint64_t Triangles = 0;
		// Drawn submesh instances per selected level of detail; the last entry also counts all higher levels.
		std::array<uint32_t, c_LODStatCount> InstancesPerLOD = {};
		uint32_t Lights = 0;          // Rendered lights: the directional light plus the point and spot lights
		uint32_t CulledLights = 0;    // Point and spot lights outside the view frustum
		uint32_t DroppedLights = 0;   // Visible point and spot lights beyond SceneRendererSpecification::MaxLights
		uint32_t Materials = 0;       // Distinct materials drawn
		// Of the materials drawn and casting shadows, those whose data the frame took over from an earlier frame's (the same
		// materials used the same ways, their textures in the same bindless slots: still frames), with their assets looked
		// up in one call to the asset manager (AssetResolveCache::Replay) instead of one by one.
		uint32_t ReusedMaterials = 0;
		uint32_t PendingAssets = 0;   // Distinct meshes, materials and textures still loading (drawn with fallbacks or skipped)

		// The render scene (RenderScene) and what the frame cost.
		uint32_t VisibleProxies = 0;         // Mesh renderers whose bounds intersect the view frustum
		uint32_t ProxyUpdates = 0;           // Render proxies updated since this renderer's previous frame (changed components, transforms, mesh layouts)
		uint64_t InstanceBytesUploaded = 0;  // Instance data and draw lists uploaded this frame (nothing when nothing changed)
		uint32_t InstanceUploadRanges = 0;   // Separate ranges of instance data uploaded
		uint64_t MaterialBytesUploaded = 0;  // Material data uploaded this frame (only materials whose data changed)
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
		bool PreviewLighting = false;     // SceneRenderOptions::PreviewEnvironment lit the scene (it has no light of its own)
		uint32_t ShadowCasters = 0;       // Instances drawn into shadow cascades (summed over cascades)
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
	};

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
		nvrhi::ITexture* GetHDRTexture() const { return m_HDRTexture; }
		nvrhi::ITexture* GetDepthTexture() const { return m_DepthTexture; }
		nvrhi::ITexture* GetNormalTexture() const { return m_NormalTexture; }
		nvrhi::ITexture* GetEntityIDTexture() const { return m_EntityIDTexture; } // Null without one (SceneRendererSpecification::EntityIDs)
		nvrhi::ITexture* GetAmbientOcclusionTexture() const { return m_AOTexture; } // 1 = unoccluded
		// Image-based lighting data of the current environment (null without one); the BRDF lookup table always exists.
		nvrhi::ITexture* GetEnvironmentCube() const { return m_Environment ? m_Environment->Cube.Get() : nullptr; }
		nvrhi::ITexture* GetIrradianceMap() const { return m_Environment ? m_Environment->Irradiance.Get() : nullptr; }
		nvrhi::ITexture* GetPrefilteredMap() const { return m_Environment ? m_Environment->Prefiltered.Get() : nullptr; }
		nvrhi::ITexture* GetBRDFLut() const { return m_BRDFLut; }

		nvrhi::ITexture* GetBloomTexture() const { return m_BloomTexture; } // Level 0 of the bloom chain (half resolution)

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

		// Automatic exposure adapts gradually; this makes the next frame jump to the scene's brightness (camera cuts,
		// scene loads).
		void ResetExposureAdaptation() { m_ResetExposure = true; }

		const SceneRendererSpecification& GetSpecification() const { return m_Specification; }
		const SceneRendererStats& GetStats() const { return m_Stats; }

		// The materials the last frame drew, with the largest size on screen of what they cover (for mip streaming).
		std::span<const VisibleMaterialUse> GetVisibleMaterials() const { return m_VisibleMaterials; }
		// Their loaded texture maps, one entry per material and map (what the renderer reports to the texture streamer).
		std::span<const VisibleTextureUse> GetVisibleTextures() const { return m_VisibleTextures; }
		// What the last frame drew: its batches and draw lists, which hold the index of every drawn instance
		// (RenderScene::GetInstances) in batch order: opaque batches, then transparent ones back to front; shadow batches
		// cascade by cascade.
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
			ShadowMasked
		};

		struct PipelineKey
		{
			Pass PassType = Pass::Opaque;
			bool DoubleSided = false;
			bool Mirrored = false; // Instances with a mirroring transform: front faces wind clockwise
			bool Skinned = false;  // The skinned vertex shaders and the skin vertex stream
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
		struct CascadeBox
		{
			glm::vec2 Min = glm::vec2(0.0f);
			glm::vec2 Max = glm::vec2(0.0f);
			float FarZ = 0.0f;        // Far side of the slice's bounding sphere
			float SphereNearZ = 0.0f; // Near side (toward the light)
			float WorldPerTexel = 0.0f;
			float Radius = 0.0f;
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
		// frame found (materials are not changed in place: edits publish new objects) and the textures' bindless slots
		// still hold them, the materials' data, flags and texture maps are what that frame built.
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
			std::vector<uint32_t> TextureSlots;           // The bindless slots of the textures among them
			std::vector<uint32_t> TextureSlotGenerations; // BindlessTextureTable::GetSlotGenerations of TextureSlots
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
			std::vector<entt::entity> Animators; // Animators of the chunk's visible Skin proxies (WhenVisible culling)
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

		struct EnvironmentMaps
		{
			Ref<Texture> Source; // Equirectangular HDR texture the maps were computed from (null for a procedural sky)
			std::optional<RenderData::ProceduralSkyParameters> Procedural; // The procedural sky the maps were computed from
			nvrhi::TextureHandle Cube;        // Radiance cubemap with mips (background, irradiance source)
			nvrhi::TextureHandle Irradiance;  // Diffuse irradiance
			nvrhi::TextureHandle Prefiltered; // Specular radiance, one mip per roughness level
		};
		// The environments computed from environment map textures, by texture, while a renderer lights with them
		// (FindSharedEnvironment); defined with the functions that use it.
		struct SharedEnvironments;

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
		// Mips of the radiance cube from its mip 0, then the irradiance and prefiltered maps from the cube.
		void FilterEnvironment(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps);
		RenderData::ProceduralSkyParameters GetProceduralSkyParameters(const SkyLightComponent& sky, const RenderData::FrameConstants& frame) const;
		void DispatchIBL(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::ITexture* source, const nvrhi::TextureSubresourceSet& sourceSubresources,
			nvrhi::TextureDimension sourceDimension, nvrhi::ITexture* output, uint32_t outputMip, nvrhi::TextureDimension outputDimension,
			const RenderData::IBLParameters& parameters, uint32_t layers);
		bool CreateSceneBindingSets();
		void RenderAmbientOcclusion(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		// Meters the HDR image (automatic exposure) or writes the manual exposure into the exposure buffer.
		void UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		void RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		// The scene's first active post-processing settings, or the defaults.
		static PostProcessComponent GetPostProcessSettings(Scene& scene);
		nvrhi::IGraphicsPipeline* GetScenePipeline(const PipelineKey& key);
		// Fullscreen-triangle pipeline for a pixel shader and target format, cached per format.
		nvrhi::IGraphicsPipeline* GetFullscreenPipeline(std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>>& cache, nvrhi::IShader* pixelShader,
			nvrhi::IBindingLayout* layout, const nvrhi::FramebufferInfo& framebufferInfo);
		// Grows a structured (or vertex) buffer to hold elementCount elements (the replaced buffer is released once the GPU
		// is done with it); false when the allocation failed.
		bool EnsureBufferCapacity(nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize, const char* name, bool& outRecreated, bool vertexBuffer = false);

		// The directional light (the preview sun with `preview`), ambient light and the visible point and spot lights.
		void CollectLights(Scene& scene, const SceneCamera& camera, bool preview, RenderData::FrameConstants& frame);

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
		// Computes the cascades and finds the shadow casters of each; leaves ShadowParams.w at 0 without shadows. Reports the
		// animators of skinned casters out of view whose shadows can fall on what the view shows (CanShadowView).
		void FindShadowCasters(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum, RenderData::FrameConstants& frame);
		// Whether a caster's shadow can fall on what the view shows within the shadow distance: the caster's box swept along
		// the light, as far as the visible receivers' bounds reach (m_VisibleReceivers, clipped to the shadowed part of the
		// view), is not separated from the view by any of `planes`. Conservative: it never rejects a shadow that shows.
		bool CanShadowView(const AABB& caster, const glm::vec3& lightDirection, const AABB& receivers, std::span<const glm::vec4> planes) const;
		// Resolves every mesh and material the frame uses, once each, at the priority its use asks for.
		void ResolveAssets(RenderScene& renderScene);
		// The materials of m_UsedMaterialSlots, one by one, recorded in m_MaterialResolution for later frames.
		void ResolveMaterials(RenderScene& renderScene);
		// Takes the materials' data over from m_MaterialResolution when the frame uses the same material slots the same ways
		// and nothing it depends on changed (see MaterialResolution). Returns false, having resolved nothing, otherwise.
		bool ReuseResolvedMaterials(const RenderScene& renderScene);
		// The view's draws in batches: opaque ones sorted by pipeline state and mesh, transparent ones back to front.
		void GatherDraws(RenderScene& renderScene, const SceneCamera& camera);
		// The shadow casters' draws per cascade, and the cascades' projections fitted to the casters.
		void GatherShadowDraws(RenderScene& renderScene, const SceneCamera& camera, RenderData::FrameConstants& frame);
		void MarkMesh(uint32_t slot, uint32_t usage);
		void MarkMaterial(uint32_t slot, uint32_t usage);
		// Sizes the per-material-slot state for `count` slots.
		void EnsureMaterialSlots(uint32_t count);
		// The GPU data of a material, its textures resolved (only those already loaded with residentOnly). The loaded ones
		// are appended to `resolved` when it is given (the materials the frame draws, for streaming their levels).
		RenderData::MaterialData BuildMaterialData(const Material& material, bool residentOnly, std::vector<ResolvedTexture>* resolved = nullptr);
		uint32_t ResolveTexture(AssetHandle handle, uint32_t missingSlot, uint32_t loadingSlot, bool residentOnly, std::vector<ResolvedTexture>* resolved);
		// Sorted draws into batches and the draw list (instance indices in batch order).
		void BuildBatches(std::vector<SortItem>& draws, std::vector<SceneDrawBatch>& outBatches, std::vector<uint32_t>& outDrawList);
		// Uploads what changed of the instance data and the palettes (since this renderer's last upload), the material data and
		// the draw lists.
		void UploadSceneData(nvrhi::ICommandList* commandList, const RenderScene& renderScene, bool instancesRecreated, bool materialsRecreated, bool drawListsRecreated,
			bool jointsRecreated);
		// Tells the scene's ISkinningPoses which animators' meshes the frame drew (or would have, unposed).
		void ReportVisibleAnimators(Scene& scene);
		// Uploads the range of a draw list that differs from the list the buffer holds (`uploaded`), which then holds the new one.
		uint64_t UploadDrawList(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, std::vector<uint32_t>& list, std::vector<uint32_t>& uploaded, bool full);

		bool EnsureShadowMap();
		uint32_t SelectLOD(const Submesh& submesh, float screenFraction) const;
		static float GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius);
		void DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent);
		void DrawShadows(nvrhi::ICommandList* commandList, uint32_t cascadeCount);
		void BuildLightClusters(nvrhi::ICommandList* commandList);
		void CreateOverlayResources();
		static bool HasOverlays(const SceneRenderOptions& options);
		// Draws the overlays into the output texture (which must hold the finished image); false when GPU resources failed.
		bool RenderOverlays(nvrhi::ICommandList* commandList, Scene& scene, const SceneRenderOptions& options, bool drawText, bool drawOverlayPasses);
		// Prepare of every overlay pass (SceneOverlayRegistry); whether any draws.
		bool PrepareOverlayPasses(Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, nvrhi::ICommandList* commandList);
		void DrawDebugLines(nvrhi::ICommandList* commandList, DebugDrawDepth depth, uint32_t firstVertex, uint32_t vertexCount);
		// The tints of SceneRenderOptions::EntityTints (StatusTint.frag); false when GPU resources failed.
		bool DrawEntityTints(nvrhi::ICommandList* commandList, Scene& scene, std::span<const SceneEntityTint> tints, const nvrhi::Viewport& viewport);
	private:
		SceneRendererSpecification m_Specification;
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::CommandListHandle m_CommandList;
		glm::uvec2 m_ViewportSize = { 0, 0 };
		bool m_TargetsValid = false;
		bool m_HasRenderedFrame = false; // Since the render targets were (re)created
		// The camera of the previous frame, to tell the texture streamer when the view jumped (a camera cut).
		bool m_HasPreviousCamera = false;
		glm::vec3 m_PreviousCameraPosition = glm::vec3(0.0f);
		glm::vec3 m_PreviousCameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
		ErrorThrottle m_Errors; // Rendering failures, logged once until a frame renders successfully again
		bool m_WarnedLightOverflow = false;
		SceneRendererStats m_Stats;

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
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::GraphicsPipelineHandle m_SkyboxPipeline;

		// Image-based lighting
		Ref<const EnvironmentMaps> m_Environment; // Null without an environment; a map's is shared (FindSharedEnvironment)
		Ref<TextureHold> m_EnvironmentHold; // While the environment map's environment is still to be computed (HoldEnvironmentMap)
		nvrhi::ITexture* m_BoundEnvironment = nullptr; // Environment cube referenced by m_SceneBindingSet
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
		glm::vec3 m_SunColor = glm::vec3(1.0f); // Color of the directional light the scene is lit by (the procedural sun's tint)
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
		nvrhi::BufferHandle m_InstanceBuffer; // Persistent: RenderScene::GetInstances, uploaded where it changed
		nvrhi::BufferHandle m_DrawListBuffer; // Instance indices of the view's draws
		nvrhi::BufferHandle m_MaterialBuffer; // Persistent: by material slot, uploaded where it changed
		nvrhi::BufferHandle m_JointBuffer;    // Persistent: RenderScene::GetPalette, uploaded where it changed
		nvrhi::BufferHandle m_LightBuffer;

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
		std::vector<LightCandidate> m_LightCandidates;

		// Extraction (see SceneRendererExtraction.cpp). Slots are those of the render scene the state belongs to.
		AssetResolveCache m_Assets;     // Holds the frame's meshes, materials and textures while the frame is rendered
		uint64_t m_RenderSceneId = 0;   // The render scene the per-slot state and the GPU copies below belong to
		uint64_t m_ProxyUpdatesScene = 0; // The render scene whose RenderSceneStats::TotalUpdates this renderer saw last
		uint64_t m_ProxyUpdatesSeen = 0;
		bool m_FullUpload = true;       // The GPU copies hold nothing of that render scene yet
		uint64_t m_UploadedInstanceVersion = 0;
		uint64_t m_UploadedPaletteVersion = 0;
		std::vector<std::pair<uint32_t, uint32_t>> m_PaletteChanges;
		std::vector<entt::entity> m_VisibleAnimators; // Animators of the frame's visible Skin proxies, reported to ISkinningPoses
		AABB m_VisibleReceivers; // The world bounds of the frame's visible proxies (scenes with Skin proxies only)
		uint32_t m_FrameStamp = 0;
		std::vector<uint32_t> m_ProxyStamps;        // Per proxy: m_FrameStamp when it is in the view this frame
		std::vector<uint32_t> m_CasterStamps;       // Per proxy: m_FrameStamp when it is a shadow caster candidate
		std::vector<uint32_t> m_VisibleProxies;
		std::vector<float> m_VisibleFractions;      // Screen fraction of each visible proxy
		std::vector<uint32_t> m_QueryScratch;
		std::vector<ShadowCaster> m_ShadowCasters;
		std::vector<uint32_t> m_MeshUsage;          // Per mesh slot: how the frame uses it (usage bits)
		std::vector<uint32_t> m_UsedMeshSlots;
		std::vector<const Mesh*> m_FrameMeshes;     // Per mesh slot: the loaded mesh with GPU buffers, while the frame holds it
		std::vector<uint32_t> m_MaterialUsage;      // Per material slot
		std::vector<uint32_t> m_UsedMaterialSlots;
		std::vector<uint32_t> m_MaterialFlags;      // Per material slot: how the frame draws it (material flags)
		std::vector<RenderData::MaterialData> m_MaterialData; // What the material buffer holds, by slot
		std::vector<uint8_t> m_MaterialUploaded;    // Per material slot: the buffer holds its m_MaterialData
		std::vector<uint32_t> m_DirtyMaterialSlots;
		std::vector<float> m_MaterialPixelSizes;    // Per material slot: the frame's VisibleMaterialUse::PixelSize
		std::vector<float> m_MaterialUVPixels;      // Per material slot: the frame's VisibleMaterialUse::UVPixels
		std::vector<float> m_MaterialUVScales;
		std::vector<ResolvedTexture> m_ResolvedTextures;          // The loaded texture maps of the materials the frame draws (kept when it reuses them)
		std::vector<ResolvedTextureRange> m_MaterialTextures;     // Per material slot: its range of m_ResolvedTextures this frame
		MaterialResolution m_MaterialResolution;                 // The latest frame that resolved its materials one by one
		std::vector<VisibleMaterialUse> m_VisibleMaterials;
		std::vector<VisibleTextureUse> m_VisibleTextures;
		std::optional<RenderData::MaterialData> m_DefaultMaterialData; // The default material, resolved once per frame
		uint32_t m_DefaultMaterialFlags = 0;
		std::vector<ExtractionChunk> m_Chunks;
		std::vector<SortItem> m_Draws;
		std::vector<SortItem> m_ShadowDraws;
		std::vector<SortItem> m_SortScratch;
		std::vector<TransparentDraw> m_Transparent;
		std::vector<uint32_t> m_InstanceChanges;
		std::vector<SceneDrawBatch> m_OpaqueBatches;
		std::vector<SceneDrawBatch> m_TransparentBatches;
		std::vector<uint32_t> m_DrawInstances;          // The view's draw list (what m_DrawListBuffer holds after the upload)
		std::vector<uint32_t> m_DrawInstancesBuild;     // The draw list being built
		std::vector<uint32_t> m_ShadowDrawInstances;
		std::vector<uint32_t> m_ShadowDrawInstancesBuild;
	};

}
