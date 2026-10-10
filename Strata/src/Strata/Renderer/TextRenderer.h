#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/ErrorThrottle.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/TextLayout.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Font;
	class Scene;

	struct TextRenderStats
	{
		uint32_t Texts = 0;            // Text components drawn
		uint32_t ScreenSpaceTexts = 0; // Of those, screen-space ones
		uint32_t HiddenScreenSpaceTexts = 0; // Screen-space text left out (Prepare without drawScreenSpace)
		uint32_t Glyphs = 0;           // Glyph quads drawn
		uint32_t PendingFonts = 0;     // Text drawn with the default font while its own font loads
		uint32_t PendingGlyphs = 0;    // Glyphs left out while they wait for rasterization (drawn on a later frame)
		uint32_t RasterizedGlyphs = 0; // Glyphs added to atlases this frame
	};

	// Draws the TextComponents of a scene with signed distance field glyph atlases (one per font, see FontAtlas):
	// screen-space text as a HUD over everything, world-space text in the entity's XY plane, hidden behind scene
	// geometry. Text without a font, or whose font is still loading, uses the engine's default font. SceneRenderer owns
	// one; main thread only.
	class TextRenderer
	{
	public:
		// Glyph rasterization per frame, shared by all fonts, so new text never stalls a frame for long (the cost allows
		// about 2 ms of rasterization in an optimized build on a desktop CPU: about ten Latin letters or one emoji).
		// Glyphs over it are drawn on the following frames (see TextRenderStats::PendingGlyphs).
		static constexpr GlyphRasterBudget c_FrameRasterBudget = { 32, 512 * 1024 };

		// frameConstants: the frame constant buffer of the owning SceneRenderer (view-projection of world-space text).
		TextRenderer(const std::string& debugName, nvrhi::IBuffer* frameConstants);
		~TextRenderer();

		TextRenderer(const TextRenderer&) = delete;
		TextRenderer& operator=(const TextRenderer&) = delete;

		// Lays out the scene's active text and uploads glyphs and vertices with the command list. Without drawScreenSpace
		// only world-space text is prepared (screen-space text is counted as hidden, and its glyphs are not rasterized).
		// Returns false when there is nothing to draw (or the upload failed, which is logged).
		bool Prepare(Scene& scene, const glm::uvec2& viewportSize, bool drawScreenSpace, nvrhi::ICommandList* commandList, TextRenderStats& outStats);
		// Draws the prepared world-space or screen-space text into a framebuffer whose depth attachment holds the scene
		// depth (world-space text is hidden behind it).
		void Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, bool screenSpace);
	private:
		struct TextVertex
		{
			glm::vec3 Position; // World space, or pixels (screen space)
			glm::vec3 TexCoord; // Atlas texels and page
			uint32_t Color;     // RGBA8, display colors
		};

		struct CachedAtlas
		{
			Scope<FontAtlas> Atlas;
			nvrhi::BindingSetHandle BindingSet;
			nvrhi::ITexture* BoundTexture = nullptr;
			bool Used = false;
		};

		// A run of vertices drawn with one atlas.
		struct DrawRange
		{
			CachedAtlas* Atlas = nullptr;
			uint32_t FirstVertex = 0;
			uint32_t VertexCount = 0;
		};

		CachedAtlas* GetAtlas(const Ref<Font>& font);
		void AppendQuads(CachedAtlas& atlas, const glm::mat4& transform, const glm::vec4& color, bool screenSpace);
		nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo, bool screenSpace);
	private:
		std::string m_DebugName;
		ErrorThrottle m_Errors; // Failures repeat every frame: logged once until text renders again
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout; // Frame constants, glyph atlas, sampler and push constants
		nvrhi::BufferHandle m_FrameConstants;
		nvrhi::SamplerHandle m_Sampler;
		nvrhi::BufferHandle m_VertexBuffer;
		std::vector<std::pair<nvrhi::FramebufferInfo, std::pair<nvrhi::GraphicsPipelineHandle, nvrhi::GraphicsPipelineHandle>>> m_Pipelines;

		std::unordered_map<const Font*, CachedAtlas> m_Atlases;
		TextLayout m_Layout; // Scratch, kept to reuse its allocation
		std::vector<TextVertex> m_Vertices;
		std::vector<DrawRange> m_WorldRanges;
		std::vector<DrawRange> m_ScreenRanges;
		glm::vec2 m_ViewportSize = glm::vec2(1.0f);
	};

}
