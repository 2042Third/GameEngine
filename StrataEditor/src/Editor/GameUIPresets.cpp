#include "Editor/GameUIPresets.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneHierarchy.h>

namespace Strata::GameUIPresets
{

	namespace
	{

		struct PresetName
		{
			const char* Type;
			const char* Display;
		};

		constexpr std::array<PresetName, c_UIPresetTypeCount> c_Names = { {
			{ "canvas", "Canvas" },
			{ "panel", "Panel" },
			{ "image", "Image" },
			{ "text", "Text" },
			{ "button", "Button" },
			{ "toggle", "Toggle" },
			{ "slider", "Slider" },
			{ "horizontalLayout", "Horizontal Layout" },
			{ "verticalLayout", "Vertical Layout" },
			{ "gridLayout", "Grid Layout" },
		} };

		// The palette of the presets: a dark, slightly blue theme that reads on any scene.
		constexpr glm::vec4 c_PanelColor = { 0.10f, 0.12f, 0.18f, 0.92f };
		constexpr glm::vec4 c_PanelBorder = { 0.35f, 0.45f, 0.75f, 1.0f };
		constexpr glm::vec4 c_AccentColor = { 0.24f, 0.48f, 0.92f, 1.0f };
		constexpr glm::vec4 c_TrackColor = { 0.15f, 0.17f, 0.24f, 1.0f };
		constexpr glm::vec4 c_CheckColor = { 0.40f, 0.68f, 1.0f, 1.0f };
		constexpr glm::vec4 c_TextColor = { 0.95f, 0.96f, 1.0f, 1.0f };

		UIRectComponent PointRect(const glm::vec2& size)
		{
			UIRectComponent rect;
			rect.Size = size;
			return rect;
		}

		UIRectComponent FillRect(const glm::vec2& inset = glm::vec2(0.0f))
		{
			UIRectComponent rect;
			rect.AnchorMin = glm::vec2(0.0f);
			rect.AnchorMax = glm::vec2(1.0f);
			rect.Size = -2.0f * inset;
			return rect;
		}

		// Creates a child (or a top-level entity) with a rect; invalid when the scene is full.
		Entity CreateElement(Scene& scene, Entity parent, std::string_view name, const UIRectComponent& rect, UIPresetParts& parts)
		{
			Entity entity = parent ? scene.CreateChildEntity(parent, std::string(name)) : scene.CreateEntity(std::string(name));
			if (!entity)
				return {};
			entity.AddComponent<UIRectComponent>(rect);
			parts.Created.push_back(entity);
			return entity;
		}

		UIImageComponent& AddImage(Entity entity, const glm::vec4& color, float cornerRadius)
		{
			UIImageComponent& image = entity.AddComponent<UIImageComponent>();
			image.Color = color;
			image.CornerRadius = cornerRadius;
			return image;
		}

		UITextComponent& AddText(Entity entity, std::string text, float fontSize)
		{
			UITextComponent& component = entity.AddComponent<UITextComponent>();
			component.Text = std::move(text);
			component.FontSize = fontSize;
			component.Color = c_TextColor;
			return component;
		}

		// Builds the element's own components and parts under `element`. False when the scene ran out of entities.
		bool Build(Scene& scene, UIPresetType type, Entity element, const std::optional<std::string>& text, UIPresetParts& parts)
		{
			switch (type)
			{
				case UIPresetType::Canvas:
				{
					UICanvasComponent& canvas = element.AddComponent<UICanvasComponent>();
					canvas.ScaleMode = UICanvasScaleMode::ScaleWithScreen;
					canvas.ReferenceResolution = { 1920.0f, 1080.0f };
					return true;
				}
				case UIPresetType::Panel:
				{
					UIImageComponent& image = AddImage(element, c_PanelColor, 12.0f);
					image.BorderWidth = 1.0f;
					image.BorderColor = c_PanelBorder;
					return true;
				}
				case UIPresetType::Image:
					AddImage(element, glm::vec4(1.0f), 0.0f);
					return true;
				case UIPresetType::Text:
					AddText(element, text.value_or("New Text"), 32.0f);
					return true;
				case UIPresetType::Button:
				{
					AddImage(element, c_AccentColor, 8.0f);
					element.AddComponent<UISelectableComponent>();
					element.AddComponent<UIButtonComponent>();
					parts.Label = CreateElement(scene, element, "Label", FillRect(), parts);
					if (!parts.Label)
						return false;
					AddText(parts.Label, text.value_or("Button"), 26.0f);
					return true;
				}
				case UIPresetType::Toggle:
				{
					UIRectComponent boxRect = PointRect({ 32.0f, 32.0f });
					boxRect.AnchorMin = boxRect.AnchorMax = { 0.0f, 0.5f };
					boxRect.Pivot = { 0.0f, 0.5f };
					parts.Background = CreateElement(scene, element, "Box", boxRect, parts);
					if (!parts.Background)
						return false;
					UIImageComponent& box = AddImage(parts.Background, c_TrackColor, 6.0f);
					box.BorderWidth = 1.0f;
					box.BorderColor = c_PanelBorder;
					parts.Checkmark = CreateElement(scene, parts.Background, "Checkmark", FillRect(glm::vec2(6.0f)), parts);
					if (!parts.Checkmark)
						return false;
					AddImage(parts.Checkmark, c_CheckColor, 3.0f);
					// The label fills the rest: 44 units from the left edge.
					UIRectComponent labelRect = FillRect();
					labelRect.Size = { -44.0f, 0.0f };
					labelRect.Position = { 22.0f, 0.0f };
					parts.Label = CreateElement(scene, element, "Label", labelRect, parts);
					if (!parts.Label)
						return false;
					AddText(parts.Label, text.value_or("Toggle"), 24.0f).Alignment = TextAlignment::Left;

					UISelectableComponent& selectable = element.AddComponent<UISelectableComponent>();
					selectable.TargetImage = parts.Background.GetUUID();
					UIToggleComponent& toggle = element.AddComponent<UIToggleComponent>();
					toggle.IsOn = true;
					toggle.Checkmark = parts.Checkmark.GetUUID();
					return true;
				}
				case UIPresetType::Slider:
				{
					parts.Background = CreateElement(scene, element, "Background", FillRect(), parts);
					if (!parts.Background)
						return false;
					AddImage(parts.Background, c_TrackColor, 6.0f);
					// The slider anchors the fill from its start to the value and the handle at the value along its direction;
					// across it they keep their own anchors.
					parts.Fill = CreateElement(scene, element, "Fill", FillRect(), parts);
					if (!parts.Fill)
						return false;
					AddImage(parts.Fill, c_AccentColor, 6.0f);
					parts.Handle = CreateElement(scene, element, "Handle", PointRect({ 18.0f, 32.0f }), parts);
					if (!parts.Handle)
						return false;
					AddImage(parts.Handle, c_TextColor, 6.0f);

					UISelectableComponent& selectable = element.AddComponent<UISelectableComponent>();
					selectable.TargetImage = parts.Handle.GetUUID();
					UISliderComponent& slider = element.AddComponent<UISliderComponent>();
					slider.Value = 0.5f;
					slider.Fill = parts.Fill.GetUUID();
					slider.Handle = parts.Handle.GetUUID();
					return true;
				}
				case UIPresetType::HorizontalLayout:
				case UIPresetType::VerticalLayout:
				case UIPresetType::GridLayout:
				{
					UILayoutComponent& layout = element.AddComponent<UILayoutComponent>();
					layout.Padding = glm::vec4(8.0f);
					layout.Spacing = glm::vec2(8.0f);
					if (type == UIPresetType::HorizontalLayout)
					{
						layout.Type = UILayoutType::Horizontal;
						layout.ChildAlignment = UIChildAlignment::MiddleLeft;
					}
					else if (type == UIPresetType::VerticalLayout)
					{
						layout.Type = UILayoutType::Vertical;
						layout.ChildAlignment = UIChildAlignment::TopCenter;
						layout.StretchChildren = true;
					}
					else
					{
						layout.Type = UILayoutType::Grid;
						layout.CellSize = { 100.0f, 100.0f };
					}
					return true;
				}
			}
			return true;
		}

		glm::vec2 GetDefaultSize(UIPresetType type)
		{
			switch (type)
			{
				case UIPresetType::Canvas: return { 100.0f, 100.0f }; // A root canvas's rect is its screen
				case UIPresetType::Panel: return { 400.0f, 300.0f };
				case UIPresetType::Image: return { 100.0f, 100.0f };
				case UIPresetType::Text: return { 300.0f, 60.0f };
				case UIPresetType::Button: return { 220.0f, 56.0f };
				case UIPresetType::Toggle: return { 220.0f, 40.0f };
				case UIPresetType::Slider: return { 260.0f, 24.0f };
				case UIPresetType::HorizontalLayout:
				case UIPresetType::VerticalLayout:
				case UIPresetType::GridLayout: return { 400.0f, 300.0f };
			}
			return { 100.0f, 100.0f };
		}

	}

	const char* GetTypeName(UIPresetType type)
	{
		const uint32_t index = static_cast<uint32_t>(type);
		return index < c_UIPresetTypeCount ? c_Names[index].Type : "image";
	}

	const char* GetDisplayName(UIPresetType type)
	{
		const uint32_t index = static_cast<uint32_t>(type);
		return index < c_UIPresetTypeCount ? c_Names[index].Display : "Image";
	}

	std::optional<UIPresetType> FindType(std::string_view name)
	{
		for (uint32_t index = 0; index < c_UIPresetTypeCount; index++)
		{
			if (name == c_Names[index].Type)
				return static_cast<UIPresetType>(index);
		}
		return std::nullopt;
	}

	Entity FindCanvas(Entity entity)
	{
		for (Entity current = entity; current; current = current.GetParent())
		{
			if (current.HasComponent<UICanvasComponent>())
				return current;
		}
		return {};
	}

	std::optional<UIPresetParts> Create(Scene& scene, UIPresetType type, Entity parent, std::string_view name, const std::optional<std::string>& text,
		std::string* outError)
	{
		auto full = [&]() -> std::optional<UIPresetParts>
		{
			if (outError)
				*outError = fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities);
			return std::nullopt;
		};

		UIPresetParts parts;
		Entity under = parent;
		if (type != UIPresetType::Canvas)
		{
			parts.Canvas = FindCanvas(parent);
			if (!parts.Canvas)
			{
				// Elements outside a canvas are not drawn: they get one of their own.
				parts.Canvas = CreateElement(scene, Entity(), "Canvas", PointRect({ 100.0f, 100.0f }), parts);
				if (!parts.Canvas || !Build(scene, UIPresetType::Canvas, parts.Canvas, std::nullopt, parts))
					return full();
				under = parts.Canvas;
			}
		}

		parts.Element = CreateElement(scene, under, name.empty() ? std::string_view(GetDisplayName(type)) : name, PointRect(GetDefaultSize(type)), parts);
		if (!parts.Element || !Build(scene, type, parts.Element, text, parts))
			return full();
		if (type == UIPresetType::Canvas)
			parts.Canvas = parts.Element;
		return parts;
	}

}
