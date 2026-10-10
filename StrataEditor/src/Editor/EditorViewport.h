#pragma once

#include "Editor/EditorCamera.h"
#include "Editor/TransformEdit.h"
#include "Editor/ViewportRenderer.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Math/AABB.h>
#include <Strata/Renderer/TextureReadback.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// Viewport options saved with the project's editor state.
	struct ViewportSettings
	{
		static constexpr float c_MaxTranslateSnap = 10000.0f;
		static constexpr float c_MaxRotateSnap = 360.0f;
		static constexpr float c_MaxScaleSnap = 100.0f;

		bool ShowGrid = true;
		bool ShowSelectionOutline = true;
		bool ShowSceneGizmos = true; // Light, camera and collider shapes
		bool ShowStats = false;      // Frame time, draw calls and loading assets over the image
		// Editor views (the editor camera while the game is not playing, see ViewportView::EditorView) light scenes that
		// have no light of their own with a preview sun and sky (SceneRenderOptions::PreviewEnvironment).
		bool PreviewLighting = true;
		// Editor views draw the game's screen-space text (its HUD); off, it does not cover the scene being edited.
		bool ShowGameUI = false;
		GizmoOperation Gizmo = GizmoOperation::Translate;
		GizmoSpace Space = GizmoSpace::World;
		// Gizmo drags snap to the steps below while this is on; holding Ctrl inverts it for a drag.
		bool Snap = false;
		// Snapping steps: world units, degrees and scale factor.
		float TranslateSnap = 0.5f;
		float RotateSnap = 15.0f;
		float ScaleSnap = 0.1f;
		// The selection outline's display color. Not saved: the editor's theme sets it (its accent).
		glm::vec4 SelectionColor = SceneRenderOptions().SelectionColor;

		nlohmann::json ToJson() const;
		// Applies a ToJson document; absent values keep their current value. Fails (changing nothing) on invalid values.
		bool FromJson(const nlohmann::json& json, std::string* outError = nullptr);
	};

	// Where the viewport panel shows the rendered image: its rectangle in UI units (ImGui coordinates, screen points) and
	// the framebuffer pixels per unit (above 1 on Retina and other high-density displays). The image is rendered at its
	// pixel size; mouse positions convert to pixels of it.
	struct ViewportImageArea
	{
		glm::vec2 Min = { 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };
		glm::vec2 PixelScale = { 1.0f, 1.0f };

		glm::uvec2 GetPixelSize() const;
		// The image pixel under a UI position; nullopt outside the image or for an empty one.
		std::optional<glm::uvec2> ToPixel(const glm::vec2& position) const;

		// Pixels per unit of a window: its viewport's own scale where the UI has one (multi-viewport support), otherwise
		// the display's, and 1 where neither is known (zero or invalid).
		static glm::vec2 ChoosePixelScale(const glm::vec2& viewportScale, const glm::vec2& displayScale);
	};

	// How a click in the viewport changes the selection.
	enum class ViewportPickMode : uint8_t
	{
		Replace = 0, // Select the clicked entity only; clicking empty space clears the selection
		Toggle,      // Ctrl: add or remove the clicked entity
		Add          // Shift: add the clicked entity
	};

	// The editor's view of its scene, independent of any UI: the editor camera, the viewport settings, the renderers of
	// the viewport and of captures (created on first use; they need a GPU), and click picking. Owned by EditorContext;
	// the ImGui viewport panel draws it and feeds it input. Main thread only.
	class EditorViewport
	{
	public:
		static constexpr uint32_t c_DefaultWidth = 1280;
		static constexpr uint32_t c_DefaultHeight = 720;
		// A pick whose GPU readback has not finished after this long is dropped (a lost or hung device).
		static constexpr std::chrono::seconds c_PickTimeout { 5 };

		EditorViewport() = default;
		~EditorViewport();

		EditorViewport(const EditorViewport&) = delete;
		EditorViewport& operator=(const EditorViewport&) = delete;

		EditorCamera& GetCamera() { return m_Camera; }
		const EditorCamera& GetCamera() const { return m_Camera; }
		ViewportSettings& GetSettings() { return m_Settings; }
		const ViewportSettings& GetSettings() const { return m_Settings; }

		// Pixel size of the image the viewport panel shows (zero while it is hidden). Captures default to it.
		void SetSize(const glm::uvec2& size) { m_Size = size; }
		const glm::uvec2& GetSize() const { return m_Size; }
		// The panel's aspect ratio, or the default capture size's while the panel has no size.
		float GetAspectRatio() const;

		// Renderer of the viewport panel, and a separate one for captures (any size, any camera). Created on first use;
		// null without an initialized renderer (headless editors).
		ViewportRenderer* GetRenderer();
		ViewportRenderer* GetCaptureRenderer();

		// Frames the entities (with their descendants) in the editor camera, or the whole scene when the span is empty.
		// False (camera unchanged) when there is nothing to frame.
		bool Focus(EditorContext& context, std::span<const Entity> entities);
		// The first view of the active scene: looks along the scene's primary camera (keeping the editor camera's
		// orientation when there is none) and fits the scene's renderable content into the view (EditorCamera::FitBounds).
		// outPendingMeshes receives meshes that were still loading (placeholder boxes stood in for them). False (camera
		// unchanged) for a scene without active entities.
		bool FrameScene(EditorContext& context, std::vector<AssetHandle>* outPendingMeshes = nullptr);

		// The editor camera of each scene asset the project has shown, so every scene opens with the view it was left
		// with. Stores the current camera for `scene` (nothing for an invalid handle).
		void StoreSceneCamera(AssetHandle scene);
		// Switches to the camera stored for `scene`. False (camera unchanged) when there is none.
		bool RestoreSceneCamera(AssetHandle scene);
		bool HasSceneCamera(AssetHandle scene) const { return m_SceneCameras.contains(scene); }
		// Forgets the cameras of scenes for which `keep` returns false (deleted scene assets).
		void PruneSceneCameras(const std::function<bool(AssetHandle)>& keep);

		// Starts picking the entity at a pixel of the viewport renderer's last frame (a GPU readback, finished by
		// UpdatePicking over the next frames). imageSize is the pixel size of the image the click was on: when the
		// renderer's last frame has another size (the panel is being resized), the pixel would point elsewhere and the
		// click is not picked. Replaces a pending pick. False when no pick was started.
		bool RequestPick(const glm::uvec2& pixel, const glm::uvec2& imageSize, ViewportPickMode mode);
		bool IsPickPending() const { return m_Pick != nullptr; }
		// Once per frame: applies a finished pick to the selection. Picks of a scene that is no longer active (play mode
		// started or stopped meanwhile) are dropped.
		void UpdatePicking(EditorContext& context);
		// Changes the selection as a click on `entity` (invalid: empty space) does in the given mode.
		static void ApplyPick(EditorContext& context, Entity entity, ViewportPickMode mode);

		// Editor state saved per project: {"Strata": {"Format": "EditorViewport", ...}, "Camera": {...},
		// "Cameras": {"<scene handle>": {...}}, "Settings": {...}}. "Camera" is the current camera (all that files written
		// before per-scene cameras hold); loading a file without "Cameras" keeps it for the first scene that opens without
		// a camera of its own.
		nlohmann::json ToJson() const;
		bool FromJson(const nlohmann::json& json, std::string* outError = nullptr);
		bool Save(const std::filesystem::path& path, std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& path, std::string* outError = nullptr);
		// Default camera and settings, no scene cameras (another project is opened).
		void ResetState();
	private:
		struct PendingPick
		{
			Scope<TextureReadback> Readback;
			std::weak_ptr<Scene> PickedScene;
			ViewportPickMode Mode = ViewportPickMode::Replace;
			std::chrono::steady_clock::time_point Requested;
		};
	private:
		EditorCamera m_Camera;
		std::map<AssetHandle, EditorCamera> m_SceneCameras;
		// The camera of a state file without per-scene cameras, for the first scene that opens without one.
		std::optional<EditorCamera> m_UnassignedCamera;
		ViewportSettings m_Settings;
		glm::uvec2 m_Size = { 0, 0 };
		Scope<ViewportRenderer> m_Renderer;
		Scope<ViewportRenderer> m_CaptureRenderer;
		Scope<PendingPick> m_Pick;
	};

}
