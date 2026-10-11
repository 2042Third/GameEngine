#include "UI/EditorActions.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HierarchyView.h"
#include "Editor/PropertyEdit.h"

#include <Strata/Core/Assert.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <utility>

namespace Strata
{

	nlohmann::json RunEditorCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters)
	{
		EditorCommandResult result = commands.Execute(context, name, parameters);
		if (result.IsPending())
		{
			// UI actions run commands that finish at once; commands that take frames go through EditorCommandRunner.
			ST_ASSERT(false, "{} finishes over several frames: run it through the EditorCommandRunner", name);
			ST_ERROR("{} finishes over several frames and cannot be run from here", name);
			return nullptr;
		}
		if (!result.Success)
		{
			ST_ERROR("{}: {}", name, result.Error);
			return nullptr;
		}
		if (result.Value.is_object() && result.Value.contains("warning"))
			ST_WARN("{}: {}", name, result.Value["warning"].get<std::string>());
		return result.Value;
	}

	void RunEditorCommandDeferred(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters)
	{
		runner.Run(context, commands, name, parameters, [name = std::string(name)](const EditorCommandResult& result)
		{
			if (!result.Success && result.ErrorKind != EditorCommandError::Cancelled)
				ST_ERROR("{}: {}", name, result.Error);
		});
	}

	namespace EditorActions
	{

		namespace
		{

			// The selected entities without those below another selected one, in hierarchy order: what moves, copies and
			// duplicates as a whole.
			nlohmann::json GetTopmostSelection(const EditorContext& context)
			{
				nlohmann::json ids = nlohmann::json::array();
				for (const UUID id : HierarchyView::GetTopmostInHierarchyOrder(*context.GetActiveScene(), context.GetSelection()))
					ids.push_back(UUIDToJson(id));
				return ids;
			}

			// The clipboard's text as a JSON document with a "Strata" header of the given format.
			std::optional<nlohmann::json> ReadClipboard(std::string_view format)
			{
				const char* text = ImGui::GetClipboardText();
				if (!text || !*text)
					return std::nullopt;
				std::optional<nlohmann::json> document = JsonUtils::Parse(text);
				if (!document || !document->is_object())
					return std::nullopt;
				const nlohmann::json* header = JsonUtils::Find(*document, "Strata");
				const nlohmann::json* found = header ? JsonUtils::Find(*header, "Format") : nullptr;
				if (!found || !found->is_string() || found->get<std::string>() != format)
					return std::nullopt;
				return document;
			}

			// The values of properties component.set can set (component.get's also holds read-only ones, such as a prefab
			// link's, which pasting would only make fail).
			nlohmann::json GetSettableValues(const ComponentInfo& component, const nlohmann::json& values)
			{
				nlohmann::json settable = nlohmann::json::object();
				if (!values.is_object())
					return settable;
				for (const auto& [name, value] : values.items())
				{
					const PropertyInfo* property = component.FindProperty(name);
					if (property && IsSettableProperty(*property))
						settable[name] = value;
				}
				return settable;
			}

			// Selects the entities a command returned under "entities".
			void SelectReturned(EditorContext& context, const nlohmann::json& result)
			{
				if (!result.is_object())
					return;
				const nlohmann::json* entities = JsonUtils::Find(result, "entities");
				if (!entities || !entities->is_array())
					return;
				std::vector<UUID> selection;
				for (const nlohmann::json& id : *entities)
				{
					if (const std::optional<UUID> uuid = UUIDFromJson(id))
						selection.push_back(*uuid);
				}
				context.SetSelection(std::move(selection));
			}

		}

		void DeleteSelection(EditorContext& context, const EditorCommandRegistry& commands)
		{
			if (context.GetSelection().empty())
				return;
			nlohmann::json ids = nlohmann::json::array();
			for (const UUID id : context.GetSelection())
				ids.push_back(UUIDToJson(id));
			RunEditorCommand(context, commands, "entity.delete", { { "entities", std::move(ids) } });
		}

		void DuplicateSelection(EditorContext& context, const EditorCommandRegistry& commands)
		{
			nlohmann::json entities = GetTopmostSelection(context);
			if (entities.empty())
				return;
			SelectReturned(context, RunEditorCommand(context, commands, "entity.duplicate", { { "entities", std::move(entities) } }));
		}

		bool CopySelection(EditorContext& context, const EditorCommandRegistry& commands)
		{
			nlohmann::json entities = GetTopmostSelection(context);
			if (entities.empty())
				return false;
			const nlohmann::json copied = RunEditorCommand(context, commands, "entity.copy", { { "entities", std::move(entities) } });
			const nlohmann::json* data = copied.is_object() ? JsonUtils::Find(copied, "data") : nullptr;
			if (!data)
				return false;
			ImGui::SetClipboardText(JsonUtils::Dump(*data).c_str());
			return true;
		}

		void CutSelection(EditorContext& context, const EditorCommandRegistry& commands)
		{
			if (CopySelection(context, commands))
				DeleteSelection(context, commands);
		}

		bool CanPaste()
		{
			return ReadClipboard(c_EntityDataFormat).has_value();
		}

		void Paste(EditorContext& context, const EditorCommandRegistry& commands)
		{
			std::optional<nlohmann::json> data = ReadClipboard(c_EntityDataFormat);
			if (!data)
				return;
			nlohmann::json parameters = { { "data", std::move(*data) } };
			// Right after the primary selection, as its siblings.
			if (const Entity primary = context.GetPrimarySelection())
			{
				const Entity parent = primary.GetParent();
				parameters["parent"] = parent ? UUIDToJson(parent.GetUUID()) : nlohmann::json(nullptr);
				parameters["index"] = context.GetActiveScene()->GetSiblingIndex(primary) + 1;
			}
			SelectReturned(context, RunEditorCommand(context, commands, "entity.paste", parameters));
		}

		void SelectAll(EditorContext& context)
		{
			std::vector<UUID> all;
			for (const Entity entity : context.GetActiveScene()->GetEntitiesInHierarchyOrder())
				all.push_back(entity.GetUUID());
			context.SetSelection(std::move(all));
		}

		void CopyComponentValues(const ComponentInfo& component, const nlohmann::json& values)
		{
			const nlohmann::json document = {
				{ "Strata", { { "Format", c_ComponentValuesFormat }, { "Version", 1 } } },
				{ "Component", component.Name },
				{ "Values", GetSettableValues(component, values) } };
			ImGui::SetClipboardText(JsonUtils::Dump(document).c_str());
		}

		std::optional<nlohmann::json> ReadComponentValues(const ComponentInfo& component)
		{
			const std::optional<nlohmann::json> document = ReadClipboard(c_ComponentValuesFormat);
			if (!document)
				return std::nullopt;
			const nlohmann::json* name = JsonUtils::Find(*document, "Component");
			const nlohmann::json* values = JsonUtils::Find(*document, "Values");
			if (!name || !name->is_string() || !values || !values->is_object() || ComponentRegistry::Find(name->get<std::string>()) != &component)
				return std::nullopt;
			nlohmann::json settable = GetSettableValues(component, *values);
			if (settable.empty())
				return std::nullopt;
			return settable;
		}

		void HandleSelectionShortcuts(EditorContext& context, const EditorCommandRegistry& commands, bool selectAll)
		{
			if (ImGui::GetTopMostPopupModal() || !context.AcceptsEditShortcuts())
				return;
			// RouteFocused (ImGui::Shortcut's default): only while this window or one inside it has the keyboard focus.
			constexpr ImGuiInputFlags c_Focused = ImGuiInputFlags_RouteFocused;
			if (ImGui::Shortcut(ImGuiKey_Delete, c_Focused))
				DeleteSelection(context, commands);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, c_Focused))
				DuplicateSelection(context, commands);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, c_Focused))
				CopySelection(context, commands);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, c_Focused))
				CutSelection(context, commands);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, c_Focused))
				Paste(context, commands);
			if (selectAll && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, c_Focused))
				SelectAll(context);
		}

	}

}
