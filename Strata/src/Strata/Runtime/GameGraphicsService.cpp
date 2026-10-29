#include "stpch.h"
#include "Strata/Runtime/GameGraphicsService.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Core/StringUtils.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <utility>

namespace Strata
{

	namespace
	{

		// Versions of every service of the process, so that a renderer that kept one notices a provider replaced by another.
		std::atomic<uint64_t> s_NextVersion { 1 };

		uint64_t NextVersion()
		{
			return s_NextVersion.fetch_add(1, std::memory_order_relaxed);
		}

		// The field of that name, ignoring case.
		const SettingsField* FindField(const SettingsSection& section, std::string_view name)
		{
			for (const SettingsField& field : section.Fields)
			{
				if (StringUtils::EqualsIgnoreCase(field.Name, name))
					return &field;
			}
			return nullptr;
		}

		std::string ListFields(const SettingsSection& section)
		{
			std::string names;
			for (const SettingsField& field : section.Fields)
				names += (names.empty() ? "" : ", ") + field.Name;
			return names;
		}

		// The section's value with the members of `options` over `base`.
		nlohmann::json Merge(const nlohmann::json& base, const nlohmann::json& options)
		{
			nlohmann::json merged = base;
			if (options.is_object())
			{
				for (const auto& [name, value] : options.items())
					merged[name] = value;
			}
			return merged;
		}

		// A positive whole number of a JSON member, or nullopt.
		std::optional<uint32_t> ReadVersion(const nlohmann::json& object, std::string_view key)
		{
			const nlohmann::json* member = JsonUtils::Find(object, key);
			if (!member || !member->is_number_integer())
				return std::nullopt;
			const int64_t value = member->get<int64_t>();
			if (value < 1 || value > static_cast<int64_t>(UINT32_MAX))
				return std::nullopt;
			return static_cast<uint32_t>(value);
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// GraphicsOptionsFile
	////////////////////////////////////////////////////////////////////////////////

	namespace GraphicsOptionsFile
	{

		bool Read(std::string_view text, const SettingsSection& section, const nlohmann::json& projectValue, nlohmann::json& outOptions,
			std::vector<std::string>* outDropped, std::string* outError)
		{
			auto fail = [outError](std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			};
			if (text.size() > c_MaxFileBytes)
				return fail(fmt::format("it is larger than {} KiB", c_MaxFileBytes / 1024));
			std::string parseError;
			const std::optional<nlohmann::json> root = JsonUtils::Parse(text, &parseError);
			if (!root || !root->is_object())
				return fail(root ? std::string("it is no JSON object") : fmt::format("it is no JSON: {}", parseError));
			const nlohmann::json* strata = JsonUtils::Find(*root, "Strata");
			const nlohmann::json* format = strata && strata->is_object() ? JsonUtils::Find(*strata, "Format") : nullptr;
			if (!format || !format->is_string() || format->get_ref<const std::string&>() != "GraphicsOptions")
				return fail("it is no graphics options file (Strata.Format is not \"GraphicsOptions\")");
			const std::optional<uint32_t> formatVersion = ReadVersion(*strata, "Version");
			if (!formatVersion)
				return fail("its Strata.Version is not a positive whole number");
			if (*formatVersion > c_FormatVersion)
			{
				return fail(fmt::format("it was written by a later version of the engine (format {}; this engine reads up to {})", *formatVersion,
					c_FormatVersion));
			}
			const nlohmann::json* sectionInfo = JsonUtils::Find(*root, "Section");
			if (!sectionInfo || !sectionInfo->is_object())
				return fail("it has no Section object");
			const nlohmann::json* id = JsonUtils::Find(*sectionInfo, "Id");
			if (!id || !id->is_string() || id->get_ref<const std::string&>() != section.Id)
			{
				return fail(fmt::format("it holds the options of another settings section ('{}', not '{}')",
					id && id->is_string() ? StringUtils::MakeValidUTF8(id->get_ref<const std::string&>()) : std::string("none"), section.Id));
			}
			const std::optional<uint32_t> sectionVersion = ReadVersion(*sectionInfo, "Version");
			if (!sectionVersion)
				return fail("its Section.Version is not a positive whole number");
			if (*sectionVersion > section.Version)
			{
				return fail(fmt::format("it was written by a later version of the game (section version {}; this game reads up to {})", *sectionVersion,
					section.Version));
			}
			const nlohmann::json* options = JsonUtils::Find(*root, "Options");
			if (!options || !options->is_object())
				return fail("it has no Options object");

			// Names matched ignoring case, in the fields' spelling (Validate matches them exactly); one member per field.
			nlohmann::json known = nlohmann::json::object();
			std::vector<std::string> dropped;
			for (const auto& [name, value] : options->items())
			{
				const SettingsField* field = FindField(section, name);
				if (!field)
				{
					dropped.push_back(fmt::format("'{}' is no graphics option of this game", StringUtils::MakeValidUTF8(name)));
					continue;
				}
				if (known.contains(field->Name))
					return fail(fmt::format("two members name '{}'", field->Name));
				known[field->Name] = value;
			}

			// The members upgraded and checked through the section's single validation, over the project's value.
			nlohmann::json result = nlohmann::json::object();
			if (std::optional<nlohmann::json> valid = section.Validate(Merge(projectValue, known), *sectionVersion))
			{
				for (const auto& [name, value] : known.items())
					result[name] = (*valid)[name];
			}
			else
			{
				for (const auto& [name, value] : known.items())
				{
					std::string error;
					std::optional<nlohmann::json> alone = section.Validate(Merge(projectValue, nlohmann::json { { name, value } }), *sectionVersion, &error);
					if (alone)
						result[name] = (*alone)[name];
					else
						dropped.push_back(StringUtils::MakeValidUTF8(error));
				}
			}
			outOptions = std::move(result);
			if (outDropped)
				*outDropped = std::move(dropped);
			return true;
		}

		std::string Write(const nlohmann::json& options, const SettingsSection& section)
		{
			const nlohmann::json document = {
				{ "Options", options.is_object() ? options : nlohmann::json::object() },
				{ "Section", { { "Id", section.Id }, { "Version", section.Version } } },
				{ "Strata", { { "Format", "GraphicsOptions" }, { "Version", c_FormatVersion } } }
			};
			return JsonUtils::Dump(document, 1, '\t') + "\n";
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// GameGraphicsService
	////////////////////////////////////////////////////////////////////////////////

	struct GameGraphicsService::Data
	{
		const IProjectSettings* Project = nullptr;
		IGameSaves* Saves = nullptr;
		RendererQuality::OptionModel Model; // Section null: no options
		bool EngineModel = false;

		bool ProjectRead = false;
		uint64_t ProjectVersion = 0; // Of the project's settings ProjectValue was read from
		nlohmann::json ProjectValue = nlohmann::json::object(); // Validated, at the section's version
		nlohmann::json Player = nlohmann::json::object();
		std::optional<RendererQualityPreset> CommandLine;

		nlohmann::json Effective = nlohmann::json::object();
		std::vector<GameGraphicsSource> Sources; // Per field of the section
		uint64_t Version = 0;

		uint64_t QualityVersion = 0; // Of the value Quality was made from
		RendererQualitySettings Quality = RendererQuality::GetPreset(RendererQualityPreset::High);

		const RendererQuality::RowTraits* FindRow(std::string_view field) const { return RendererQuality::FindRow(field, Model); }
		bool FollowsPreset(const SettingsField& field) const
		{
			const RendererQuality::RowTraits* row = FindRow(field.Name);
			return row && row->FollowsPreset;
		}

		// The project's and the player's values validated and spelled out, without the command line: what a player's change
		// starts from.
		nlohmann::json GetBase() const
		{
			std::optional<nlohmann::json> valid = Model.Section->Validate(Merge(ProjectValue, Player), Model.Section->Version);
			return RendererQuality::Normalize(valid ? *valid : ProjectValue, Model);
		}

		// The player's choices of the rows the presets leave alone.
		nlohmann::json GetComfortChoices() const
		{
			nlohmann::json choices = nlohmann::json::object();
			for (const auto& [name, value] : Player.items())
			{
				const SettingsField* field = FindField(*Model.Section, name);
				if (field && field->Name != "Preset" && !FollowsPreset(*field))
					choices[name] = value;
			}
			return choices;
		}

		// The effective value and the sources made again; the version moves when they or the player's choices changed.
		void Recompute(bool playerChanged)
		{
			const SettingsSection& section = *Model.Section;
			const nlohmann::json base = GetBase();
			nlohmann::json effective = base;
			if (CommandLine)
			{
				effective["Preset"] = RendererQuality::PresetToString(*CommandLine);
				const size_t index = static_cast<size_t>(*CommandLine);
				if (Model.PresetValues && index < c_RendererQualityPresetCount)
				{
					for (const RendererQuality::RowTraits& row : Model.Rows)
					{
						if (!row.FollowsPreset)
							continue;
						if (const nlohmann::json* value = JsonUtils::Find(Model.PresetValues[index], row.Field))
							effective[std::string(row.Field)] = *value;
					}
				}
			}
			const bool playerPreset = Player.contains("Preset");
			const nlohmann::json* basePreset = JsonUtils::Find(base, "Preset");
			const bool custom = basePreset && *basePreset == RendererQuality::PresetToString(RendererQualityPreset::Custom);
			std::vector<GameGraphicsSource> sources;
			sources.reserve(section.Fields.size());
			for (const SettingsField& field : section.Fields)
			{
				const bool isPreset = field.Name == "Preset";
				const bool follows = isPreset || FollowsPreset(field);
				// A row that follows the preset is the player's when their preset decides it, or when their own value of it is
				// in effect: under a named preset of the project's a member of theirs (a file written by hand) is not.
				const bool player = isPreset ? playerPreset
					: follows ? playerPreset || (custom && Player.contains(field.Name))
					: Player.contains(field.Name);
				if (CommandLine && follows)
					sources.push_back(GameGraphicsSource::CommandLine);
				else if (player)
					sources.push_back(GameGraphicsSource::Player);
				else
					sources.push_back(GameGraphicsSource::Project);
			}
			if (playerChanged || Version == 0 || effective != Effective || sources != Sources)
				Version = NextVersion();
			Effective = std::move(effective);
			Sources = std::move(sources);
		}

		// Pulls the project's value when its version changed: the player's choices stay on top, those the new value makes
		// invalid dropped with one warning.
		void Refresh()
		{
			if (!Model.Section)
				return;
			const uint64_t version = Project ? Project->GetVersion() : 0;
			if (ProjectRead && version == ProjectVersion)
				return;
			ProjectRead = true;
			ProjectVersion = version;
			const nlohmann::json* value = Project ? Project->FindSection(Model.Section->Id) : nullptr;
			ProjectValue = value ? *value : Model.Section->GetDefault();
			bool playerChanged = false;
			if (!Model.Section->Validate(Merge(ProjectValue, Player), Model.Section->Version))
			{
				std::vector<std::string> dropped;
				nlohmann::json kept = nlohmann::json::object();
				for (const auto& [name, member] : Player.items())
				{
					std::string error;
					if (Model.Section->Validate(Merge(ProjectValue, nlohmann::json { { name, member } }), Model.Section->Version, &error))
						kept[name] = member;
					else
						dropped.push_back(std::move(error));
				}
				Player = std::move(kept);
				playerChanged = true;
				ST_CORE_WARN("The players' graphics options set what the game no longer offers ({} dropped; the project's values apply there): {}",
					dropped.size(), dropped.empty() ? std::string() : dropped.front());
			}
			Recompute(playerChanged);
		}

		std::optional<std::filesystem::path> GetFile() const
		{
			const std::optional<std::filesystem::path> directory = Saves ? Saves->GetSaveDirectory() : std::nullopt;
			if (!directory)
				return std::nullopt;
			return *directory / FileSystem::FromUTF8(GraphicsOptionsFile::c_FileName);
		}

		// The file's choices read against `projectValue`: nullopt without a file; otherwise the choices (empty when the file is
		// refused) and in `problem` the warning text of a refused file or of dropped members.
		std::optional<nlohmann::json> ReadFile(const nlohmann::json& projectValue, std::string& problem) const
		{
			problem.clear();
			const std::optional<std::filesystem::path> file = GetFile();
			if (!Model.Section || !file || !FileSystem::Exists(*file))
				return std::nullopt;
			const std::string path = FileSystem::ToUTF8(*file);
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytesPrefix(*file, GraphicsOptionsFile::c_MaxFileBytes + 1);
			nlohmann::json options = nlohmann::json::object();
			std::vector<std::string> dropped;
			std::string error = "it cannot be read";
			if (!bytes || !GraphicsOptionsFile::Read(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), *Model.Section,
				projectValue, options, &dropped, &error))
			{
				problem = fmt::format("The players' graphics options '{}' are ignored ({}): the project's quality applies", path, error);
				return nlohmann::json::object();
			}
			if (!dropped.empty())
			{
				problem = fmt::format("The players' graphics options '{}' set what the game no longer offers ({} dropped; the project's values apply "
					"there): {}", path, dropped.size(), dropped.front());
			}
			return options;
		}
	};

	GameGraphicsService::GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const SettingsRegistry* registry)
		: m_Data(CreateScope<Data>())
	{
		m_Data->Project = project;
		m_Data->Saves = saves;
		const SettingsRegistry& settings = registry ? *registry : SettingsRegistry::GetProcessRegistry();
		if (const SettingsSection* section = settings.Find(RendererQuality::c_SectionId))
		{
			m_Data->Model = RendererQuality::GetOptionModel(*section);
			m_Data->EngineModel = true;
		}
	}

	GameGraphicsService::GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const RendererQuality::OptionModel& model)
		: m_Data(CreateScope<Data>())
	{
		m_Data->Project = project;
		m_Data->Saves = saves;
		m_Data->Model = model;
	}

	GameGraphicsService::~GameGraphicsService() = default;

	void GameGraphicsService::SetCommandLinePreset(std::optional<RendererQualityPreset> preset)
	{
		if (preset == RendererQualityPreset::Custom)
			preset.reset();
		m_Data->Refresh();
		if (m_Data->CommandLine == preset)
			return;
		m_Data->CommandLine = preset;
		if (m_Data->Model.Section)
			m_Data->Recompute(false);
	}

	std::optional<RendererQualityPreset> GameGraphicsService::GetCommandLinePreset() const
	{
		return m_Data->CommandLine;
	}

	void GameGraphicsService::LoadPlayerOptions()
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.Model.Section)
			return;
		std::string problem;
		std::optional<nlohmann::json> options = data.ReadFile(data.ProjectValue, problem);
		if (!problem.empty())
			ST_CORE_WARN("{}", problem);
		data.Player = options ? std::move(*options) : nlohmann::json::object();
		data.Recompute(true);
	}

	void GameGraphicsService::ResetSession()
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.Model.Section)
			return;
		const bool changed = !data.Player.empty();
		data.Player = nlohmann::json::object();
		data.Recompute(changed);
	}

	nlohmann::json GameGraphicsService::ReadSavedOptions(std::string* outProblem) const
	{
		const Data& data = *m_Data;
		std::string problem;
		std::optional<nlohmann::json> options;
		if (data.Model.Section)
		{
			// The project's value as it is now (the service may not have pulled it yet: this is const).
			const nlohmann::json* value = data.Project ? data.Project->FindSection(data.Model.Section->Id) : nullptr;
			options = data.ReadFile(value ? *value : data.Model.Section->GetDefault(), problem);
		}
		if (outProblem)
			*outProblem = std::move(problem);
		return options ? std::move(*options) : nlohmann::json::object();
	}

	bool GameGraphicsService::DeleteSavedOptions(std::string* outError)
	{
		const std::optional<std::filesystem::path> file = m_Data->GetFile();
		if (!file || !FileSystem::Exists(*file))
			return true;
		if (!FileSystem::Remove(*file) || FileSystem::Exists(*file))
		{
			if (outError)
				*outError = fmt::format("Removing the players' graphics options '{}' failed", FileSystem::ToUTF8(*file));
			return false;
		}
		return true;
	}

	std::optional<std::filesystem::path> GameGraphicsService::GetOptionsFile() const
	{
		return m_Data->GetFile();
	}

	const RendererQualitySettings& GameGraphicsService::GetQuality()
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.EngineModel || data.QualityVersion == data.Version)
			return data.Quality;
		data.Quality = RendererQuality::FromSectionValue(&data.Effective);
		data.QualityVersion = data.Version;
		return data.Quality;
	}

	const SettingsSection* GameGraphicsService::GetSection() const
	{
		return m_Data->Model.Section;
	}

	const nlohmann::json& GameGraphicsService::GetEffectiveValue()
	{
		m_Data->Refresh();
		return m_Data->Effective;
	}

	uint64_t GameGraphicsService::GetVersion()
	{
		m_Data->Refresh();
		return m_Data->Version;
	}

	std::optional<GameGraphicsSource> GameGraphicsService::GetSource(std::string_view option)
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.Model.Section)
			return std::nullopt;
		const std::vector<SettingsField>& fields = data.Model.Section->Fields;
		for (size_t index = 0; index < fields.size() && index < data.Sources.size(); index++)
		{
			if (StringUtils::EqualsIgnoreCase(fields[index].Name, option))
				return data.Sources[index];
		}
		return std::nullopt;
	}

	const nlohmann::json& GameGraphicsService::GetPlayerOptions()
	{
		m_Data->Refresh();
		return m_Data->Player;
	}

	bool GameGraphicsService::SetOption(std::string_view option, const nlohmann::json& value, std::string* outError)
	{
		Data& data = *m_Data;
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		data.Refresh();
		if (!data.Model.Section)
			return fail("the program running the game has no graphics options");
		const SettingsSection& section = *data.Model.Section;
		const SettingsField* field = FindField(section, option);
		if (!field)
			return fail(fmt::format("there is no graphics option '{}' (options: {})", StringUtils::MakeValidUTF8(option), ListFields(section)));

		// A choice's name in its option's spelling.
		nlohmann::json stored = value;
		if (field->Type == SettingsFieldType::Enum && value.is_string())
		{
			for (const std::string& name : field->Options)
			{
				if (StringUtils::EqualsIgnoreCase(name, value.get_ref<const std::string&>()))
					stored = name;
			}
		}

		const nlohmann::json base = data.GetBase();
		// The player's choices of the preset and the rows that follow it, for a switch to a preset (Custom: from the look in
		// effect, the rows that follow the preset spelled out), with the choices the presets leave alone kept.
		auto choosePreset = [&](const nlohmann::json& preset)
		{
			nlohmann::json player = data.GetComfortChoices();
			if (preset.is_string() && preset.get_ref<const std::string&>() == RendererQuality::PresetToString(RendererQualityPreset::Custom))
			{
				const nlohmann::json start = RendererQuality::MakeCustomStart(base, data.Model);
				for (const RendererQuality::RowTraits& row : data.Model.Rows)
				{
					if (row.FollowsPreset)
						player[std::string(row.Field)] = start[std::string(row.Field)];
				}
			}
			player["Preset"] = preset;
			return player;
		};
		nlohmann::json player = data.Player;
		if (field->Name == "Preset")
		{
			player = choosePreset(stored);
		}
		else if (data.FollowsPreset(*field))
		{
			// Unless the player's own preset is Custom, Custom first, from the look in effect (also under the project's Custom:
			// the player's tuning then holds every row and keeps its meaning when the project's preset changes).
			const nlohmann::json* preset = JsonUtils::Find(data.Player, "Preset");
			if (!preset || *preset != RendererQuality::PresetToString(RendererQualityPreset::Custom))
				player = choosePreset(RendererQuality::PresetToString(RendererQualityPreset::Custom));
			player[field->Name] = stored;
		}
		else
		{
			player[field->Name] = stored;
		}

		std::string error;
		const std::optional<nlohmann::json> valid = section.Validate(Merge(data.ProjectValue, player), section.Version, &error);
		if (!valid)
			return fail(error);
		// The members in their stored form (a Float as a floating-point number).
		for (auto& [name, member] : player.items())
			member = (*valid)[name];
		const bool changed = player != data.Player;
		data.Player = std::move(player);
		data.Recompute(changed);
		return true;
	}

	bool GameGraphicsService::ResetOptions(std::string_view option, std::string* outError)
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.Model.Section)
		{
			if (outError)
				*outError = "the program running the game has no graphics options";
			return false;
		}
		const SettingsSection& section = *data.Model.Section;
		nlohmann::json player = data.Player;
		if (option.empty())
		{
			player = nlohmann::json::object();
		}
		else
		{
			const SettingsField* field = FindField(section, option);
			if (!field)
			{
				if (outError)
					*outError = fmt::format("there is no graphics option '{}' (options: {})", StringUtils::MakeValidUTF8(option), ListFields(section));
				return false;
			}
			if (field->Name == "Preset")
			{
				// The preset and the rows it decides: the project's quality applies again.
				player.erase("Preset");
				for (const RendererQuality::RowTraits& row : data.Model.Rows)
				{
					if (row.FollowsPreset)
						player.erase(std::string(row.Field));
				}
			}
			else if (data.FollowsPreset(*field) && player.contains("Preset")
				&& player["Preset"] == RendererQuality::PresetToString(RendererQualityPreset::Custom))
			{
				// Under the player's Custom every row is the player's: this one takes what the project would use for it.
				player[field->Name] = RendererQuality::Normalize(data.ProjectValue, data.Model)[field->Name];
			}
			else
			{
				player.erase(field->Name);
			}
		}
		const bool changed = player != data.Player;
		data.Player = std::move(player);
		data.Recompute(changed);
		return true;
	}

	bool GameGraphicsService::SaveOptions(std::string* outError)
	{
		Data& data = *m_Data;
		data.Refresh();
		if (!data.Model.Section)
		{
			if (outError)
				*outError = "the program running the game has no graphics options";
			return false;
		}
		const std::optional<std::filesystem::path> file = data.GetFile();
		if (!file)
		{
			if (outError)
				*outError = "the game has no save directory to keep the players' graphics options in";
			return false;
		}
		if (!FileSystem::WriteText(*file, GraphicsOptionsFile::Write(data.Player, *data.Model.Section)))
		{
			if (outError)
				*outError = fmt::format("writing the players' graphics options to '{}' failed", FileSystem::ToUTF8(*file));
			return false;
		}
		return true;
	}

}
