#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Renderer/DebugDraw.h>
#include <Strata/Renderer/SceneRenderer.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class RenderScene;
	class Scene;

	// What the viewport shows: the scene as rendered, or colored by what the streaming pillars do to it.
	enum class ViewportDebugView : uint8_t
	{
		None = 0,
		Streaming, // Entities whose mesh, material or textures are still loading, or failed to
		LOD,       // Entities by the level of detail their mesh is drawn at
		Mip        // Entities by how far their textures' resident mip levels are from those their view needs
	};

	// "none", "streaming", "lod", "mip" (the names commands and the saved viewport state use).
	const char* ViewportDebugViewToString(ViewportDebugView view);
	std::optional<ViewportDebugView> ViewportDebugViewFromString(std::string_view name);

	// Display colors (sRGB; alpha is the tint's opacity) of the debug views. Not saved: the editor's UI sets them from its
	// theme (Sulfur and Cinnabar; Malachite, Azurite, Sulfur and Cinnabar for the levels of detail and the mip levels).
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
		// Textures at the mip level their view needs, 1, 2, and 3 or more levels coarser (or still loading).
		std::array<glm::vec4, 4> Mip = { {
			{ 0.24f, 0.75f, 0.55f, 0.55f },
			{ 0.30f, 0.56f, 0.88f, 0.55f },
			{ 0.91f, 0.76f, 0.30f, 0.55f },
			{ 0.90f, 0.33f, 0.29f, 0.55f } } };
		// The viewport's cell bounds (CellBoundsOverlay): cells loading, loaded and unloading.
		glm::vec4 CellLoading = { 0.91f, 0.76f, 0.30f, 1.0f };
		glm::vec4 CellLoaded = { 0.84f, 0.55f, 0.27f, 1.0f };
		glm::vec4 CellUnloading = { 0.60f, 0.58f, 0.55f, 1.0f };
	};

	// What a debug view found in the last Build, among the entities in the camera's view.
	struct StreamingViewStats
	{
		// Streaming view: entities whose mesh, material or a texture of it is still loading (none failed), and entities with
		// a failed one. Entities whose mesh is not there are drawn as placeholder boxes (at most c_MaxPlaceholderBoxes).
		uint32_t PendingEntities = 0;
		uint32_t FailedEntities = 0;
		uint32_t PlaceholderBoxes = 0;
		// LOD view: entities by the level of detail of their mesh (the last entry counts level 3 and higher), and per entry the
		// largest error in pixels (of the viewport height) of the levels drawn there, among levels chosen by their errors
		// (LODSelection::GetErrorPixels; 0 where there are none).
		std::array<uint32_t, 4> EntitiesPerLOD = {};
		std::array<float, 4> MaxErrorPixelsPerLOD = {};
		// Mip view: entities with textures, by the mip levels their worst texture lacks: none (resident at the level the views
		// need), 1, 2, and 3 or more (or a texture still loading).
		std::array<uint32_t, 4> EntitiesPerMipShortfall = {};
	};

	// The LOD view's legend name of a level (`last`: the entry that counts it and the higher ones): "LOD 1", "LOD 3+", with the
	// largest error drawn at it in pixels where it has one (StreamingViewStats::MaxErrorPixelsPerLOD): "LOD 2 (0.84 px)".
	std::string GetLODLegendName(size_t level, bool last, float errorPixels);

	// Builds what a debug view draws over the viewport image for one frame: SceneRenderOptions::EntityTints (the drawn
	// entities' colors) and, in the streaming view, placeholder boxes for entities whose mesh is not there to draw. Works on
	// the scene's render proxies (RenderScene): the entities the renderer finds in the view, culled with the bounds it culls
	// with (a mesh's own, else those the asset pipeline recorded for it), with the same choice of mesh, material and
	// textures, and the level of detail it draws (LODSelection::SelectLOD). It observes streaming without changing it: asset
	// states are read and loaded objects peeked at (AssetManagerBase::PeekLoadedAsset), so no load starts and nothing counts
	// as used, and an asset only the view looks at is evicted like any other. Main thread only.
	class StreamingView
	{
	public:
		static constexpr uint32_t c_MaxPlaceholderBoxes = 4096;

		// Rebuilds the tints and adds placeholder boxes to `placeholders` (which the caller clears and draws). Entities outside
		// the camera's view get no tint. ViewportDebugView::None builds nothing.
		// `lod`: the renderer's (SceneRenderer::GetLODCriteria).
		void Build(Scene& scene, ViewportDebugView view, const SceneCamera& camera, const LODCriteria& lod, const ViewportDebugColors& colors,
			DebugDraw& placeholders);

		std::span<const SceneEntityTint> GetTints() const { return m_Tints; }
		const StreamingViewStats& GetStats() const { return m_Stats; }
	private:
		enum class AssetStatus : uint8_t
		{
			Ready = 0,
			Pending,
			Failed,
			Unknown, // Not registered: the renderer ignores it (fallbacks), it never arrives
			NotLookedUp
		};

		AssetStatus GetStatus(AssetHandle handle);
		// The worst status of a material and the textures it uses.
		AssetStatus GetMaterialStatus(const RenderScene& renderScene, uint32_t materialSlot);
		void BuildStreaming(Scene& scene, const RenderScene& renderScene, const ViewportDebugColors& colors, DebugDraw& placeholders);
		void BuildLOD(Scene& scene, const RenderScene& renderScene, const SceneCamera& camera, const LODCriteria& lod, const ViewportDebugColors& colors);
		// The mip levels a material's worst texture lacks (Asset::GetDetailLevels: the finest level resident beyond the finest
		// one wanted): c_Loading while the material or a texture is still loading (or failed to), c_NoTextures for a material
		// without registered textures (nothing of it streams).
		uint32_t GetMaterialMipShortfall(const RenderScene& renderScene, uint32_t materialSlot);
		uint32_t GetTextureMipShortfall(AssetHandle texture);
		void BuildMip(Scene& scene, const RenderScene& renderScene, const ViewportDebugColors& colors);
	private:
		static constexpr uint32_t c_Loading = std::numeric_limits<uint32_t>::max();
		static constexpr uint32_t c_NoTextures = std::numeric_limits<uint32_t>::max() - 1;
		static constexpr uint32_t c_ShortfallNotLookedUp = std::numeric_limits<uint32_t>::max() - 2;

		std::vector<SceneEntityTint> m_Tints;
		StreamingViewStats m_Stats;
		// Per Build: the proxies in view, and each mesh's and material's status (a material's with its textures), looked up
		// once per slot of the render scene.
		std::vector<uint32_t> m_Visible;
		std::vector<AssetStatus> m_MeshStatuses;
		std::vector<AssetStatus> m_MaterialStatuses;
		std::unordered_map<AssetHandle, AssetStatus> m_TextureStatuses;
		// Mip view: each material's shortfall and each texture's, looked up once per Build.
		std::vector<uint32_t> m_MaterialShortfalls;
		std::unordered_map<AssetHandle, uint32_t> m_TextureShortfalls;
	};

}
