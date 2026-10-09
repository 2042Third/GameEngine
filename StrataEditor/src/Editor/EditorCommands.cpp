#include "Editor/EditorCommands.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorContext.h"

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

	EditorCommandRegistry::EditorCommandRegistry()
	{
		RegisterSceneCommands(*this);
		RegisterAssetCommands(*this);
		RegisterEditorStateCommands(*this);
		RegisterViewportCommands(*this);

		Register({ "editor.commands", "Every editor command with its description and JSON Schema of its parameters.", CommandUtils::ObjectSchema({}),
			[this](EditorContext&, const nlohmann::json&)
			{
				nlohmann::json commands = nlohmann::json::array();
				for (const EditorCommand* command : GetAll())
					commands.push_back({ { "name", command->Name }, { "description", command->Description }, { "parameters", command->Parameters } });
				return EditorCommandResult::Ok({ { "commands", std::move(commands) } });
			} });
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

	EditorCommandResult EditorCommandRegistry::Execute(EditorContext& context, std::string_view name, const nlohmann::json& parameters) const
	{
		const EditorCommand* command = Find(name);
		if (!command)
			return EditorCommandResult::Fail(fmt::format("Unknown command '{}' (editor.commands lists them)", name), EditorCommandError::UnknownCommand);
		if (!parameters.is_object() && !parameters.is_null())
			return EditorCommandResult::InvalidParameters("Command parameters must be a JSON object");

		const nlohmann::json& arguments = parameters.is_null() ? nlohmann::json::object() : parameters;
		try
		{
			// The schema is the contract: a misspelled or missing parameter must not silently change the meaning.
			if (const auto declared = command->Parameters.find("properties"); declared != command->Parameters.end() && declared->is_object())
			{
				for (const auto& [key, value] : arguments.items())
				{
					if (declared->contains(key))
						continue;
					std::string expected;
					for (const auto& [parameter, schema] : declared->items())
						expected += (expected.empty() ? "" : ", ") + parameter;
					return EditorCommandResult::InvalidParameters(fmt::format("Unknown parameter '{}' for {} (parameters: {})", key, command->Name, expected.empty() ? "none" : expected));
				}
			}
			if (const auto required = command->Parameters.find("required"); required != command->Parameters.end() && required->is_array())
			{
				for (const nlohmann::json& parameter : *required)
				{
					if (parameter.is_string() && !arguments.contains(parameter.get<std::string>()))
						return EditorCommandResult::InvalidParameters(fmt::format("Missing parameter '{}' for {}", parameter.get<std::string>(), command->Name));
				}
			}
			return command->Handler(context, arguments);
		}
		catch (const std::exception& exception)
		{
			// Engine code does not throw; this guards third-party code (JSON access) used by handlers.
			return EditorCommandResult::Fail(fmt::format("Command '{}' failed: {}", name, exception.what()), EditorCommandError::Internal);
		}
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

}
