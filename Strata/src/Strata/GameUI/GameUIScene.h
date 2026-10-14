#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIRenderData.h"
#include "Strata/Renderer/TextLayout.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class Scene;

	// A screen the UI is shown on: the viewport's size in pixels and the display's content scale (1 = 96 DPI).
	struct UIScreen
	{
		glm::vec2 Size = { 1920.0f, 1080.0f };
		float ContentScale = 1.0f;

		bool operator==(const UIScreen& other) const = default;
	};

	// A rect in canvas units (+Y down). Contains is half-open: the right and bottom edges belong to the next rect.
	struct UIBox
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Size = glm::vec2(0.0f);

		glm::vec2 GetMax() const { return Min + Size; }
		glm::vec2 GetCenter() const { return Min + Size * 0.5f; }
		bool Contains(const glm::vec2& point) const
		{
			return point.x >= Min.x && point.y >= Min.y && point.x < Min.x + Size.x && point.y < Min.y + Size.y;
		}
	};

	// What the UI did since the counters were last read (GameUIScene::TakeStats): the work follows what changed.
	struct GameUISceneStats
	{
		uint64_t ElementsLaidOut = 0; // Elements whose rects were computed
		uint64_t TextsShaped = 0;     // ShapeText calls (measuring and drawing)
		uint64_t QuadsBuilt = 0;      // Quads written
		uint64_t FullRebuilds = 0;    // Layouts of a whole canvas (a new screen size, the scene no longer knew what changed)
	};

	// A root canvas as a screen shows it (GameUIScene::EnsureLayout).
	struct UICanvasView
	{
		entt::entity Canvas = entt::null;
		float Scale = 1.0f;                  // Pixels per canvas unit
		glm::vec2 UnitSize = glm::vec2(0.0f); // The screen in canvas units
		int32_t SortOrder = 0;
		bool ReceivesInput = true;
		bool PixelSnap = true;
		uint32_t CanvasIndex = 0; // Internal: of the scene's root canvases
		uint32_t LayoutIndex = 0; // Internal: of the canvas's layouts
	};

	// How a selectable shows (its tint or swapped image): disabled, else pressed, hovered, focused or normal.
	enum class UIVisualState : uint8_t
	{
		Normal = 0,
		Hover,
		Pressed,
		Focused,
		Disabled
	};

	// The screen the game shows its UI on (the pixels of the image it renders: the game's window, the editor's game view;
	// c_DefaultScreen without one), and the factor from input coordinates (Input::GetMousePosition, in the input viewport)
	// to its pixels.
	struct UIInputScreen
	{
		UIScreen Screen;
		glm::vec2 InputToPixels = glm::vec2(1.0f);
	};

	// A changed range of a canvas's quads (GameUIScene::GetQuadChanges).
	struct UIQuadRange
	{
		uint32_t First = 0;
		uint32_t Count = 0;
	};

	// The game UI of a scene: its root canvases, their elements in hierarchy order, the layout of each canvas for the
	// screens it is shown on, the shaped texts and the quads the UI pass draws. One per scene, in its registry's context,
	// created on first use (Get) and shared by the UI system, the renderers' UI passes, hit testing and the editor's
	// commands. It holds asset handles, never Refs or GPU objects (a scene may be released on a background thread). Main
	// thread only.
	//
	// The UI contract (like the transform contract): it listens to the signals of the UI components (on_construct,
	// on_update, on_destroy), reads the scene's hierarchy moves (Scene::GetHierarchyMoves) and activity changes
	// (Scene::GetWorldTransformChanges, filtered by its element index, so moving bodies cost a lookup each), and
	// recomputes only what changed: code that writes UI component fields directly must signal it (MarkModified,
	// registry.patch). A change marks its element; layout recomputes the element's subtree, starting at the highest
	// ancestor whose rect depends on its children (a layout group, or a rect fitting its content). Layout is lazy:
	// EnsureLayout lays a screen out when the pass or the system asks for it, and keeps up to c_MaxLayoutsPerCanvas
	// layouts per canvas, keyed by the canvas's size in units, so a window resized at the same aspect ratio under
	// ScaleWithScreen lays out nothing again. Rects are kept in canvas units; pixels are units * Scale.
	class GameUIScene
	{
	public:
		static constexpr UIScreen c_DefaultScreen = {};
		static constexpr uint32_t c_MaxLayoutsPerCanvas = 4;
		// Changed quad ranges remembered per layout (GetQuadChanges); older ones mean a full upload.
		static constexpr size_t c_MaxQuadChanges = 256;

		// The scene's UI, created on first use. It lives as long as the scene's registry.
		static GameUIScene& Get(Scene& scene);
		static GameUIScene* Find(Scene& scene);
		// Whether the scene has any UICanvasComponent: one storage-size check.
		static bool HasCanvases(const Scene& scene);

		// A root canvas's scale (pixels per unit) on a screen.
		static float ComputeScale(const UICanvasComponent& canvas, const UIScreen& screen);
		// The screen of the game's UI input: what the game renders (IGameWindow's size and content scale), else the input
		// viewport, else c_DefaultScreen (headless editors and games, tests), so hit tests use the layout that is drawn.
		static UIInputScreen GetInputScreen();

		~GameUIScene();
		GameUIScene(const GameUIScene&) = delete;
		GameUIScene& operator=(const GameUIScene&) = delete;

		// Lays every root canvas out for the screen (only what changed since that screen's layout) and returns them in draw
		// order: by SortOrder, then in hierarchy order. Valid until the next EnsureLayout or a change of the scene.
		std::span<const UICanvasView> EnsureLayout(const UIScreen& screen);
		// The scene's root canvases, counted once per change of the canvases or the hierarchy (hidden UI costs this).
		uint32_t GetRootCanvasCount();

		// The elements of a canvas view (EnsureLayout) in hierarchy order: the canvas first, parents before children.
		uint32_t GetElementCount(const UICanvasView& view) const;
		entt::entity GetElementEntity(const UICanvasView& view, uint32_t element) const;
		UIBox GetElementBox(const UICanvasView& view, uint32_t element) const; // Canvas units
		// Active in the hierarchy and not the checkmark of a toggle that is off (with its descendants).
		bool IsElementVisible(const UICanvasView& view, uint32_t element) const;
		// Whether the pointer can hit the element: it has a UIImage or UIText that is a raycast target, or a UISelectable.
		bool IsRaycastTarget(const UICanvasView& view, uint32_t element) const;
		// The view and element of an entity in the canvases of the latest EnsureLayout of `screen`; false for entities that
		// are not laid-out UI elements.
		bool FindElement(entt::entity entity, const UIScreen& screen, UICanvasView& outView, uint32_t& outElement);

		// Identifies a canvas view's layout and what hit tests see of it (boxes, visibility, raycast targets): a hit test
		// grid built for the same id and stamp is current.
		uint64_t GetLayoutId(const UICanvasView& view) const;
		uint64_t GetHitStamp(const UICanvasView& view) const;

		// The quads of a canvas view, built for what changed since they were last built.
		const std::vector<RenderData::UIQuad>& EnsureQuads(const UICanvasView& view);
		// Identifies the array EnsureQuads returns: changes when it is rebuilt whole (its size or slots changed), so
		// renderers upload it whole; otherwise GetQuadChanges lists what changed since a version.
		uint64_t GetQuadArrayId(const UICanvasView& view) const;
		uint64_t GetQuadVersion(const UICanvasView& view) const;
		// Appends the ranges written after quad version `sinceVersion`; false when they are no longer known (upload it whole).
		bool GetQuadChanges(const UICanvasView& view, uint64_t sinceVersion, std::vector<UIQuadRange>& outRanges) const;

		// What the quads refer to: UI texture index -> texture (with the number of quads that use each, 0 for textures no quad
		// uses any more), scene font index -> font (index 0 and null handles: the engine's default font), the distinct glyphs
		// the quads of each font use, and the fonts a canvas view's quads use.
		std::span<const AssetHandle> GetTextures() const { return m_Textures; }
		std::span<const uint32_t> GetTextureUses() const { return m_TextureUses; }
		std::span<const AssetHandle> GetFonts() const { return m_Fonts; }
		void GetFontGlyphs(uint32_t font, std::vector<uint32_t>& outGlyphs) const;
		void GetCanvasFonts(const UICanvasView& view, std::vector<uint32_t>& outFonts) const;
		// Unique in the process, so that renderers tell scenes' UIs apart (a new one may reuse an address).
		uint64_t GetId() const { return m_Id; }
		// Changes whenever the textures, the fonts or a font's glyph set change.
		uint64_t GetResourceVersion() const { return m_ResourceVersion; }

		// The state a selectable shows, which the UI system sets while the scene plays (input), with the tint it fades to (or
		// through); the quads of the image it targets follow. Without one, a selectable shows its rest state: Disabled when not
		// Interactable, else Normal, with that state's color.
		void SetSelectableState(entt::entity selectable, UIVisualState state, const glm::vec4& tint);
		void ClearSelectableState(entt::entity selectable);
		void ClearSelectableStates();
		UIVisualState GetVisualState(entt::entity selectable, const UISelectableComponent& component) const;
		glm::vec4 GetTintColor(entt::entity selectable, const UISelectableComponent& component, UIVisualState state) const;

		// Compares every cached layout and visibility with a full recomputation (within 1e-4 canvas units). False with the
		// first difference, naming the entity (whose write was not signaled). Debug builds check it after every
		// EnsureLayout.
		bool Validate(std::string* outError = nullptr);

		// The counters since the previous call.
		GameUISceneStats TakeStats();
	private:
		struct Element;
		struct Canvas;
		struct Layout;
		struct ElementText;
		struct ElementRef
		{
			entt::entity Entity = entt::null;
			uint32_t Canvas = ~0u;
			uint32_t Element = ~0u;
		};

		explicit GameUIScene(Scene& scene);
		friend struct GameUISceneContext;

		// Signals of the UI components.
		void OnCanvasAddedOrRemoved(entt::registry& registry, entt::entity entity);
		void OnCanvasChanged(entt::registry& registry, entt::entity entity);
		void OnStructureChanged(entt::registry& registry, entt::entity entity);
		void OnLayoutChanged(entt::registry& registry, entt::entity entity);
		void OnVisualChanged(entt::registry& registry, entt::entity entity);
		void OnTextChanged(entt::registry& registry, entt::entity entity);
		// The components that make raycast targets, before they are removed (they are still there during the signal).
		void OnImageDestroyed(entt::registry& registry, entt::entity entity);
		void OnTextDestroyed(entt::registry& registry, entt::entity entity);
		void OnSelectableDestroyed(entt::registry& registry, entt::entity entity);
		// What hit tests see of an element changed when it became or stopped being a raycast target, which an element's
		// flag remembers: other changes of its image, text or selectable (colors, texts) leave hit test grids alone.
		void UpdateRaycastTarget(entt::entity entity, bool ignoreImage = false, bool ignoreText = false, bool ignoreSelectable = false);
		bool ComputeRaycastTarget(entt::entity entity, bool ignoreImage = false, bool ignoreText = false, bool ignoreSelectable = false) const;
		void OnSliderChanged(entt::registry& registry, entt::entity entity);
		void OnToggleChanged(entt::registry& registry, entt::entity entity);

		// Changes since the last update: structure, hierarchy moves, activity, assets.
		void ProcessChanges();
		void RebuildCanvasList();
		void RebuildCanvas(uint32_t canvasIndex);
		// Widget parts (slider fills and handles, toggle checkmarks, selectables' target images) of a canvas's elements.
		void ResolveParts(uint32_t canvasIndex);
		// The root canvas the entity is in (or is), or ~0u.
		uint32_t FindRootCanvas(entt::entity entity) const;
		const ElementRef* FindRef(entt::entity entity) const;
		bool DependsOnChildren(uint32_t element, const Canvas& canvas) const;
		bool IsVisible(const Canvas& canvas, uint32_t element) const;
		// Texts and images that use assets which loaded, reloaded or went (empty: any asset).
		void RefreshAssetUsers(std::span<const AssetHandle> changed);
		void MarkElementLayoutDirty(uint32_t canvasIndex, uint32_t element);
		void MarkElementQuadsDirty(uint32_t canvasIndex, uint32_t element);
		void MarkSubtreeQuadsDirty(uint32_t canvasIndex, uint32_t element);
		void MarkCanvasDirty(uint32_t canvasIndex);
		// What hit tests see of the canvas changed (in every layout).
		void TouchHits(uint32_t canvasIndex);

		// Layout (GameUILayout.cpp).
		Layout& GetLayout(Canvas& canvas, const glm::vec2& unitSize, uint32_t& outIndex);
		void UpdateLayout(Canvas& canvas, Layout& layout);
		// Lays out the subtree of `root` into `boxes`, the root placed in its parent's box (the root rect for the canvas).
		void LayOutSubtree(const Canvas& canvas, Layout& layout, uint32_t root, std::vector<UIBox>& boxes);
		UIBox GetRootBox(const Canvas& canvas, const glm::vec2& unitSize) const;
		// One axis (0: x, 1: y) of a laid-out subtree: each element's position and size in its parent's box, or where its
		// parent's layout group places it. Widths come first: text heights depend on them.
		void LayOutAxis(const Canvas& canvas, Layout& layout, uint32_t root, int axis, std::vector<UIBox>& boxes);
		void PlaceGroupChildren(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t group, int axis, std::vector<UIBox>& boxes);
		// What an element asks of a layout group on an axis: its content when its rect fits it, else its Size.
		float GetPreferredSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t element, int axis, const std::vector<UIBox>& boxes);
		// The size of an element's content on an axis: its group's children with padding, or its text.
		float GetContentSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t element, int axis, const std::vector<UIBox>& boxes);
		float GetGroupContentSize(const Canvas& canvas, Layout& layout, uint32_t root, uint32_t group, int axis, const std::vector<UIBox>& boxes);
		// A grid's columns for its inner width.
		static uint32_t GetGridColumns(const UILayoutComponent& group, float innerWidth, uint32_t children);
		// Whether a layout group places the element (it is active, with its rect not ignoring layout).
		bool IsPlacedByGroup(const Canvas& canvas, uint32_t element) const;

		// Text (GameUIText.cpp): the font face a text is shaped with (the default font while its own loads), and shaping
		// with a cache per element and layout.
		const FontFace* GetTextFace(const UITextComponent& text, AssetHandle& outShapedFont);
		TextShapeOptions GetShapeOptions(const UITextComponent& text, const UIRectComponent* rect, const UIBox& box) const;
		TextShapeOptions GetMeasureOptions(const UITextComponent& text, float wrapWidth) const;
		const ShapedText& ShapeElementText(Layout& layout, uint32_t element, const UITextComponent& text, const TextShapeOptions& options);

		// Quads (GameUIQuads.cpp).
		void BuildQuads(Canvas& canvas, Layout& layout);
		uint32_t WriteElementQuads(Canvas& canvas, Layout& layout, uint32_t element, std::vector<RenderData::UIQuad>& out);
		uint32_t GetTextureIndex(AssetHandle texture);
		uint32_t GetFontIndex(AssetHandle font);
		void RecordQuadChange(Layout& layout, uint32_t first, uint32_t count);
		void ReleaseLayoutGlyphs(Layout& layout);
		// Counts what a quad written into (`delta` 1) or removed from (-1) a layout's array uses: glyphs, fonts, textures.
		void CountQuadUses(Layout& layout, const RenderData::UIQuad& quad, int delta);
	private:
		Scene* m_Scene = nullptr;
		entt::registry* m_Registry = nullptr;

		std::vector<Canvas> m_Canvases;          // Root canvases, in hierarchy order of their entities
		std::vector<ElementRef> m_ElementRefs;   // By entity index: where an element is
		std::vector<entt::entity> m_PendingStructure; // Entities whose canvas is looked for at the next update
		std::vector<UICanvasView> m_Views;       // The latest EnsureLayout's result
		UIScreen m_ViewsScreen;
		bool m_ViewsValid = false;
		bool m_CanvasListDirty = true;
		uint64_t m_HierarchyVersion = 0;         // Moves read up to it
		uint64_t m_TransformsVersion = 0;        // Activity changes read up to it
		uint64_t m_AssetContentVersion = 0;      // Asset changes read up to it
		uint64_t m_LayoutStamp = 0;              // EnsureLayout calls (layouts used longest ago are replaced first)
		uint32_t m_RootCanvasCount = 0;
		uint64_t m_RootCanvasCountVersion = ~0ull; // Hierarchy version of the count
		uint64_t m_CanvasSetStamp = 1;           // Canvases added or removed
		uint64_t m_RootCanvasCountStamp = 0;
		uint64_t m_NextQuadArrayId = 1;
		uint64_t m_NextLayoutId = 1;
		uint64_t m_HitStamp = 1;
		std::vector<entt::entity> m_ChangeScratch;
		std::vector<AssetHandle> m_AssetScratch;
		struct WalkFrame
		{
			entt::entity Entity = entt::null;
			uint32_t ParentElement = ~0u;
		};
		std::vector<WalkFrame> m_WalkScratch;

		std::vector<AssetHandle> m_Textures;                 // UI texture index -> texture
		std::unordered_map<AssetHandle, uint32_t> m_TextureIndices;
		std::vector<AssetHandle> m_Fonts;                    // Scene font index -> font (0: the default font)
		std::unordered_map<AssetHandle, uint32_t> m_FontIndices;
		std::vector<std::unordered_map<uint32_t, uint32_t>> m_FontGlyphUses; // Per font: glyph -> quads using it
		std::vector<uint32_t> m_TextureUses;                 // Per UI texture index: quads using it
		uint64_t m_Id = 0;
		std::unordered_set<AssetHandle> m_WatchedAssets;     // Fonts and textures the UI asked for
		uint64_t m_ResourceVersion = 1;

		// Layout scratch, per element of the subtree being laid out (indexed from its root).
		std::vector<float> m_PreferredMemo;
		std::vector<uint8_t> m_PreferredKnown;
		std::vector<float> m_PlacedPosition;
		std::vector<float> m_PlacedSize;
		std::vector<uint8_t> m_Placed;
		std::vector<UIBox> m_BoxScratch;
		std::vector<RenderData::UIQuad> m_QuadScratch;
		struct SelectableState
		{
			UIVisualState State = UIVisualState::Normal;
			glm::vec4 Tint = glm::vec4(1.0f);
		};
		std::unordered_map<entt::entity, SelectableState> m_SelectableStates;
		ShapedText m_ValidateShaped;

		GameUISceneStats m_Stats;
		bool m_Validating = false; // Validate's recomputation counts nothing and caches nothing
	};

}
