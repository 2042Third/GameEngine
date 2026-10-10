#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Renderer/DebugDraw.h>
#include <Strata/Renderer/SceneRenderer.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;

	// What the viewport shows: the scene as rendered, or colored by what the streaming pillars do to it.
	enum class ViewportDebugView : uint8_t
	{
		None = 0,
		Streaming, // Entities whose mesh, material or textures are still loading, or failed to
		LOD        // Entities by the level of detail their mesh is drawn at
	};

	// "none", "streaming", "lod" (the names commands and the saved viewport state use).
	const char* ViewportDebugViewToString(ViewportDebugView view);
	std::optional<ViewportDebugView> ViewportDebugViewFromString(std::string_view name);

	// Display colors (sRGB; alpha is the tint's opacity) of the debug views. Not saved: the editor's UI sets them from its
	// theme (Sulfur and Cinnabar; Malachite, Azurite, Sulfur and Cinnabar for the levels of detail).
	struct ViewportDebugColors
	{
		glm::vec4 Pending = { 0.91f, 0.76f, 0.30f, 0.65f };
		glm::vec4 Failed = { 0.90f, 0.33f, 0.29f, 0.65f };
		// Levels of detail 0, 1, 2 and 3 or higher.
		std::array<glm::vec4, 4> LOD = { {
			{ 0.24f, 0.75f, 0.55f, 0.55f },
			{ 0.30f, 0.56f, 0.88f, 0.55f },
			{ 0.91f, 0.76f, 0.30f, 0.55f },
			{ 0.90f, 0.33f, 0.29f, 0.55f } } };
	};

	// What a debug view found in the last Build, among the entities in the camera's view.
	struct StreamingViewStats
	{
		// Streaming view: entities whose mesh, material or a texture of it is still loading (none failed), and entities with
		// a failed one. Entities whose mesh is not there are drawn as placeholder boxes (at most c_MaxPlaceholderBoxes).
		uint32_t PendingEntities = 0;
		uint32_t FailedEntities = 0;
		uint32_t PlaceholderBoxes = 0;
		// LOD view: entities by the level of detail of their mesh (the last entry counts level 3 and higher).
		std::array<uint32_t, 4> EntitiesPerLOD = {};
	};

	// Builds what a debug view draws over the viewport image for one frame: SceneRenderOptions::EntityTints (the drawn
	// entities' colors) and, in the streaming view, placeholder boxes for entities whose mesh is not there to draw. Reads
	// the active asset manager's states without requesting loads (asset objects are only read once they are Ready), with
	// the same choice of mesh, material and textures as SceneRenderer, and the level of detail it draws
	// (LODSelection::SelectLOD). Main thread only.
	class StreamingView
	{
	public:
		static constexpr uint32_t c_MaxPlaceholderBoxes = 4096;

		// Rebuilds the tints and adds placeholder boxes to `placeholders` (which the caller clears and draws). Entities outside
		// the camera's view get no tint. ViewportDebugView::None builds nothing.
		void Build(Scene& scene, ViewportDebugView view, const SceneCamera& camera, float lodThreshold, const ViewportDebugColors& colors,
			DebugDraw& placeholders);

		std::span<const SceneEntityTint> GetTints() const { return m_Tints; }
		const StreamingViewStats& GetStats() const { return m_Stats; }
	private:
		enum class AssetStatus : uint8_t
		{
			Ready = 0,
			Pending,
			Failed,
			Unknown // Not registered: the renderer ignores it (fallbacks), it never arrives
		};

		AssetStatus GetStatus(AssetHandle handle);
		// The worst status of a material and the textures it uses.
		AssetStatus GetMaterialStatus(AssetHandle material);
		void BuildStreaming(Scene& scene, const SceneCamera& camera, const ViewportDebugColors& colors, DebugDraw& placeholders);
		void BuildLOD(Scene& scene, const SceneCamera& camera, float lodThreshold, const ViewportDebugColors& colors);
	private:
		std::vector<SceneEntityTint> m_Tints;
		StreamingViewStats m_Stats;
		// Per Build: each asset's status and each material's (with its textures), looked up once.
		std::unordered_map<AssetHandle, AssetStatus> m_Statuses;
		std::unordered_map<AssetHandle, AssetStatus> m_MaterialStatuses;
	};

}
