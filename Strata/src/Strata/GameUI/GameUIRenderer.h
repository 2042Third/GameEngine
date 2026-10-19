#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/ErrorThrottle.h"
#include "Strata/GameUI/GameUIRenderData.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Renderer/SceneOverlay.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Font;

	// The game UI's overlay pass (one per SceneRenderer, made through SceneOverlayRegistry): draws the root canvases of the
	// scene's GameUIScene over the finished image in display values: the screen-space ones after the screen-space text
	// (Draw), the world-space ones in the scene with the world-space text, depth-tested, through the frame's camera
	// (DrawWorld; also in views that hide the game's screen-space UI).
	//
	// The quads (built by GameUIScene, in canvas units) live in one storage buffer per renderer, uploaded only where they
	// changed since this renderer's last frame (the scene's journal of changed ranges; a canvas whose array was rebuilt, or
	// another scene, is uploaded whole), so an idle frame uploads nothing. Images reach their textures through a small table
	// of bindless slots (UI texture index -> slot, rewritten when mip streaming moves a slot), glyphs their atlas placements
	// through a table per font (glyph -> place in this renderer's atlas, from the renderer's FontAtlasCache), so quads never
	// change for either; masked quads name their canvas's clip rects, uploaded beside them when they change. Each canvas is drawn instanced, six vertices per quad, one draw per run of quads whose fonts fit
	// the c_MaxBoundAtlases atlases bound at once: a canvas with up to four fonts is one draw call. Each distinct texture and
	// font is requested once per frame through the scene's asset manager (High priority), and the textures drawn are
	// reported for mip streaming at the pixels one texture coordinate unit spans on screen. Main thread only.
	class GameUIRenderer final : public ISceneOverlayPass
	{
	public:
		explicit GameUIRenderer(const SceneOverlayPassContext& context);
		~GameUIRenderer() override;

		GameUIRenderer(const GameUIRenderer&) = delete;
		GameUIRenderer& operator=(const GameUIRenderer&) = delete;

		bool Prepare(const SceneOverlayFrame& frame) override;
		void DrawWorld(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer) override;
		void Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer) override;
		bool HasScreenContent(Scene& scene) override;
	private:
		// A run of a canvas's quads drawn in one call: its fonts' scene indices, glyph table bases and atlas textures.
		struct DrawRun
		{
			uint32_t FirstQuad = 0; // In the quad buffer
			uint32_t QuadCount = 0;
			glm::uvec4 Fonts = glm::uvec4(~0u);
			glm::uvec4 GlyphBases = glm::uvec4(0u);
			std::array<nvrhi::ITexture*, RenderData::c_MaxBoundAtlases> Atlases = {};
			uint32_t FirstClip = 0; // The canvas's clip rects in the clip buffer
			float Scale = 1.0f;
			bool PixelSnap = true;
			bool World = false;     // A world-space canvas: drawn by DrawWorld with its transform
			uint32_t Transform = 0; // Its transform in the transform buffer
		};
		static constexpr uint32_t c_NoTransform = ~0u;

		// What this renderer's quad buffer holds of a canvas view.
		struct UploadedCanvas
		{
			uint64_t QuadArrayId = 0;
			uint64_t QuadVersion = 0;
			uint32_t Offset = 0;
			uint32_t Count = 0;
			// Its clip rects (masks): where they are in the clip buffer, and what was uploaded there.
			uint64_t ClipLayoutId = 0;
			uint64_t ClipVersion = 0;
			uint32_t ClipOffset = 0;
			uint32_t ClipCount = 0;
		};

		// A font's place in the glyph table, for the atlas it was filled from.
		struct FontRegion
		{
			uint64_t AtlasId = 0;
			uint32_t Base = 0;
			uint32_t Size = 0;
			nvrhi::ITexture* Texture = nullptr; // The atlas texture this frame (null: the font is not drawn)
		};

		// False when the quads cannot be uploaded (logged).
		bool UploadQuads(GameUIScene& ui, std::span<const UICanvasView> views, nvrhi::ICommandList* commandList, SceneOverlayStats& stats);
		// The canvases' clip rects, after their quads (only those that changed since this renderer's last frame).
		bool UploadClips(GameUIScene& ui, std::span<const UICanvasView> views, nvrhi::ICommandList* commandList, SceneOverlayStats& stats);
		// The world-space canvases' transforms (canvas units to clip space), when they changed; views without one draw nothing.
		bool UploadTransforms(GameUIScene& ui, std::span<const UICanvasView> views, const SceneOverlayFrame& frame, SceneOverlayStats& stats);
		static float GetProjectedUnitPixels(const glm::mat4& transform, const glm::vec2& viewport, const glm::vec2& point);
		void DrawRuns(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, bool world);
		void UpdateGlyphs(GameUIScene& ui, const SceneOverlayFrame& frame, AssetManagerBase* assets);
		void UpdateTextures(GameUIScene& ui, const SceneOverlayFrame& frame, AssetManagerBase* assets, float maxScale);
		void BuildRuns(GameUIScene& ui, std::span<const UICanvasView> views);
		bool EnsureBuffer(nvrhi::BufferHandle& buffer, uint64_t bytes, uint32_t stride, const char* name);
		// Lets go of the binding sets and the buffers (a scene without canvases).
		void ReleaseResources();
		nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo, bool world);
		nvrhi::IBindingSet* GetBindingSet(const std::array<nvrhi::ITexture*, RenderData::c_MaxBoundAtlases>& atlases);
	private:
		std::string m_DebugName;
		ErrorThrottle m_Errors;
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::TextureHandle m_EmptyAtlas; // Bound to atlas slots no font of the draw uses
		bool m_EmptyAtlasWritten = false;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_Pipelines;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_WorldPipelines; // Depth-tested
		std::map<std::array<nvrhi::ITexture*, RenderData::c_MaxBoundAtlases>, std::pair<nvrhi::BindingSetHandle, bool>> m_BindingSets; // With "used this frame"

		nvrhi::BufferHandle m_QuadBuffer;
		nvrhi::BufferHandle m_TextureSlotBuffer;
		nvrhi::BufferHandle m_GlyphBuffer;
		nvrhi::BufferHandle m_ClipBuffer;
		nvrhi::BufferHandle m_TransformBuffer;
		std::vector<glm::mat4> m_Transforms;         // This frame's, by world-space view
		std::vector<glm::mat4> m_UploadedTransforms; // What the transform buffer holds
		std::vector<uint32_t> m_ViewTransforms;      // By view: its transform, or c_NoTransform

		uint64_t m_SceneId = 0; // The GameUIScene the buffers hold
		std::vector<UploadedCanvas> m_Uploaded;
		std::vector<UIQuadRange> m_RangeScratch;
		std::vector<uint32_t> m_TextureSlots;  // What the slot buffer holds, by UI texture index
		std::vector<float> m_TextureDensity;   // By UI texture index: most canvas units one texture coordinate spans (x scale)
		std::vector<glm::uvec2> m_Glyphs;      // What the glyph buffer holds
		std::vector<FontRegion> m_FontRegions; // By scene font index
		uint32_t m_GlyphDirtyBegin = 0;
		uint32_t m_GlyphDirtyEnd = 0;
		std::vector<uint32_t> m_GlyphScratch;
		std::vector<uint32_t> m_FontScratch;
		std::vector<DrawRun> m_Runs;
		glm::vec2 m_ViewportSize = glm::vec2(1.0f);
		float m_ScreenZoom = 1.0f;                  // SceneScreenView: the screen's pixels -> the image's
		glm::vec2 m_ScreenOrigin = glm::vec2(0.0f); // Image pixels
	};

	// Registers the pass with SceneOverlayRegistry (RegisterGameUIModule).
	void RegisterGameUIRenderer();

}
