#include "Perf/WorldFixture.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <thread>

// The editor half of Perf/WorldFixture.h: exporting a world project needs the editor's GameExport, which builds without the
// editor do not have.
namespace Strata::Tests
{

	namespace
	{

		// How long one step of an export may take: opening a project with a million-entity scene searches its references,
		// and the export compresses it.
		constexpr std::chrono::minutes c_WorldGameStepTimeout { 10 };

		// Runs a command to its end, one editor frame at a time while it is pending; its value, or nullopt with the error.
		std::optional<nlohmann::json> RunCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
			const nlohmann::json& parameters, std::string& error)
		{
			EditorCommandResult result = commands.Execute(context, name, parameters);
			const auto deadline = std::chrono::steady_clock::now() + c_WorldGameStepTimeout;
			while (result.IsPending())
			{
				if (std::chrono::steady_clock::now() > deadline)
				{
					error = fmt::format("{} did not finish within {} minutes", name, c_WorldGameStepTimeout.count());
					return std::nullopt;
				}
				context.Update(Timestep(1.0f / 60.0f));
				if (!PollCommandResult(result, context))
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!result.Success)
			{
				error = fmt::format("{} failed: {}", name, result.Error);
				return std::nullopt;
			}
			return result.Value;
		}

	}

	std::optional<WorldGame> WriteWorldGame(const WorldProject& project, const std::filesystem::path& outputDirectory, std::string& error,
		const WorldGameOptions& options)
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		if (!RunCommand(context, commands, "project.open", { { "path", FileSystem::ToUTF8(project.ProjectFile) } }, error))
			return std::nullopt;
		if (options.StartScene.IsValid() && !RunCommand(context, commands, "project.setStartScene", { { "scene", options.StartScene.ToString() } }, error))
			return std::nullopt;
		const std::optional<nlohmann::json> exported = RunCommand(context, commands, "project.export",
			{ { "directory", FileSystem::ToUTF8(outputDirectory) }, { "includeRuntime", options.IncludeRuntime } }, error);
		if (!exported)
			return std::nullopt;

		WorldGame game;
		game.Directory = outputDirectory;
		game.Manifest = FileSystem::FromUTF8(JsonUtils::GetString(*exported, "manifest"));
		game.AssetPack = FileSystem::FromUTF8(JsonUtils::GetString(*exported, "assetPack"));
		game.Executable = FileSystem::FromUTF8(JsonUtils::GetString(*exported, "executable"));
		if (game.Manifest.empty() || game.AssetPack.empty())
		{
			error = "project.export named no manifest or asset pack";
			return std::nullopt;
		}
		return game;
	}

}
