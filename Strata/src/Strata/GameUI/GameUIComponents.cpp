#include "stpch.h"
#include "Strata/GameUI/GameUIComponents.h"

#include "Strata/GameUI/GameUIRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"

namespace Strata
{

	namespace
	{

		constexpr float c_Unbounded = 1.0e6f;
		constexpr const char* c_Category = "UI";
		constexpr const char* c_RectName = "UIRect";

		PropertyOptions Range(float min, float max, float speed = 0.0f)
		{
			PropertyOptions options;
			options.Min = min;
			options.Max = max;
			options.Speed = speed;
			return options;
		}

		PropertyOptions Slider(float min, float max)
		{
			PropertyOptions options = Range(min, max);
			options.Flags = PropertyFlags::Slider;
			return options;
		}

		PropertyOptions Color()
		{
			PropertyOptions options;
			options.Color = true;
			return options;
		}

		PropertyOptions WithTooltip(PropertyOptions options, std::string tooltip)
		{
			options.Tooltip = std::move(tooltip);
			return options;
		}

		std::vector<EnumValue> FillDirections()
		{
			return { { "LeftToRight", 0, "Left To Right" }, { "RightToLeft", 1, "Right To Left" }, { "TopToBottom", 2, "Top To Bottom" },
				{ "BottomToTop", 3, "Bottom To Top" } };
		}

	}

	void RegisterGameUIComponents()
	{
		ComponentRegistry::Register<UICanvasComponent>("UICanvas")
			.DisplayName("UI Canvas")
			.Category(c_Category)
			.Description("A screen of game UI: lays its elements out for the screen it is shown on, scaled by its scale mode, and draws them over the scene")
			.Requires(c_RectName)
			.EnumProperty("ScaleMode", &UICanvasComponent::ScaleMode,
				{ { "ConstantPixelSize", 0, "Constant Pixel Size" }, { "ScaleWithScreen", 1, "Scale With Screen" }, { "ConstantPhysicalSize", 2, "Constant Physical Size" } },
				{ .Tooltip = "Constant Pixel Size: a canvas unit is Scale Factor pixels; Scale With Screen: the layout keeps the reference resolution's "
					"proportions on every screen; Constant Physical Size: a canvas unit is Scale Factor pixels at 96 DPI" })
			.Property("ReferenceResolution", &UICanvasComponent::ReferenceResolution,
				WithTooltip(Range(1.0f, 16384.0f, 1.0f), "Scale With Screen: the screen size in pixels the layout is designed for"))
			.Property("MatchWidthOrHeight", &UICanvasComponent::MatchWidthOrHeight,
				WithTooltip(Slider(0.0f, 1.0f), "Scale With Screen: 0 follows the screen's width, 1 its height, between them both"))
			.Property("ScaleFactor", &UICanvasComponent::ScaleFactor, WithTooltip(Range(0.01f, 100.0f, 0.01f), "Multiplies the scale of the constant modes"))
			.Property("SafeAreaPadding", &UICanvasComponent::SafeAreaPadding,
				WithTooltip(Range(0.0f, c_Unbounded, 1.0f), "Left, top, right and bottom insets of the canvas's rect, in canvas units"))
			.Property("SortOrder", &UICanvasComponent::SortOrder, WithTooltip({}, "Canvases with a higher order draw over the others and get input first"))
			.Property("PixelSnap", &UICanvasComponent::PixelSnap, WithTooltip({}, "Rect edges and text baselines on whole pixels"))
			.Property("ReceivesInput", &UICanvasComponent::ReceivesInput,
				WithTooltip({}, "Off: drawn, but never hit by the pointer or reached by navigation (a HUD that never blocks clicks)"))
			.EntityProperty("FirstSelected", &UICanvasComponent::FirstSelected, { .Tooltip = "The selectable focused when keyboard or gamepad navigation starts" });

		ComponentRegistry::Register<UIRectComponent>(c_RectName)
			.DisplayName("UI Rect")
			.Category(c_Category)
			.Description("The rectangle of a UI element: anchored in its parent's rect, offset and sized in canvas units")
			.Property("AnchorMin", &UIRectComponent::AnchorMin, WithTooltip(Range(0.0f, 1.0f, 0.005f), "Lower anchor in the parent's rect (0, 0 is its top-left corner)"))
			.Property("AnchorMax", &UIRectComponent::AnchorMax, WithTooltip(Range(0.0f, 1.0f, 0.005f), "Upper anchor in the parent's rect (1, 1 is its bottom-right corner)"))
			.Property("Pivot", &UIRectComponent::Pivot, WithTooltip(Range(0.0f, 1.0f, 0.005f), "The point of the rect Position places, in the rect (0, 0 is its top-left corner)"))
			.Property("Position", &UIRectComponent::Position, WithTooltip(Range(0.0f, 0.0f, 1.0f), "The pivot's offset from the anchor point, in canvas units (+Y down)"))
			.Property("Size", &UIRectComponent::Size, WithTooltip(Range(0.0f, 0.0f, 1.0f), "Added to the span between the anchors, in canvas units"))
			.EnumProperty("FitContent", &UIRectComponent::FitContent, { { "None", 0 }, { "Width", 1 }, { "Height", 2 }, { "Both", 3 } },
				{ .Tooltip = "The rect takes its content's size on these axes: its text, or the children its layout places" })
			.Property("IgnoreLayout", &UIRectComponent::IgnoreLayout, WithTooltip({}, "A parent's layout leaves this element where its anchors put it"));

		ComponentRegistry::Register<UIImageComponent>("UIImage")
			.DisplayName("UI Image")
			.Category(c_Category)
			.Description("Fills the element's rect with a texture or a solid color, optionally nine-sliced, filled, with rounded corners and a border")
			.Requires(c_RectName)
			.AssetProperty("Texture", &UIImageComponent::Texture, AssetType::Texture, { .Tooltip = "None: a solid color" })
			.Property("Color", &UIImageComponent::Color, WithTooltip(Color(), "Tints the texture (display color)"))
			.EnumProperty("Type", &UIImageComponent::Type, { { "Simple", 0 }, { "Sliced", 1 }, { "Filled", 2 } },
				{ .Tooltip = "Sliced: nine-slice, the borders keep their size; Filled: cut to Fill Amount (health bars)" })
			.Property("UVRect", &UIImageComponent::UVRect, WithTooltip(Range(-c_Unbounded, c_Unbounded, 0.005f), "The part of the texture shown: u0, v0, u1, v1"))
			.Property("SliceBorders", &UIImageComponent::SliceBorders, WithTooltip(Range(0.0f, c_Unbounded, 1.0f), "Sliced: left, top, right and bottom borders in texels"))
			.Property("SliceScale", &UIImageComponent::SliceScale, WithTooltip(Range(0.001f, 1000.0f, 0.01f), "Sliced: canvas units per border texel"))
			.Property("FillAmount", &UIImageComponent::FillAmount, WithTooltip(Slider(0.0f, 1.0f), "Filled: the fraction shown"))
			.EnumProperty("FillDirection", &UIImageComponent::FillDirection, FillDirections(), { .Tooltip = "Filled: where the shown part starts" })
			.Property("PreserveAspect", &UIImageComponent::PreserveAspect, WithTooltip({}, "Keeps the texture's aspect ratio, centered in the rect"))
			.Property("CornerRadius", &UIImageComponent::CornerRadius, WithTooltip(Range(0.0f, c_Unbounded, 0.5f), "Canvas units"))
			.Property("BorderWidth", &UIImageComponent::BorderWidth, WithTooltip(Range(0.0f, c_Unbounded, 0.5f), "Canvas units, inside the rect"))
			.Property("BorderColor", &UIImageComponent::BorderColor, Color())
			.Property("RaycastTarget", &UIImageComponent::RaycastTarget, WithTooltip({}, "Hit by the pointer, so the game does not get clicks on it"));

		ComponentRegistry::Register<UITextComponent>("UIText")
			.DisplayName("UI Text")
			.Category(c_Category)
			.Description("Text in the element's rect, aligned, wrapped and truncated in it")
			.Requires(c_RectName)
			.Property("Text", &UITextComponent::Text, { .Flags = PropertyFlags::MultiLine })
			.AssetProperty("Font", &UITextComponent::Font, AssetType::Font, { .Tooltip = "None: the engine's default font" })
			.Property("FontSize", &UITextComponent::FontSize, WithTooltip(Range(1.0f, 1024.0f, 0.5f), "Canvas units per em"))
			.Property("Color", &UITextComponent::Color, Color())
			.EnumProperty("Alignment", &UITextComponent::Alignment, { { "Left", 0 }, { "Center", 1 }, { "Right", 2 } })
			.EnumProperty("VerticalAlignment", &UITextComponent::VerticalAlignment, { { "Top", 0 }, { "Middle", 1 }, { "Bottom", 2 } })
			.Property("Wrap", &UITextComponent::Wrap, WithTooltip({}, "Lines wrap at the rect's width"))
			.EnumProperty("Overflow", &UITextComponent::Overflow, { { "Overflow", 0 }, { "Truncate", 1 }, { "Ellipsis", 2 } },
				{ .Tooltip = "Lines beyond the rect's height: drawn outside it, left out, or left out with \"...\" ending the last line shown" })
			.Property("LineSpacing", &UITextComponent::LineSpacing, WithTooltip(Range(0.5f, 3.0f, 0.01f), "Times the font's line height"))
			.Property("OutlineWidth", &UITextComponent::OutlineWidth, WithTooltip(Range(0.0f, 0.1f, 0.001f), "Em"))
			.Property("OutlineColor", &UITextComponent::OutlineColor, Color())
			.Property("RaycastTarget", &UITextComponent::RaycastTarget, WithTooltip({}, "Hit by the pointer, so the game does not get clicks on it"));

		ComponentRegistry::Register<UISelectableComponent>("UISelectable")
			.DisplayName("UI Selectable")
			.Category(c_Category)
			.Description("How a widget shows hover, press, focus and disabled, and how keyboard and gamepad navigation leaves it")
			.Requires(c_RectName)
			.Property("Interactable", &UISelectableComponent::Interactable, WithTooltip({}, "Off: disabled, never hovered, pressed or focused, and raises no events"))
			.EnumProperty("Transition", &UISelectableComponent::Transition, { { "None", 0 }, { "ColorTint", 1, "Color Tint" }, { "ImageSwap", 2, "Image Swap" } },
				{ .Tooltip = "Color Tint multiplies the target image's color by the state's; Image Swap shows the state's texture" })
			.EntityProperty("TargetImage", &UISelectableComponent::TargetImage, { .Tooltip = "The element whose image shows the states; none: this entity's" })
			.Property("NormalColor", &UISelectableComponent::NormalColor, Color())
			.Property("HoverColor", &UISelectableComponent::HoverColor, Color())
			.Property("PressedColor", &UISelectableComponent::PressedColor, Color())
			.Property("FocusedColor", &UISelectableComponent::FocusedColor, Color())
			.Property("DisabledColor", &UISelectableComponent::DisabledColor, Color())
			.AssetProperty("HoverImage", &UISelectableComponent::HoverImage, AssetType::Texture)
			.AssetProperty("PressedImage", &UISelectableComponent::PressedImage, AssetType::Texture)
			.AssetProperty("FocusedImage", &UISelectableComponent::FocusedImage, AssetType::Texture)
			.AssetProperty("DisabledImage", &UISelectableComponent::DisabledImage, AssetType::Texture)
			.Property("FadeDuration", &UISelectableComponent::FadeDuration, WithTooltip(Range(0.0f, 60.0f, 0.01f), "Seconds of unscaled time a color change takes"))
			.EnumProperty("Navigation", &UISelectableComponent::Navigation, { { "Automatic", 0 }, { "Explicit", 1 }, { "None", 2 } },
				{ .Tooltip = "Automatic: the nearest selectable in the direction; Explicit: the entities below; None: never reached or left by navigation" })
			.EntityProperty("NavigateUp", &UISelectableComponent::NavigateUp, { .Tooltip = "Explicit navigation" })
			.EntityProperty("NavigateDown", &UISelectableComponent::NavigateDown, { .Tooltip = "Explicit navigation" })
			.EntityProperty("NavigateLeft", &UISelectableComponent::NavigateLeft, { .Tooltip = "Explicit navigation" })
			.EntityProperty("NavigateRight", &UISelectableComponent::NavigateRight, { .Tooltip = "Explicit navigation" });

		ComponentRegistry::Register<UIButtonComponent>("UIButton")
			.DisplayName("UI Button")
			.Category(c_Category)
			.Description("Clicks on release over the button it was pressed on, or on Submit while focused: the scripts of its entity get OnUIClick")
			.Requires(c_RectName)
			.EntityProperty("EventTarget", &UIButtonComponent::EventTarget, { .Tooltip = "Its scripts get the button's events too" });

		ComponentRegistry::Register<UIToggleComponent>("UIToggle")
			.DisplayName("UI Toggle")
			.Category(c_Category)
			.Description("A switch that flips on click; toggles of one group are exclusive")
			.Requires(c_RectName)
			.Property("IsOn", &UIToggleComponent::IsOn)
			.EntityProperty("Checkmark", &UIToggleComponent::Checkmark, { .Tooltip = "An element drawn only while the toggle is on" })
			.EntityProperty("Group", &UIToggleComponent::Group, { .Tooltip = "Toggles naming the same group are exclusive" })
			.EntityProperty("EventTarget", &UIToggleComponent::EventTarget, { .Tooltip = "Its scripts get the toggle's events too" });

		ComponentRegistry::Register<UISliderComponent>("UISlider")
			.DisplayName("UI Slider")
			.Category(c_Category)
			.Description("A value between a minimum and a maximum, dragged with the pointer or stepped with the navigation keys")
			.Requires(c_RectName)
			.Property("MinValue", &UISliderComponent::MinValue, Range(-c_Unbounded, c_Unbounded, 0.01f))
			.Property("MaxValue", &UISliderComponent::MaxValue, Range(-c_Unbounded, c_Unbounded, 0.01f))
			.Property("Value", &UISliderComponent::Value, Range(-c_Unbounded, c_Unbounded, 0.01f))
			.Property("WholeNumbers", &UISliderComponent::WholeNumbers)
			.EnumProperty("Direction", &UISliderComponent::Direction, FillDirections(), { .Tooltip = "Where the minimum is" })
			.EntityProperty("Fill", &UISliderComponent::Fill, { .Tooltip = "An element anchored from the slider's start to the value" })
			.EntityProperty("Handle", &UISliderComponent::Handle, { .Tooltip = "An element anchored at the value" })
			.Property("KeyboardStep", &UISliderComponent::KeyboardStep, WithTooltip(Range(0.0f, c_Unbounded, 0.01f), "The navigation keys' step; 0: a tenth of the range"))
			.EntityProperty("EventTarget", &UISliderComponent::EventTarget, { .Tooltip = "Its scripts get the slider's events too" });

		ComponentRegistry::Register<UILayoutComponent>("UILayout")
			.DisplayName("UI Layout")
			.Category(c_Category)
			.Description("Places the element's children: stacked horizontally or vertically, or in a grid")
			.Requires(c_RectName)
			.EnumProperty("Type", &UILayoutComponent::Type, { { "Horizontal", 0 }, { "Vertical", 1 }, { "Grid", 2 } })
			.Property("Padding", &UILayoutComponent::Padding, WithTooltip(Range(0.0f, c_Unbounded, 1.0f), "Left, top, right and bottom, in canvas units"))
			.Property("Spacing", &UILayoutComponent::Spacing, WithTooltip(Range(-c_Unbounded, c_Unbounded, 1.0f), "Between children (x: horizontal, y: vertical), in canvas units"))
			.EnumProperty("ChildAlignment", &UILayoutComponent::ChildAlignment,
				{ { "TopLeft", 0, "Top Left" }, { "TopCenter", 1, "Top Center" }, { "TopRight", 2, "Top Right" }, { "MiddleLeft", 3, "Middle Left" },
					{ "MiddleCenter", 4, "Middle Center" }, { "MiddleRight", 5, "Middle Right" }, { "BottomLeft", 6, "Bottom Left" },
					{ "BottomCenter", 7, "Bottom Center" }, { "BottomRight", 8, "Bottom Right" } },
				{ .Tooltip = "Where the block of children sits in the padded rect" })
			.Property("StretchChildren", &UILayoutComponent::StretchChildren, WithTooltip({}, "Children fill the cross axis"))
			.Property("CellSize", &UILayoutComponent::CellSize, WithTooltip(Range(1.0f, c_Unbounded, 1.0f), "Grid: the size of a cell, in canvas units"))
			.Property("Columns", &UILayoutComponent::Columns, WithTooltip(Range(0.0f, 4096.0f), "Grid: 0 fits as many columns as the width holds"));

		ComponentRegistry::Register<UIMaskComponent>("UIMask")
			.DisplayName("UI Mask")
			.Category(c_Category)
			.Description("Clips the element's children to its rect: what lies outside is neither drawn nor hit")
			.Requires(c_RectName)
			.Property("Padding", &UIMaskComponent::Padding, WithTooltip(Range(-c_Unbounded, c_Unbounded, 1.0f),
				"Left, top, right and bottom insets of the clip rect, in canvas units (negative: outside the rect)"))
			.Property("Softness", &UIMaskComponent::Softness, WithTooltip(Range(0.0f, c_Unbounded, 0.5f),
				"Canvas units over which the children fade out toward the clip rect's edges (0: a hard edge)"));
	}

}
