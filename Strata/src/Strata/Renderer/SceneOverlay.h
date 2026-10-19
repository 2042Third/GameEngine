#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	class FontAtlasCache;
	class GPUPassTimer;
	class Scene;
	struct VisibleTextureUse;

	// Where the game's screen lies in a renderer's image (SceneRenderOptions::ScreenView). The game's screen-space UI (the
	// HUD text and the canvases of the overlay passes) is laid out for a screen of ScreenSize pixels and drawn Zoom times
	// larger, the screen's top-left corner at Offset in the image's pixels. The default is the whole image; the editor's 2D
	// canvas mode zooms and pans it, so the UI keeps the layout of the game's screen while it is magnified.
	struct SceneScreenView
	{
		glm::vec2 ScreenSize = glm::vec2(0.0f); // Pixels; zero or invalid: the image's size
		float Zoom = 1.0f;                       // Image pixels per screen pixel; zero or invalid: 1
		glm::vec2 Offset = glm::vec2(0.0f);      // Image pixels; invalid: 0

		// The screen's size for an image of `viewport` pixels.
		glm::vec2 GetScreenSize(const glm::uvec2& viewport) const
		{
			const bool valid = std::isfinite(ScreenSize.x) && std::isfinite(ScreenSize.y) && ScreenSize.x >= 1.0f && ScreenSize.y >= 1.0f;
			return valid ? ScreenSize : glm::vec2(viewport);
		}
		float GetZoom() const { return std::isfinite(Zoom) && Zoom > 0.0f ? Zoom : 1.0f; }
		glm::vec2 GetOffset() const { return std::isfinite(Offset.x) && std::isfinite(Offset.y) ? Offset : glm::vec2(0.0f); }
	};

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
		SceneScreenView ScreenView;               // SceneRenderOptions::ScreenView: where the game's screen lies in the image
		bool DrawScreenSpace = true;              // SceneRenderOptions::DrawScreenSpaceText: the game's screen-space UI
		// The frame's camera, for overlays placed in the scene (world-space canvases), in the scene shaders' camera-relative
		// space: ViewProjection (clip space: reversed-Z) takes positions relative to CameraPosition (its view only rotates),
		// so subtract the camera from what is placed, in double, before multiplying. HasCamera is false when the overlay scene
		// is not the rendered one (its camera would be another scene's).
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		glm::vec3 CameraPosition = glm::vec3(0.0f);
		bool HasCamera = false;
		nvrhi::ICommandList* CommandList = nullptr; // Open: uploads are recorded here
		FontAtlasCache* Atlases = nullptr;        // The renderer's glyph atlases, with what the frame's raster budget has left
		std::vector<VisibleTextureUse>* VisibleTextures = nullptr; // Textures drawn, appended for mip streaming
		SceneOverlayStats* Stats = nullptr;       // Added to
		// The renderer's GPU timer of the frame (GPUProfiler.h), valid until the frame's Draw returns. A pass times its draws
		// as a GPU stage of its own: Next(its stage) before them, then Next back to the stage the timer had (GPU.Overlays).
		GPUPassTimer* GPUTimer = nullptr;
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
		// Draws what Prepare prepared in the scene (world-space canvases) into `framebuffer` (the renderer's output: display
		// values, the scene depth attached read-only), depth-tested, right after the world-space text.
		virtual void DrawWorld(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer) = 0;
		// Draws what Prepare prepared on the screen into `framebuffer`, over everything drawn before.
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
