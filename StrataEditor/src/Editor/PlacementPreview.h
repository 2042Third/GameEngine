#pragma once

#include "Editor/EditorPlacement.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>
#include <Strata/Math/Ray.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	class DebugDraw;
	class EditorContext;

	// The colors of the placement ghost (ViewportSettings::Placement): display colors the editor's theme sets, not saved.
	struct PlacementColors
	{
		glm::vec4 Hit;   // A surface that takes the asset; a material's target outline
		glm::vec4 Guess; // The quieter box of a fallback (Grid, InFront)
		glm::vec4 Tint;  // A material target's tint (alpha: its opacity)

		// Without the editor's theme (core code, tests): Hit is SceneRenderOptions().SelectionColor, Guess and Tint the same
		// color at alpha 0.5 and 0.35.
		static PlacementColors Default();
	};

	// The pointer of the Scene view over its image this frame, with an asset dragged.
	struct PlacementPointer
	{
		AssetHandle Asset;
		Ray PointerRay;                // EditorPlacement::MakeViewRay of the view's camera at the pointer
		float MaxDistance = 0.0f;      // The view camera's far plane
		float FallbackDistance = 0.0f; // EditorPlacement::GetFallbackDistance of the view
		std::optional<float> Snap;     // ViewportSettings::TranslateSnap while snapping (Snap, inverted by Ctrl)
		bool CanvasMode = false;       // The view shows the 2D canvas
	};

	// What a dragged asset would do if it were dropped now, for the Scene view's ghost (the box where a prefab, model or mesh
	// lands, the mesh a material goes to) and its label at the pointer. Owned by EditorViewport, fed by the Scene view's panel
	// in every frame it renders and read by its renderer in the same frame. It writes nothing: no entity, component, undo
	// step, selection or version. It casts again only when what it depends on changed (the pointer's ray, the render scene's
	// updates, a finished load), and the drop runs asset.place with the request it placed by (MakeDropParameters), so the drop
	// lands where the ghost stood. Main thread only.
	class PlacementPreview
	{
	public:
		enum class Kind : uint8_t
		{
			None = 0, // Nothing is dragged over the view
			Place,    // A prefab, model or mesh: GetPlacement says where
			Material, // A material over a mesh renderer: GetTarget
			NoTarget, // A material over anything else
			Refused   // An asset that cannot be placed, or a view that takes none now (canvas mode, a scene being opened)
		};
		enum class LabelStyle : uint8_t
		{
			Normal = 0,
			Quiet,
			Warning
		};

		// How strongly the box's edges hidden behind the scene show (a fraction of the hit color's alpha).
		static constexpr float c_HiddenEdgeAlpha = 0.3f;

		// Once per frame while an asset is dragged over the view's image: casts again only when what it depends on changed.
		void Update(EditorContext& context, const PlacementPointer& pointer);
		// The drag ended, left the view, or the view is hidden.
		void Clear();

		Kind GetKind() const { return m_Kind; }
		bool IsActive() const { return m_Kind != Kind::None; }
		// Kind::Place: where it lands.
		const std::optional<EditorPlacement::Placement>& GetPlacement() const { return m_Placement; }
		// Kind::Place: AssetBounds is still measuring its size (the ghost shows a marker meanwhile).
		bool IsSizePending() const { return m_SizePending; }
		// Kind::Material: the mesh renderer it would go to, and that entity's own bounds in world space.
		Entity GetTarget() const { return m_Target; }
		const AABB& GetTargetBounds() const { return m_TargetBounds; }
		// The text the view shows at the pointer (made when the state changes, not per frame).
		const std::string& GetLabel() const { return m_Label; }
		LabelStyle GetLabelStyle() const { return m_LabelStyle; }
		// Changes whenever the label does, so that the view rebuilds what it draws it with only then.
		uint64_t GetLabelVersion() const { return m_LabelVersion; }
		// asset.place's parameters for a drop now: {asset, ray, maxDistance, fallbackDistance, snap?} (Kind::Place); null
		// otherwise.
		nlohmann::json MakeDropParameters() const;
		// The ghost's lines: for Kind::Place the box where it lands (tested, its hidden edges faint, and a ring where it
		// touches) on a hit, only a box on top in the quieter color for a guess, a marker while its size loads; for
		// Kind::Material the target's box. Nothing for the other kinds.
		void Draw(DebugDraw& draw, const PlacementColors& colors) const;
		// Updates that cast (tests: a still pointer casts nothing).
		uint64_t GetCastCount() const { return m_CastCount; }
	private:
		// What an update depends on: equal to the last update's, nothing is cast, measured or formatted again.
		struct Key
		{
			AssetHandle Asset;
			glm::vec3 Origin = glm::vec3(0.0f);
			glm::vec3 Direction = glm::vec3(0.0f);
			float MaxDistance = 0.0f;
			float FallbackDistance = 0.0f;
			std::optional<float> Snap;
			uint64_t RenderScene = 0;
			uint64_t RenderSceneUpdates = 0;
			uint64_t ContentVersion = 0;
			bool SceneLoading = false;
			bool CanvasMode = false;
			bool Playing = false;

			bool operator==(const Key& other) const = default;
		};
		void SetLabel(std::string label, LabelStyle style);
	private:
		Kind m_Kind = Kind::None;
		std::optional<Key> m_Key;
		AssetHandle m_Asset;
		PlacementPointer m_Pointer;
		std::optional<EditorPlacement::Placement> m_Placement;
		bool m_SizePending = false;
		Entity m_Target;
		AABB m_TargetBounds;
		std::string m_Label;
		LabelStyle m_LabelStyle = LabelStyle::Normal;
		uint64_t m_LabelVersion = 0;
		uint64_t m_CastCount = 0;
	};

}
