#include "stpch.h"
#include "Strata/Runtime/GamePresentation.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Core/Version.h"

namespace Strata
{

	GamePresentation GamePresentation::Make(const GameManifest& manifest, const std::filesystem::path& manifestPath, const std::filesystem::path& executablePath)
	{
		GamePresentation presentation;
		presentation.Title = manifest.GetDisplayTitle();
		const std::string version = manifest.Version.empty() ? std::string() : " " + manifest.Version;
		presentation.Banner = fmt::format("{}{} (Strata {}, {})", presentation.Title, version, c_EngineVersion, Platform::GetName());
		presentation.DataDirectoryName = SaveFiles::MakeGameDirectoryName(manifest.Name);
		if (!manifest.Icon.empty())
			presentation.IconFile = manifestPath.parent_path() / FileSystem::FromUTF8(manifest.Icon);
		presentation.AppId = FileSystem::ToUTF8(executablePath.stem());
		return presentation;
	}

	void GamePresentation::ApplyTo(WindowSpecification& window) const
	{
		window.Title = Title;
		window.IconPath = IconFile;
		window.AppId = AppId;
	}

}
