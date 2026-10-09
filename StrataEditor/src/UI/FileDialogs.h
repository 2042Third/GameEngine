#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	struct FileDialogFilter
	{
		std::string Name;       // e.g. "Strata Project"
		std::string Extensions; // Comma-separated, without dots, e.g. "stproj"
	};

	// Native file dialogs (blocking, main thread). Initialize once with Init before use. Return nothing when the user
	// cancels or the dialog fails (failures are logged).
	namespace FileDialogs
	{
		bool Init();
		void Shutdown();

		std::optional<std::filesystem::path> OpenFile(const std::vector<FileDialogFilter>& filters, const std::filesystem::path& defaultDirectory = {});
		std::optional<std::filesystem::path> SaveFile(const std::vector<FileDialogFilter>& filters, const std::string& defaultName = {},
			const std::filesystem::path& defaultDirectory = {});
		std::optional<std::filesystem::path> PickFolder(const std::filesystem::path& defaultDirectory = {});
	}

}
