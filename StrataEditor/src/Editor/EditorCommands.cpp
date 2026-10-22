#include "Editor/EditorCommands.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Profiling.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>

#include <limits>

namespace Strata
{

	const char* EditorCommandErrorToString(EditorCommandError error)
	{
		switch (error)
		{
			case EditorCommandError::None:              return "None";
			case EditorCommandError::UnknownCommand:    return "UnknownCommand";
			case EditorCommandError::InvalidParameters: return "InvalidParameters";
			case EditorCommandError::Failed:            return "Failed";
			case EditorCommandError::Cancelled:         return "Cancelled";
			case EditorCommandError::Internal:          return "Internal";
		}
		return "Unknown";
	}

	const char* EditorCommandKindToString(EditorCommandKind kind)
	{
		switch (kind)
		{
			case EditorCommandKind::Edit:    return "Edit";
			case EditorCommandKind::Query:   return "Query";
			case EditorCommandKind::Session: return "Session";
		}
		return "Unknown";
	}

	EditorCommandRegistry::EditorCommandRegistry()
	{
		RegisterSceneCommands(*this);
		RegisterEntityCommands(*this);
		RegisterPrefabCommands(*this);
		RegisterAssetCommands(*this);
		RegisterProjectSettingsCommands(*this);
		RegisterPreferencesCommands(*this);
		RegisterEditorStateCommands(*this);
		RegisterPlayCommands(*this);
		RegisterViewportCommands(*this);
		RegisterScriptCommands(*this);
		RegisterInputCommands(*this);
		RegisterStreamingCommands(*this);
		RegisterAnimationCommands(*this);
		RegisterWorldCommands(*this);
		RegisterAudioCommands(*this);
		RegisterUICommands(*this);
		RegisterUIInputCommands(*this);
		RegisterBatchCommands(*this);

		Register({ "editor.commands",
			"Every editor command with its description, the JSON Schema of its parameters and its kind: \"Edit\" (changes the scene, its assets or "
			"the project), \"Query\" (changes nothing) or \"Session\" (changes the editor's session: open project, play mode, view, selection).",
			CommandUtils::ObjectSchema({}),
			[this](EditorContext&, const nlohmann::json&)
			{
				nlohmann::json commands = nlohmann::json::array();
				for (const EditorCommand* command : GetAll())
				{
					commands.push_back({ { "name", command->Name }, { "description", command->Description }, { "parameters", command->Parameters },
						{ "kind", EditorCommandKindToString(command->Kind) } });
				}
				return EditorCommandResult::Ok({ { "commands", std::move(commands) } });
			}, EditorCommandKind::Query });
	}

	void EditorCommandRegistry::Register(EditorCommand command)
	{
		ST_ASSERT(!command.Name.empty() && command.Handler, "Editor commands need a name and a handler");
		std::string name = command.Name;
		m_Commands.insert_or_assign(std::move(name), std::move(command));
		m_Revision++;
	}

	const EditorCommand* EditorCommandRegistry::Find(std::string_view name) const
	{
		auto it = m_Commands.find(name);
		return it != m_Commands.end() ? &it->second : nullptr;
	}

	std::vector<const EditorCommand*> EditorCommandRegistry::GetAll() const
	{
		std::vector<const EditorCommand*> commands;
		commands.reserve(m_Commands.size());
		for (const auto& [name, command] : m_Commands)
			commands.push_back(&command);
		return commands;
	}

	EditorCommandResult EditorCommandRegistry::Execute(EditorContext& context, std::string_view name, const nlohmann::json& parameters,
		const CommandOrigin& origin) const
	{
		// Command work shows in the frame profiler (editor.perf) instead of as untimed time; the stages of what a command runs
		// (a scene load's, the renderer's of a capture) nest in it and count for themselves.
		ST_PROFILE_STAGE("Editor.Commands");
		const EditorCommand* command = Find(name);
		CommandJournal& journal = context.GetJournal();
		const UndoStack& undo = context.GetUndoStack();
		journal.NoteEditsOutsideCommands(undo);
		const uint64_t entry = journal.Begin(origin, name, command ? command->Kind : EditorCommandKind::Query,
			command ? command->Results : EditorCommandResults::Content, parameters, context.IsPlaying());
		const uint64_t records = undo.GetRecordCount();
		EditorCommandResult result;
		if (command && context.IsWaitingForAssets() && WaitsForAssetScan(*command))
		{
			// The registry is still filling: the command runs once the scan ended (and goes on from there if it defers itself).
			// This deferral is the registry's, not the handler's, so it needs no MayDefer; what the handler answers then does.
			result = EditorCommandResult::Defer([this, command, name = std::string(name), parameters = nlohmann::json(parameters)](EditorContext& context)
				-> std::optional<EditorCommandResult>
			{
				if (context.IsWaitingForAssets())
					return std::nullopt;
				EditorCommandResult deferred = Run(context, command, name, parameters);
				ST_ASSERT(!deferred.IsPending() || command->MayDefer, "Command '{}' answered later (Defer) but is not registered with MayDefer", name);
				return deferred;
			});
		}
		else
		{
			result = Run(context, command, name, parameters);
			// The flag keeps editor.batch from starting what cannot finish in its frame: it must be right.
			ST_ASSERT(!result.IsPending() || !command || command->MayDefer, "Command '{}' answered later (Defer) but is not registered with MayDefer", name);
		}
		// A step the handler recorded (or extended) is the command's.
		journal.NoteUndo(entry, undo, records);
		result.JournalId = entry;
		if (result.IsPending())
		{
			journal.Suspend(entry);
		}
		else
		{
			journal.Complete(entry, result);
		}
		return result;
	}

	bool EditorCommandRegistry::WaitsForAssetScan(const EditorCommand& command)
	{
		return command.Kind == EditorCommandKind::Edit || command.WaitsForAssets;
	}

	std::optional<std::string> EditorCommandRegistry::DescribeAssetScanWait(const EditorContext& context, std::string_view name) const
	{
		const EditorCommand* command = Find(name);
		if (!command || !context.IsWaitingForAssets() || !WaitsForAssetScan(*command))
			return std::nullopt;
		const std::optional<AssetScanProgress> progress = context.GetAssetScanProgress();
		if (!progress)
			return std::string("the project's start scene is still being opened");
		if (progress->Total == 0)
			return std::string("the project's assets are still being scanned");
		return fmt::format("the project's assets are still being scanned ({} of {})", StringUtils::FormatThousands(progress->Done),
			StringUtils::FormatThousands(progress->Total));
	}

	EditorCommandResult EditorCommandRegistry::Run(EditorContext& context, const EditorCommand* command, std::string_view name, const nlohmann::json& parameters) const
	{
		if (!command)
			return EditorCommandResult::Fail(fmt::format("Unknown command '{}' (editor.commands lists them)", name), EditorCommandError::UnknownCommand);
		if (!parameters.is_object() && !parameters.is_null())
			return EditorCommandResult::InvalidParameters("Command parameters must be a JSON object");

		const nlohmann::json& arguments = parameters.is_null() ? nlohmann::json::object() : parameters;
		try
		{
			// The schema is the contract: a misspelled or missing parameter must not silently change the meaning.
			if (std::optional<std::string> problem = CheckParameters(*command, arguments))
				return EditorCommandResult::InvalidParameters(std::move(*problem));
			return command->Handler(context, arguments);
		}
		catch (const std::exception& exception)
		{
			// Engine code does not throw; this guards third-party code (JSON access) used by handlers.
			return EditorCommandResult::Fail(fmt::format("Command '{}' failed: {}", name, exception.what()), EditorCommandError::Internal);
		}
	}

	std::optional<std::string> EditorCommandRegistry::CheckParameters(const EditorCommand& command, const nlohmann::json& parameters)
	{
		if (!parameters.is_object())
			return std::string("Command parameters must be a JSON object");
		if (const auto declared = command.Parameters.find("properties"); declared != command.Parameters.end() && declared->is_object())
		{
			for (const auto& [key, value] : parameters.items())
			{
				if (declared->contains(key))
					continue;
				std::string expected;
				for (const auto& [parameter, schema] : declared->items())
					expected += (expected.empty() ? "" : ", ") + parameter;
				return fmt::format("Unknown parameter '{}' for {} (parameters: {})", key, command.Name, expected.empty() ? "none" : expected);
			}
		}
		if (const auto required = command.Parameters.find("required"); required != command.Parameters.end() && required->is_array())
		{
			for (const nlohmann::json& parameter : *required)
			{
				if (parameter.is_string() && !parameters.contains(parameter.get<std::string>()))
					return fmt::format("Missing parameter '{}' for {}", parameter.get<std::string>(), command.Name);
			}
		}
		return std::nullopt;
	}

	////////////////////////////////////////////////////////////////////////////////
	// CommandArguments
	////////////////////////////////////////////////////////////////////////////////

	CommandArguments::CommandArguments(const nlohmann::json& parameters)
		: m_Parameters(parameters)
	{
	}

	bool CommandArguments::Has(std::string_view name) const
	{
		auto it = m_Parameters.find(name);
		return it != m_Parameters.end() && !it->is_null();
	}

	void CommandArguments::SetError(std::string error)
	{
		if (m_Error.empty())
			m_Error = std::move(error);
	}

	const nlohmann::json* CommandArguments::Get(std::string_view name)
	{
		auto it = m_Parameters.find(name);
		return it != m_Parameters.end() && !it->is_null() ? &*it : nullptr;
	}

	void CommandArguments::TypeError(std::string_view name, std::string_view expected)
	{
		SetError(fmt::format("Parameter '{}' must be {}", name, expected));
	}

	std::string CommandArguments::GetString(std::string_view name)
	{
		const nlohmann::json* value = Get(name);
		if (!value)
		{
			SetError(fmt::format("Missing parameter '{}'", name));
			return {};
		}
		if (!value->is_string())
		{
			TypeError(name, "a string");
			return {};
		}
		return value->get<std::string>();
	}

	std::string CommandArguments::GetString(std::string_view name, std::string fallback)
	{
		return Has(name) ? GetString(name) : fallback;
	}

	bool CommandArguments::GetBool(std::string_view name, bool fallback)
	{
		const nlohmann::json* value = Get(name);
		if (!value)
			return fallback;
		if (!value->is_boolean())
		{
			TypeError(name, "true or false");
			return fallback;
		}
		return value->get<bool>();
	}

	int64_t CommandArguments::GetInt(std::string_view name, int64_t fallback, int64_t min, int64_t max)
	{
		const nlohmann::json* value = Get(name);
		if (!value)
			return fallback;
		if (!value->is_number_integer())
		{
			TypeError(name, "an integer");
			return fallback;
		}
		// Unsigned values above the int64 range compare as negative after conversion; check them first.
		if (value->is_number_unsigned() && value->get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		{
			SetError(fmt::format("Parameter '{}' must be between {} and {}", name, min, max));
			return fallback;
		}
		const int64_t result = value->get<int64_t>();
		if (result < min || result > max)
		{
			SetError(fmt::format("Parameter '{}' must be between {} and {}", name, min, max));
			return fallback;
		}
		return result;
	}

	const nlohmann::json& CommandArguments::GetObject(std::string_view name)
	{
		static const nlohmann::json s_Empty = nlohmann::json::object();
		const nlohmann::json* value = Get(name);
		if (!value)
		{
			SetError(fmt::format("Missing parameter '{}'", name));
			return s_Empty;
		}
		if (!value->is_object())
		{
			TypeError(name, "an object");
			return s_Empty;
		}
		return *value;
	}

	const nlohmann::json* CommandArguments::FindObject(std::string_view name)
	{
		const nlohmann::json* value = Get(name);
		if (value && !value->is_object())
		{
			TypeError(name, "an object");
			return nullptr;
		}
		return value;
	}

	UUID CommandArguments::GetUUID(std::string_view name)
	{
		const nlohmann::json* value = Get(name);
		if (!value)
		{
			SetError(fmt::format("Missing parameter '{}'", name));
			return UUID::Null();
		}
		std::optional<UUID> uuid = UUIDFromJson(*value);
		if (!uuid || !uuid->IsValid())
		{
			TypeError(name, "an ID (16 hexadecimal digits)");
			return UUID::Null();
		}
		return *uuid;
	}

	Entity CommandArguments::GetEntity(Scene& scene, std::string_view name)
	{
		const UUID id = GetUUID(name);
		if (!id.IsValid())
			return {};
		Entity entity = scene.GetEntityByUUID(id);
		if (!entity)
			SetError(fmt::format("Parameter '{}': no entity {} in the scene", name, id.ToString()));
		return entity;
	}

	Entity CommandArguments::FindEntity(Scene& scene, std::string_view name)
	{
		return Has(name) ? GetEntity(scene, name) : Entity();
	}

	std::vector<Entity> CommandArguments::GetEntities(Scene& scene, std::string_view name)
	{
		std::vector<Entity> entities;
		const nlohmann::json* value = Get(name);
		if (!value || !value->is_array() || value->empty())
		{
			TypeError(name, "a non-empty array of entity IDs");
			return entities;
		}
		for (const nlohmann::json& element : *value)
		{
			std::optional<UUID> id = UUIDFromJson(element);
			Entity entity = id ? scene.GetEntityByUUID(*id) : Entity();
			if (!entity)
			{
				SetError(fmt::format("Parameter '{}': no entity {} in the scene", name, element.dump()));
				return {};
			}
			entities.push_back(entity);
		}
		return entities;
	}

	std::vector<std::string> CommandArguments::GetStringArray(std::string_view name)
	{
		std::vector<std::string> strings;
		const nlohmann::json* value = Get(name);
		if (!value)
			return strings;
		if (!value->is_array())
		{
			TypeError(name, "an array of strings");
			return strings;
		}
		for (const nlohmann::json& element : *value)
		{
			if (!element.is_string())
			{
				TypeError(name, "an array of strings");
				return {};
			}
			strings.push_back(element.get<std::string>());
		}
		return strings;
	}

}
