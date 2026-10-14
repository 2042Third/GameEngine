#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameUIPresets.h"
#include "Editor/SceneEdit.h"
#include "Editor/UIRectEdit.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIHitTest.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneServices.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// ui.layout lists at most this many elements (a 10,000-element menu would be a 10 MB answer).
		constexpr size_t c_MaxListedElements = 1000;
		// Screens commands lay canvases out for, per side.
		constexpr int64_t c_MaxScreenPixels = 16384;
		// Canvas units of positions and sizes beyond this are mistakes.
		constexpr double c_MaxCanvasUnits = 1'000'000.0;

		nlohmann::json PairSchema(std::string description)
		{
			return { { "type", "array" }, { "items", { { "type", "number" } } }, { "minItems", 2 }, { "maxItems", 2 }, { "description", std::move(description) } };
		}

		nlohmann::json ScreenSchema()
		{
			return { { "type", "array" }, { "items", { { "type", "integer" }, { "minimum", 1 }, { "maximum", c_MaxScreenPixels } } }, { "minItems", 2 },
				{ "maxItems", 2 }, { "description", "[width, height] of the screen in pixels (default: the game's screen: while playing the game "
					"view's image, in edit mode the viewport's image, without either 1920x1080)" } };
		}

		std::optional<glm::vec2> ReadPair(const nlohmann::json& parameters, CommandArguments& arguments, const char* name, double limit)
		{
			const auto it = parameters.find(name);
			if (it != parameters.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number())
			{
				const double x = (*it)[0].get<double>();
				const double y = (*it)[1].get<double>();
				if (std::isfinite(x) && std::isfinite(y) && std::abs(x) <= limit && std::abs(y) <= limit)
					return glm::vec2(static_cast<float>(x), static_cast<float>(y));
			}
			arguments.SetError(fmt::format("Parameter '{}' must be an array of two finite numbers within +-{}", name, limit));
			return std::nullopt;
		}

		// The game's screen: while playing the UI's input screen (the game view's image), in edit mode the viewport's image,
		// without either (headless) the default screen.
		UIScreen GetGameScreen(EditorContext& context)
		{
			if (context.IsPlaying())
				return GameUIScene::GetInputScreen().Screen;
			const glm::uvec2 viewport = context.GetViewport().GetSize();
			if (viewport.x > 0 && viewport.y > 0)
				return UIScreen { glm::vec2(viewport), context.GetGameWindow().GetState().ContentScale };
			return GameUIScene::c_DefaultScreen;
		}

		// The screen a command lays canvases out for: its "screen" parameter, else the game's screen.
		std::optional<UIScreen> ReadScreen(EditorContext& context, const nlohmann::json& parameters, CommandArguments& arguments)
		{
			const float contentScale = context.GetGameWindow().GetState().ContentScale;
			const auto it = parameters.find("screen");
			if (it != parameters.end())
			{
				if (it->is_array() && it->size() == 2 && (*it)[0].is_number_integer() && (*it)[1].is_number_integer())
				{
					const int64_t width = (*it)[0].get<int64_t>();
					const int64_t height = (*it)[1].get<int64_t>();
					if (width >= 1 && height >= 1 && width <= c_MaxScreenPixels && height <= c_MaxScreenPixels)
						return UIScreen { glm::vec2(static_cast<float>(width), static_cast<float>(height)), contentScale };
				}
				arguments.SetError(fmt::format("Parameter 'screen' must be [width, height] in whole pixels from 1 to {}", c_MaxScreenPixels));
				return std::nullopt;
			}
			return GetGameScreen(context);
		}

		nlohmann::json ToJson(const UIBox& box, float scale)
		{
			return { box.Min.x * scale, box.Min.y * scale, box.Size.x * scale, box.Size.y * scale };
		}

		const char* GetStateName(UIVisualState state)
		{
			switch (state)
			{
				case UIVisualState::Normal: return "normal";
				case UIVisualState::Hover: return "hover";
				case UIVisualState::Pressed: return "pressed";
				case UIVisualState::Focused: return "focused";
				case UIVisualState::Disabled: return "disabled";
			}
			return "normal";
		}

		// "Canvas/Menu/Play": the entity's names from its canvas down.
		std::string GetPath(Entity entity)
		{
			std::string path = entity.GetName();
			for (Entity current = entity; current && !current.HasComponent<UICanvasComponent>();)
			{
				current = current.GetParent();
				if (!current)
					break;
				path = current.GetName() + "/" + path;
			}
			return path;
		}

		// The rect of an element in pixels at a screen (null when it is not a laid-out element).
		nlohmann::json DescribeRect(GameUIScene& ui, Entity entity, const UIScreen& screen)
		{
			ui.EnsureLayout(screen); // The changes since the latest layout (a new element) included
			UICanvasView view;
			uint32_t element = 0;
			if (!ui.FindElement(entity.GetHandle(), screen, view, element))
				return nullptr;
			return ToJson(ui.GetElementBox(view, element), view.Scale);
		}

		nlohmann::json DescribeElement(Scene& scene, GameUIScene& ui, const UICanvasView& view, uint32_t element, Entity focused)
		{
			const Entity entity(ui.GetElementEntity(view, element), &scene);
			nlohmann::json described = {
				{ "entity", UUIDToJson(entity.GetUUID()) },
				{ "name", entity.GetName() },
				{ "rect", ToJson(ui.GetElementBox(view, element), view.Scale) },
				{ "visible", ui.IsElementVisible(view, element) } };
			if (const UISelectableComponent* selectable = entity.TryGetComponent<UISelectableComponent>())
			{
				described["interactable"] = selectable->Interactable;
				described["state"] = GetStateName(ui.GetVisualState(entity.GetHandle(), *selectable));
				described["focused"] = focused && focused == entity;
			}
			if (const UITextComponent* text = entity.TryGetComponent<UITextComponent>())
				described["text"] = text->Text;
			if (const UISliderComponent* slider = entity.TryGetComponent<UISliderComponent>())
				described["value"] = slider->Value;
			else if (const UIToggleComponent* toggle = entity.TryGetComponent<UIToggleComponent>())
				described["value"] = toggle->IsOn ? 1.0f : 0.0f;
			return described;
		}

		std::string GetRunningCopyWarning(const EditorContext& context)
		{
			return fmt::format("The scene is running ({}): this change applies to the running copy and is discarded by play.stop",
				SceneStateToString(context.GetSceneState()));
		}

		// Records the undo step (edit mode) and reports the result, as the scene commands do: while playing the running copy
		// changed (the result warns), while a scene is being opened the edit is undone and fails.
		EditorCommandResult Finish(EditorContext& context, SceneEditTransaction& transaction, nlohmann::json value)
		{
			if (std::string error; !context.CheckSceneEditable(&error))
			{
				transaction.Rollback();
				return EditorCommandResult::Fail(std::move(error));
			}
			context.CommitEdit(transaction);
			if (!context.IsPlaying())
				return EditorCommandResult::Ok(std::move(value));
			value["warning"] = GetRunningCopyWarning(context);
			EditorCommandResult result = EditorCommandResult::Ok(std::move(value));
			result.RunningCopy = true;
			return result;
		}

		std::string JoinTypeNames()
		{
			std::string names;
			for (uint32_t index = 0; index < c_UIPresetTypeCount; index++)
				names += (index == 0 ? "" : ", ") + std::string(GameUIPresets::GetTypeName(static_cast<UIPresetType>(index)));
			return names;
		}

		std::string JoinAnchorNames()
		{
			std::string names;
			for (uint32_t index = 0; index < c_UIAnchorPresetCount; index++)
				names += (index == 0 ? "" : ", ") + std::string(UIRectEdit::GetPresetName(static_cast<UIAnchorPreset>(index)));
			return names;
		}

	}

	void RegisterUICommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "ui.create",
			"Creates a game UI element ready to use, in one undo step (while playing it changes the running copy only): a canvas, a panel (a "
			"rounded, framed background), an image, a text, a button (with its label), a toggle (with box, checkmark and label), a slider (with "
			"track, fill and handle) or a horizontal, vertical or grid layout group. An element whose parent is outside every canvas (or that has "
			"none) gets a new top-level canvas first. anchor places it on its parent (and sets the pivot to match); position and size are in "
			"canvas units (along a stretched axis size is added to the parent's span: 0 fills it). Returns entity, canvas, created (every new "
			"entity), parts (label, background, fill, handle, checkmark, as the type has them) and rect ([x, y, width, height] in pixels at the "
			"game's screen).",
			ObjectSchema({
				{ "type", StringSchema(fmt::format("What to create: {}", JoinTypeNames())) },
				{ "parent", OptionalEntitySchema("The parent (a canvas or an element of one; outside a canvas a new canvas is made)") },
				{ "name", StringSchema("The element's name (default: the type's, e.g. \"Button\")") },
				{ "text", StringSchema("The text of a text, button or toggle (default \"New Text\", \"Button\", \"Toggle\")") },
				{ "anchor", StringSchema(fmt::format("Where it sits on its parent: {} (default middleCenter)", JoinAnchorNames())) },
				{ "position", PairSchema("[x, y]: the pivot's offset from the anchors in canvas units, +y down (default [0, 0])") },
				{ "size", PairSchema("[width, height] in canvas units (default: the type's; along stretched axes 0)") } }, { "type" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::string typeName = arguments.GetString("type");
				const Entity parent = arguments.FindEntity(scene, "parent");
				const std::string name = arguments.GetString("name", "");
				std::optional<std::string> text;
				if (arguments.Has("text"))
					text = arguments.GetString("text");
				const std::string anchorName = arguments.GetString("anchor", "middleCenter");
				std::optional<glm::vec2> position;
				std::optional<glm::vec2> size;
				if (arguments.Has("position"))
					position = ReadPair(parameters, arguments, "position", c_MaxCanvasUnits);
				if (arguments.Has("size"))
					size = ReadPair(parameters, arguments, "size", c_MaxCanvasUnits);
				const std::optional<UIPresetType> type = GameUIPresets::FindType(typeName);
				if (arguments.IsValid() && !type)
					arguments.SetError(fmt::format("Unknown UI type '{}': use {}", typeName, JoinTypeNames()));
				const std::optional<UIAnchorPreset> anchor = UIRectEdit::FindPreset(anchorName);
				if (arguments.IsValid() && !anchor)
					arguments.SetError(fmt::format("Unknown anchor '{}': use {}", anchorName, JoinAnchorNames()));
				if (arguments.IsValid() && size)
				{
					// A negative size shrinks a stretched span (margins); at a point anchor it is a mistake.
					const UIRectEdit::Anchors anchors = UIRectEdit::GetAnchors(*anchor);
					for (int axis = 0; axis < 2; axis++)
					{
						if ((*size)[axis] < 0.0f && anchors.Min[axis] == anchors.Max[axis])
							arguments.SetError(fmt::format("Parameter 'size' must not be negative along an axis anchored at a point ({} {})",
								axis == 0 ? "width" : "height", (*size)[axis]));
					}
				}
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, fmt::format("Create {}", GameUIPresets::GetDisplayName(*type)), {});
				std::string error;
				const std::optional<UIPresetParts> parts = GameUIPresets::Create(scene, *type, parent, name, text, &error);
				if (parts)
				{
					for (const Entity created : parts->Created)
						transaction.TrackCreated(created.GetUUID());
				}
				if (!parts)
				{
					transaction.Rollback();
					return EditorCommandResult::Fail(error);
				}

				// Placed on the parent: the preset's anchors and pivot, then the position and size asked for.
				Entity element = parts->Element;
				UIRectComponent rect = element.GetComponent<UIRectComponent>();
				const UIRectEdit::Anchors anchors = UIRectEdit::GetAnchors(*anchor);
				rect.AnchorMin = anchors.Min;
				rect.AnchorMax = anchors.Max;
				rect.Pivot = anchors.Pivot;
				rect.Position = position.value_or(glm::vec2(0.0f));
				for (int axis = 0; axis < 2; axis++)
				{
					if (size)
						rect.Size[axis] = (*size)[axis];
					else if (UIRectEdit::IsStretched(rect, axis))
						rect.Size[axis] = 0.0f;
				}
				element.GetComponent<UIRectComponent>() = rect;
				element.MarkModified<UIRectComponent>();

				nlohmann::json created = nlohmann::json::array();
				for (const Entity entity : parts->Created)
					created.push_back(UUIDToJson(entity.GetUUID()));
				nlohmann::json described = nlohmann::json::object();
				const std::array<std::pair<const char*, Entity>, 5> namedParts = { { { "label", parts->Label }, { "background", parts->Background },
					{ "fill", parts->Fill }, { "handle", parts->Handle }, { "checkmark", parts->Checkmark } } };
				for (const auto& [key, part] : namedParts)
				{
					if (part)
						described[key] = UUIDToJson(part.GetUUID());
				}
				// The rect where the game will show it (the layout follows the new entities at once).
				const UIScreen screen = GetGameScreen(context);
				nlohmann::json value = {
					{ "entity", UUIDToJson(element.GetUUID()) },
					{ "canvas", UUIDToJson(parts->Canvas.GetUUID()) },
					{ "created", std::move(created) },
					{ "parts", std::move(described) },
					{ "rect", DescribeRect(GameUIScene::Get(scene), element, screen) } };
				return Finish(context, transaction, std::move(value));
			} });

		registry.Register({ "ui.layout",
			"Where the game UI's elements are at a screen size, without rendering: the root canvases (entity, name, scale in pixels per canvas "
			"unit, unitSize, sortOrder, receivesInput) and the elements of an entity's subtree (all canvases without one), in draw order: "
			"entity, name, rect ([x, y, width, height] in screen pixels), visible, and for selectables interactable, state (normal, hover, "
			"pressed, focused, disabled) and focused, for texts text, for sliders and toggles value. At most 1,000 elements (truncated tells). "
			"Check that a HUD keeps its proportions by comparing rects at several screens.",
			ObjectSchema({
				{ "entity", OptionalEntitySchema("A canvas or element whose subtree to list (default: every canvas)") },
				{ "screen", ScreenSchema() } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity root = arguments.FindEntity(scene, "entity");
				const std::optional<UIScreen> screen = ReadScreen(context, parameters, arguments);
				if (!arguments.IsValid())
					return arguments.Fail();

				nlohmann::json value = { { "screen", { screen->Size.x, screen->Size.y } }, { "canvases", nlohmann::json::array() },
					{ "elements", nlohmann::json::array() }, { "truncated", false } };
				if (!GameUIScene::HasCanvases(scene))
				{
					if (root)
						return EditorCommandResult::Fail(fmt::format("'{}' is not on a UI canvas: the scene has none", root.GetName()));
					return EditorCommandResult::Ok(std::move(value));
				}
				GameUIScene& ui = GameUIScene::Get(scene);
				const std::span<const UICanvasView> laidOut = ui.EnsureLayout(*screen);
				const std::vector<UICanvasView> views(laidOut.begin(), laidOut.end());
				IUIControl* control = scene.GetService<IUIControl>();
				const Entity focused = control ? control->GetFocus() : Entity();

				std::optional<uint32_t> rootView;
				uint32_t first = 0;
				uint32_t end = 0;
				if (root)
				{
					UICanvasView view;
					uint32_t element = 0;
					if (!ui.FindElement(root.GetHandle(), *screen, view, element))
						return EditorCommandResult::Fail(fmt::format("'{}' is not an element of a UI canvas (it needs a UIRect under a UICanvas)", root.GetName()));
					for (uint32_t index = 0; index < views.size() && !rootView; index++)
					{
						if (views[index].CanvasIndex == view.CanvasIndex)
							rootView = index;
					}
					first = element;
					end = ui.GetElementSubtreeEnd(view, element);
				}
				for (uint32_t index = 0; index < views.size(); index++)
				{
					const UICanvasView& view = views[index];
					const Entity canvas(view.Canvas, &scene);
					value["canvases"].push_back({ { "entity", UUIDToJson(canvas.GetUUID()) }, { "name", canvas.GetName() }, { "scale", view.Scale },
						{ "unitSize", { view.UnitSize.x, view.UnitSize.y } }, { "sortOrder", view.SortOrder }, { "receivesInput", view.ReceivesInput } });
					if (rootView && *rootView != index)
						continue;
					const uint32_t from = rootView ? first : 0;
					const uint32_t to = rootView ? end : ui.GetElementCount(view);
					for (uint32_t element = from; element < to; element++)
					{
						if (value["elements"].size() >= c_MaxListedElements)
						{
							value["truncated"] = true;
							break;
						}
						value["elements"].push_back(DescribeElement(scene, ui, view, element, focused));
					}
				}
				return EditorCommandResult::Ok(std::move(value));
			}, EditorCommandKind::Query });

		registry.Register({ "ui.hitTest",
			"What the player would hit at a position of the game's screen (edit and play mode): the topmost raycast target (an image or text "
			"that is one, or a selectable) of the canvases that receive input, as entity, name, path (from its canvas), selectable (the "
			"selectable it belongs to, which a click would press) and interactable; entity null when the position hits nothing. With all, every "
			"element whose rect contains the position, topmost first, raycast target or not, in every canvas.",
			ObjectSchema({
				{ "position", PairSchema("[x, y] in screen pixels from the top-left corner") },
				{ "screen", ScreenSchema() },
				{ "all", BoolSchema("List every element at the position instead (default false)") } }, { "position" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::optional<glm::vec2> position = ReadPair(parameters, arguments, "position", static_cast<double>(c_MaxScreenPixels) * 4.0);
				const std::optional<UIScreen> screen = ReadScreen(context, parameters, arguments);
				const bool all = arguments.GetBool("all", false);
				if (!arguments.IsValid())
					return arguments.Fail();

				nlohmann::json value = { { "position", { position->x, position->y } }, { "screen", { screen->Size.x, screen->Size.y } } };
				if (!GameUIScene::HasCanvases(scene))
				{
					value[all ? "hits" : "entity"] = all ? nlohmann::json::array() : nlohmann::json(nullptr);
					return EditorCommandResult::Ok(std::move(value));
				}
				GameUIScene& ui = GameUIScene::Get(scene);
				const std::span<const UICanvasView> laidOut = ui.EnsureLayout(*screen);
				const std::vector<UICanvasView> views(laidOut.begin(), laidOut.end());
				GameUIHitTester tester;
				tester.Update(ui, views);
				auto describe = [&](entt::entity handle)
				{
					const Entity entity(handle, &scene);
					nlohmann::json hit = { { "entity", UUIDToJson(entity.GetUUID()) }, { "name", entity.GetName() }, { "path", GetPath(entity) } };
					for (Entity current = entity; current; current = current.GetParent())
					{
						if (const UISelectableComponent* selectable = current.TryGetComponent<UISelectableComponent>())
						{
							hit["selectable"] = UUIDToJson(current.GetUUID());
							hit["interactable"] = selectable->Interactable;
							break;
						}
						if (current.HasComponent<UICanvasComponent>())
							break;
					}
					if (!hit.contains("selectable"))
						hit["interactable"] = false;
					return hit;
				};
				if (all)
				{
					std::vector<UIHitResult> hits;
					tester.HitTestAll(ui, views, *position, hits);
					value["hits"] = nlohmann::json::array();
					for (const UIHitResult& hit : hits)
						value["hits"].push_back(describe(hit.Entity));
					return EditorCommandResult::Ok(std::move(value));
				}
				const std::optional<UIHitResult> hit = tester.HitTest(*position);
				if (!hit)
				{
					value["entity"] = nullptr;
					return EditorCommandResult::Ok(std::move(value));
				}
				value.update(describe(hit->Entity));
				return EditorCommandResult::Ok(std::move(value));
			}, EditorCommandKind::Query });
	}

}
