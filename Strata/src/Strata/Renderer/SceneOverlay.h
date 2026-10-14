#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	class FontAtlasCache;
	class Scene;
	struct VisibleTextureUse;

	// What a renderer's overlay passes drew in a frame (SceneRendererStats::Overlays): screen-space UI, which modules above
	// the renderer (the game UI) draw over the finished image.
	struct SceneOverlayStats
	{
		uint32_t Canvases = 0;       // Root canvases drawn
		uint32_t HiddenCanvases = 0; // Root canvases left out (SceneRenderOptions::DrawScreenSpaceText is off), not laid out
		uint32_t Elements = 0;       // Elements of the drawn canvases
		uint32_t Quads = 0;          // Quads drawn (empty slots included)
		uint32_t DrawCalls = 0;
		uint32_t PendingGlyphs = 0;  // Glyphs waiting for rasterization (budgeted per frame), drawn on a later frame
		uint32_t PendingAssets = 0;  // Fonts and textures still loading (drawn with the default font and white)
		uint64_t UploadedBytes = 0;  // Quads, texture slots and glyph placements uploaded this frame
	};

	// A frame of a SceneRenderer, as its overlay passes see it in Prepare.
	struct SceneOverlayFrame
	{
		Scene* OverlayScene = nullptr;            // Whose overlays are drawn (SceneRenderOptions::OverlayScene, else the rendered scene)
		glm::uvec2 ViewportSize = glm::uvec2(0);  // Pixels
		bool DrawScreenSpace = true;              // SceneRenderOptions::DrawScreenSpaceText: the game's screen-space UI
		nvrhi::ICommandList* CommandList = nullptr; // Open: uploads are recorded here
		FontAtlasCache* Atlases = nullptr;        // The renderer's glyph atlases, with what the frame's raster budget has left
		std::vector<VisibleTextureUse>* VisibleTextures = nullptr; // Textures drawn, appended for mip streaming
		SceneOverlayStats* Stats = nullptr;       // Added to
	};

	// A pass a module adds to every SceneRenderer, drawn over the finished image after the screen-space text (the game UI).
	// The renderer knows nothing of the module: modules register factories (SceneOverlayRegistry) and each renderer makes one
	// pass per factory when it is constructed. Main thread only.
	class ISceneOverlayPass
	{
	public:
		virtual ~ISceneOverlayPass() = default;

		// Lays out and uploads what the frame draws; false when it draws nothing (a scene without such overlays costs as
		// little as a check whether it has any).
		virtual bool Prepare(const SceneOverlayFrame& frame) = 0;
		// Draws what Prepare prepared into `framebuffer` (the renderer's output: display values, the scene depth attached
		// read-only), over everything drawn before.
		virtual void Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer) = 0;
		// Whether the scene has screen-space content this pass would draw (GameRenderer shows a scene without a camera
		// that has some, e.g. a main menu, instead of its missing-camera message).
		virtual bool HasScreenContent(Scene& scene) = 0;
	};

	struct SceneOverlayPassContext
	{
		std::string DebugName; // Of the SceneRenderer
	};

	using SceneOverlayFactory = std::function<Scope<ISceneOverlayPass>(const SceneOverlayPassContext& context)>;

	// The overlay passes renderers make. Modules register their factories in their registration function (the composition
	// root runs them before any renderer exists); renderers made afterwards make one pass per factory, in registration order.
	// Main thread only.
	class SceneOverlayRegistry
	{
	public:
		struct Entry
		{
			std::string Name;
			SceneOverlayFactory Factory;
		};

		// Adds a factory, or replaces the one registered under the same name. False (nothing changes) for an empty name or
		// factory.
		static bool Register(std::string name, SceneOverlayFactory factory);
		static const std::vector<Entry>& GetAll();
	};

}
