#pragma once

// GameUIScene's internal data, shared by its implementation files (GameUIScene.cpp, GameUILayout.cpp, GameUIText.cpp,
// GameUIQuads.cpp). Not part of the module's interface.

#include "Strata/Core/UUID.h"
#include "Strata/GameUI/GameUIScene.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace Strata
{

	constexpr uint32_t c_NoElement = ~0u;

	// A UI element of a root canvas, in depth-first hierarchy order (the canvas is element 0).
	struct GameUIScene::Element
	{
		entt::entity Entity = entt::null;
		uint32_t Parent = c_NoElement; // The nearest ancestor that is an element
		uint32_t SubtreeEnd = 0;       // One past its last descendant: its subtree is [index, SubtreeEnd)
		bool Active = true;            // WorldTransformComponent::ActiveInHierarchy as last seen
		bool RaycastTarget = false;    // Whether hit tests see it (GameUIScene::IsRaycastTarget) as last seen
		// Widget parts placed or shown by another element: a slider's fill or handle (its anchors follow the value) and a
		// toggle's checkmark (drawn while the toggle is on).
		uint32_t SliderOf = c_NoElement;
		bool SliderHandle = false;
		uint32_t CheckmarkOf = c_NoElement;
		// A selectable and the image it tints (itself, or its TargetImage), both ways.
		uint32_t TintTarget = c_NoElement;
		uint32_t TintFrom = c_NoElement;
		// Masks: 1 + the clip rect of the nearest mask above the element (0: none), and its own mask's (c_NoElement: none).
		uint32_t Clip = 0;
		uint32_t MaskSlot = c_NoElement;
		// Of a slider, toggle or selectable: the parts it named when they were resolved (a change of them rebuilds the canvas).
		UUID FillID = UUID::Null();
		UUID HandleID = UUID::Null();
		UUID CheckmarkID = UUID::Null();
		UUID TargetImageID = UUID::Null();
	};

	// Shaped text of an element in one layout, with the options it was shaped with.
	struct GameUIScene::ElementText
	{
		bool Valid = false;
		TextShapeOptions Options;
		AssetHandle ShapedFont = UUID::Null(); // The font it was shaped with (null: the default font)
		ShapedText Shaped;
	};

	// A canvas laid out for one size in canvas units.
	struct GameUIScene::Layout
	{
		glm::vec2 UnitSize = glm::vec2(0.0f);
		uint64_t LastUsed = 0;
		uint64_t Id = 0;       // Unique in the scene's UI
		uint64_t HitStamp = 0; // Changes with what hit tests see (boxes, visibility, raycast targets)
		std::vector<UIBox> Boxes;           // Per element
		std::vector<ElementText> Texts;     // Per element (used by elements with text)
		std::vector<uint32_t> Dirty;        // Elements marked since the last update
		std::vector<uint8_t> DirtyMarks;    // Per element: in Dirty
		bool AllDirty = true;               // Everything is laid out again (new layouts, lost changes)

		// The clip rects of the canvas's masks (by mask slot), from the boxes; recomputed when boxes or masks change.
		std::vector<RenderData::UIClipRect> Clips;
		uint64_t ClipVersion = 0;
		bool ClipsDirty = true;

		// Quads: each element has a slot of QuadCapacity quads from QuadFirst, in element order; unused quads are hidden.
		std::vector<RenderData::UIQuad> Quads;
		std::vector<uint32_t> QuadFirst;
		std::vector<uint32_t> QuadCapacity;
		std::vector<uint32_t> QuadsDirty;
		std::vector<uint8_t> QuadsDirtyMarks;
		bool QuadsRepack = true;            // The slots are rebuilt (sizes changed, elements were added or removed)
		uint64_t QuadArrayId = 0;
		uint64_t QuadVersion = 0;
		struct QuadChange
		{
			uint64_t Version = 0;
			UIQuadRange Range;
		};
		std::deque<QuadChange> QuadChanges;
		uint64_t QuadChangesFloor = 0;      // Changes up to this version may be missing
		std::unordered_map<uint32_t, uint32_t> FontUses; // Scene font index -> glyph quads of this array
	};

	struct GameUIScene::Canvas
	{
		entt::entity Entity = entt::null;
		std::vector<Element> Elements;
		std::vector<uint32_t> Masks; // Elements with a UIMaskComponent, in element order (their slots)
		std::vector<Layout> Layouts;
		bool StructureDirty = true;
	};

}
