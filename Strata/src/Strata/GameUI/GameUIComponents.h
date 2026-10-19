#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Strata
{

	// The game UI's components (the GameUI module, registered by RegisterGameUIModule).
	//
	// Coordinates are canvas units with +Y down and (0, 0) at the top left, like TextComponent::ScreenAnchor and
	// Input::GetMousePosition; a root canvas maps them to pixels with its scale (see UICanvasComponent). Colors are display
	// (sRGB) colors, like TextComponent::Color. An entity is a UI element when it has a UIRectComponent and a root canvas (a
	// UICanvasComponent without a canvas among its ancestors) is the entity itself or one of its ancestors. A canvas under
	// another canvas is an element of the outer one (its scale settings are ignored); UI components outside any canvas, or
	// without a UIRectComponent, draw nothing.
	//
	// The UI contract (like the transform contract): code that writes these components' fields directly signals the change
	// (Entity::MarkModified, registry.patch; ComponentAccess, the script API and the UI system do), so that the scene's UI
	// layout (GameUIScene) recomputes what changed. Values written right after AddComponent need no signal.

	enum class UICanvasScaleMode : uint8_t
	{
		ConstantPixelSize = 0, // One canvas unit is ScaleFactor pixels
		ScaleWithScreen,       // The canvas keeps the proportions of ReferenceResolution on every screen
		ConstantPhysicalSize   // One canvas unit is ScaleFactor pixels at 96 DPI (the screen's content scale times ScaleFactor)
	};

	enum class UICanvasRenderMode : uint8_t
	{
		ScreenSpace = 0, // Over the scene, laid out for the screen (the scale mode)
		WorldSpace       // A plane in the scene at the entity's transform
	};

	// A screen of UI. A root canvas lays its elements out for the screen it is shown on: at a scale s (pixels per canvas
	// unit) over a size of screen / s canvas units, inset by SafeAreaPadding. Root canvases draw over the scene in SortOrder
	// (higher on top), and the topmost one gets input first.
	//
	// A WorldSpace canvas is a sign, a screen or a name tag in the scene instead: ReferenceResolution canvas units wide and
	// high (the scale settings do nothing), each WorldUnitsPerUnit world units, in the entity's XY plane facing +Z (like world
	// text) with the point its UIRect's Pivot names at the entity's origin. Every view of the scene draws it depth-tested
	// like world text (editor views too: it is no screen-space UI), and the pointer hits it where the game camera's ray
	// through the pointer crosses it, below every screen-space canvas.
	struct UICanvasComponent
	{
		UICanvasScaleMode ScaleMode = UICanvasScaleMode::ScaleWithScreen;
		glm::vec2 ReferenceResolution = { 1920.0f, 1080.0f }; // ScaleWithScreen: the screen the layout is designed for
		// ScaleWithScreen: 0 scales with the screen's width, 1 with its height, between them with both (logarithmically).
		float MatchWidthOrHeight = 0.5f;
		float ScaleFactor = 1.0f;             // Multiplies the scale of the constant modes
		glm::vec4 SafeAreaPadding = glm::vec4(0.0f); // Left, top, right, bottom insets of the root rect, in canvas units
		int32_t SortOrder = 0;
		bool PixelSnap = true;     // Rect edges and text baselines on whole pixels
		bool ReceivesInput = true; // False: drawn, but never hit or navigated (a HUD that never blocks clicks)
		UUID FirstSelected = UUID::Null(); // The selectable focused when navigation starts
		UICanvasRenderMode RenderMode = UICanvasRenderMode::ScreenSpace;
		float WorldUnitsPerUnit = 0.01f;   // WorldSpace: world units per canvas unit (a canvas 100 units wide is a meter)
	};

	enum class UIFitContent : uint8_t
	{
		None = 0,
		Width,
		Height,
		Both
	};

	// The rect of a UI element in its parent element's rect (the root rect for a canvas's children). Anchor points are
	// A0 = parent.min + AnchorMin * parent.size and A1 = parent.min + AnchorMax * parent.size; the size is
	// max(A1 - A0 + Size, 0) and the pivot point A0 + (A1 - A0) * Pivot + Position, around which the rect lies by Pivot.
	// Point anchors with Size (200, 50) are a 200x50 rect; stretched anchors (0, 0)-(1, 1) with Size (-20, -20) inset the
	// parent by 10. A parent with a UILayoutComponent places its children itself (unless IgnoreLayout).
	struct UIRectComponent
	{
		glm::vec2 AnchorMin = { 0.5f, 0.5f };
		glm::vec2 AnchorMax = { 0.5f, 0.5f };
		glm::vec2 Pivot = { 0.5f, 0.5f };
		glm::vec2 Position = { 0.0f, 0.0f }; // The pivot's offset from the anchor point, in canvas units
		glm::vec2 Size = { 100.0f, 100.0f }; // Added to the anchored span, in canvas units
		UIFitContent FitContent = UIFitContent::None; // The rect takes its content's size on these axes (its text, or its layout's children)
		bool IgnoreLayout = false; // A layout-group parent leaves it where its anchors put it
	};

	enum class UIImageType : uint8_t
	{
		Simple = 0,
		Sliced, // Nine-slice: the borders keep their size while the edges and the center stretch
		Filled  // Cut to FillAmount along FillDirection (health bars)
	};

	enum class UIFillDirection : uint8_t
	{
		LeftToRight = 0,
		RightToLeft,
		TopToBottom,
		BottomToTop
	};

	// A rectangle filling the element's rect: a texture, or a solid color without one, with optional rounded corners and a
	// border.
	struct UIImageComponent
	{
		AssetHandle Texture = UUID::Null();    // None: a solid color
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // Tints the texture
		UIImageType Type = UIImageType::Simple;
		glm::vec4 UVRect = { 0.0f, 0.0f, 1.0f, 1.0f };  // u0, v0, u1, v1 of the texture
		glm::vec4 SliceBorders = glm::vec4(0.0f);       // Sliced: left, top, right, bottom borders in texels
		float SliceScale = 1.0f;                        // Sliced: canvas units per border texel
		float FillAmount = 1.0f;                        // Filled: the fraction shown, 0-1
		UIFillDirection FillDirection = UIFillDirection::LeftToRight;
		bool PreserveAspect = false; // Simple and Filled images keep the texture's aspect ratio, centered in the rect
		float CornerRadius = 0.0f;   // Canvas units
		float BorderWidth = 0.0f;    // Canvas units, inside the rect
		glm::vec4 BorderColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		bool RaycastTarget = true;   // Hit by the pointer (blocks the game's clicks)
	};

	enum class UITextVerticalAlignment : uint8_t
	{
		Top = 0,
		Middle,
		Bottom
	};

	enum class UITextOverflow : uint8_t
	{
		Overflow = 0, // Lines beyond the rect's height are drawn outside it
		Truncate,     // Lines that do not fit are left out
		Ellipsis      // Like Truncate, with "..." ending the last line shown
	};

	// UTF-8 text in the element's rect ('\n' starts a line), measured and wrapped without rasterizing (FontFace).
	struct UITextComponent
	{
		std::string Text = "Text";
		AssetHandle Font = UUID::Null(); // The default engine font when unset (or while the font loads)
		float FontSize = 32.0f;          // Canvas units per em
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		TextAlignment Alignment = TextAlignment::Center;
		UITextVerticalAlignment VerticalAlignment = UITextVerticalAlignment::Middle;
		bool Wrap = true; // Lines wrap at the rect's width (between words, inside words longer than a line)
		UITextOverflow Overflow = UITextOverflow::Overflow;
		float LineSpacing = 1.0f;  // Times the font's line height
		float OutlineWidth = 0.0f; // Em
		glm::vec4 OutlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		bool RaycastTarget = false;
	};

	enum class UISelectableTransition : uint8_t
	{
		None = 0,
		ColorTint, // The target image's color is multiplied by the state's color, fading over FadeDuration
		ImageSwap  // The target image shows the state's texture (its own texture in the normal state)
	};

	enum class UINavigation : uint8_t
	{
		Automatic = 0, // The nearest selectable in the direction
		Explicit,      // The entities named by NavigateUp, NavigateDown, NavigateLeft and NavigateRight
		None           // Never reached or left by navigation
	};

	// The interaction states of a widget (buttons, toggles, sliders): hover, press, focus and disabled, how they show, and
	// how keyboard and gamepad navigation moves from it.
	struct UISelectableComponent
	{
		bool Interactable = true; // False: disabled (never hovered, pressed, focused, and raises no events)
		UISelectableTransition Transition = UISelectableTransition::ColorTint;
		UUID TargetImage = UUID::Null(); // The element whose image shows the states; none: the entity's own UIImage
		glm::vec4 NormalColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 HoverColor = { 0.9f, 0.9f, 0.9f, 1.0f };
		glm::vec4 PressedColor = { 0.75f, 0.75f, 0.75f, 1.0f };
		glm::vec4 FocusedColor = { 0.95f, 0.95f, 0.95f, 1.0f };
		glm::vec4 DisabledColor = { 0.75f, 0.75f, 0.75f, 0.5f };
		AssetHandle HoverImage = UUID::Null();
		AssetHandle PressedImage = UUID::Null();
		AssetHandle FocusedImage = UUID::Null();
		AssetHandle DisabledImage = UUID::Null();
		float FadeDuration = 0.1f; // Seconds of unscaled time
		UINavigation Navigation = UINavigation::Automatic;
		UUID NavigateUp = UUID::Null();
		UUID NavigateDown = UUID::Null();
		UUID NavigateLeft = UUID::Null();
		UUID NavigateRight = UUID::Null();
	};

	// Clicks: on release over the button it was pressed on, or Submit while it has the focus.
	struct UIButtonComponent
	{
		UUID EventTarget = UUID::Null(); // Also receives the click (its scripts' OnUIClick), besides the button's own entity
	};

	// A switch that flips on click. Toggles naming the same Group are exclusive: turning one on turns the others off.
	struct UIToggleComponent
	{
		bool IsOn = false;
		UUID Checkmark = UUID::Null(); // An element drawn only while the toggle is on
		UUID Group = UUID::Null();
		UUID EventTarget = UUID::Null();
	};

	// A value between MinValue and MaxValue, dragged with the pointer or stepped with the navigation keys.
	struct UISliderComponent
	{
		float MinValue = 0.0f;
		float MaxValue = 1.0f;
		float Value = 0.0f;
		bool WholeNumbers = false;
		UIFillDirection Direction = UIFillDirection::LeftToRight;
		UUID Fill = UUID::Null();   // An element anchored from the start of the slider to the value
		UUID Handle = UUID::Null(); // An element anchored at the value
		float KeyboardStep = 0.0f;  // The step of the navigation keys; 0: a tenth of the range
		UUID EventTarget = UUID::Null();
	};

	enum class UILayoutType : uint8_t
	{
		Horizontal = 0,
		Vertical,
		Grid
	};

	enum class UIChildAlignment : uint8_t
	{
		TopLeft = 0,
		TopCenter,
		TopRight,
		MiddleLeft,
		MiddleCenter,
		MiddleRight,
		BottomLeft,
		BottomCenter,
		BottomRight
	};

	// Places the element's active children with a UIRectComponent (not IgnoreLayout) in sibling order: stacked vertically or
	// horizontally at their preferred sizes, or in a grid of CellSize cells, inside the padded rect.
	struct UILayoutComponent
	{
		UILayoutType Type = UILayoutType::Vertical;
		glm::vec4 Padding = glm::vec4(0.0f); // Left, top, right, bottom, in canvas units
		glm::vec2 Spacing = glm::vec2(0.0f); // Between children (x: horizontal, y: vertical)
		UIChildAlignment ChildAlignment = UIChildAlignment::TopLeft; // Where the block of children sits in the padded rect
		bool StretchChildren = false;           // Children fill the cross axis
		glm::vec2 CellSize = { 100.0f, 100.0f }; // Grid
		uint32_t Columns = 0;                    // Grid: 0 fits as many cells as the width holds
	};

	// A line of text the player types while the field has the focus (it needs a UISelectable to be focused): typed
	// characters go in at the caret, Backspace and Delete remove, Left, Right, Home and End move the caret (with Shift they
	// select), Ctrl+A (Cmd+A) selects everything, Enter submits (OnUISubmit) and Escape stops editing. While it is edited the
	// keyboard is the UI's (Input's keyboard capture). Its TextElement, an element with a UIText, shows the text in its font,
	// size, color and alignment on one line (scrolled to keep the caret in view; give the field a UIMask to clip what does not
	// fit); its Placeholder shows while the text is empty.
	struct UIInputFieldComponent
	{
		std::string Text;
		UUID TextElement = UUID::Null(); // An element with a UIText: shows the text, the caret and the selection
		UUID Placeholder = UUID::Null(); // An element shown while the text is empty
		uint32_t CharacterLimit = 0;     // Characters (code points) at most; 0: no limit
		glm::vec4 CaretColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		glm::vec4 SelectionColor = { 0.3f, 0.55f, 1.0f, 0.45f };
		UUID EventTarget = UUID::Null(); // Also receives the submit (its scripts' OnUISubmit), besides the field's own entity
	};

	// Clips the element's descendants to its rect inset by Padding: what lies outside is neither drawn nor hit by the
	// pointer (scrolling lists, cards whose content must not spill). Masks nest: a descendant of two shows only where both
	// rects overlap. The element's own image and text are clipped only by the masks above it.
	struct UIMaskComponent
	{
		glm::vec4 Padding = glm::vec4(0.0f); // Left, top, right, bottom insets of the clip rect, in canvas units
		float Softness = 0.0f;                // Canvas units over which content fades out toward the clip rect's edges
	};

}
