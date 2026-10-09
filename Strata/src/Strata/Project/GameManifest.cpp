#include "stpch.h"
#include "Strata/Project/GameManifest.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyJson.h"

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MaxWindowSize = 16384;

		// Files a manifest names must lie next to it: a manifest cannot point the runtime elsewhere.
		bool IsPlainFileName(const std::string& name)
		{
			const std::filesystem::path path = FileSystem::FromUTF8(name);
			return !name.empty() && !path.has_parent_path() && !path.is_absolute() && path.filename() == path;
		}

	}

	nlohmann::json GameManifest::ToJson() const
	{
		return {
			{ "Strata", { { "Format", "Game" }, { "Version", c_FormatVersion } } },
			{ "Game", {
				{ "Name", Name },
				{ "AssetPack", AssetPack },
				{ "StartScene", UUIDToJson(StartScene) },
				{ "ScriptModule", ScriptModule },
				{ "Window", { { "Width", WindowWidth }, { "Height", WindowHeight }, { "Fullscreen", Fullscreen }, { "VSync", VSync } } } } } };
	}

	std::optional<GameManifest> GameManifest::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<GameManifest>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};

		const nlohmann::json* header = JsonUtils::Find(json, "Strata");
		if (!header || JsonUtils::GetString(*header, "Format") != "Game")
			return fail("Not a Strata game manifest");
		const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
		if (version == 0 || version > c_FormatVersion)
			return fail(fmt::format("Unsupported game manifest version {} (this runtime reads 1-{})", version, c_FormatVersion));
		const nlohmann::json* game = JsonUtils::Find(json, "Game");
		if (!game || !game->is_object())
			return fail("The manifest has no \"Game\" object");

		GameManifest manifest;
		manifest.Name = JsonUtils::GetString(*game, "Name", manifest.Name);
		manifest.AssetPack = JsonUtils::GetString(*game, "AssetPack");
		if (!IsPlainFileName(manifest.AssetPack))
			return fail("\"AssetPack\" must be the file name of the asset pack next to the manifest");
		manifest.ScriptModule = JsonUtils::GetString(*game, "ScriptModule");
		if (!manifest.ScriptModule.empty() && !IsPlainFileName(manifest.ScriptModule))
			return fail("\"ScriptModule\" must be the file name of the script module next to the manifest");
		const nlohmann::json* startScene = JsonUtils::Find(*game, "StartScene");
		std::optional<UUID> scene = startScene ? UUIDFromJson(*startScene) : std::nullopt;
		if (!scene || !scene->IsValid())
			return fail("\"StartScene\" must be a scene handle");
		manifest.StartScene = *scene;

		if (const nlohmann::json* window = JsonUtils::Find(*game, "Window"); window && window->is_object())
		{
			manifest.WindowWidth = static_cast<uint32_t>(std::clamp<uint64_t>(JsonUtils::GetUInt(*window, "Width", manifest.WindowWidth), 1, c_MaxWindowSize));
			manifest.WindowHeight = static_cast<uint32_t>(std::clamp<uint64_t>(JsonUtils::GetUInt(*window, "Height", manifest.WindowHeight), 1, c_MaxWindowSize));
			manifest.Fullscreen = JsonUtils::GetBool(*window, "Fullscreen", manifest.Fullscreen);
			manifest.VSync = JsonUtils::GetBool(*window, "VSync", manifest.VSync);
		}
		return manifest;
	}

	bool GameManifest::Save(const std::filesystem::path& path, std::string* outError) const
	{
		if (!FileSystem::WriteText(path, JsonUtils::Dump(ToJson(), 1, '\t') + "\n"))
		{
			if (outError)
				*outError = fmt::format("Could not write '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		return true;
	}

	std::optional<GameManifest> GameManifest::Load(const std::filesystem::path& path, std::string* outError)
	{
		std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			if (outError)
				*outError = fmt::format("Cannot read '{}'", FileSystem::ToUTF8(path));
			return std::nullopt;
		}
		std::optional<nlohmann::json> json = JsonUtils::Parse(*text, outError);
		if (!json)
			return std::nullopt;
		return FromJson(*json, outError);
	}

	std::filesystem::path GameManifest::FindForExecutable(const std::filesystem::path& executablePath)
	{
		const std::filesystem::path directory = executablePath.parent_path();
		std::filesystem::path named = directory / executablePath.stem();
		named += c_FileExtension;
		if (FileSystem::IsRegularFile(named))
			return named;

		std::filesystem::path found;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			std::error_code typeError;
			if (it->path().extension() != c_FileExtension || !it->is_regular_file(typeError))
				continue;
			if (!found.empty())
				return {}; // Ambiguous
			found = it->path();
		}
		return found;
	}

}
