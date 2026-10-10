#include "UI/FileDialogs.h"

#include "UI/TextFormat.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <nfd.h>

namespace Strata
{

	namespace
	{

		bool s_Initialized = false;

		// Keeps the UTF-8 strings alive for the filter items that point into them.
		struct FilterList
		{
			std::vector<FileDialogFilter> Filters;
			std::vector<nfdu8filteritem_t> Items;

			explicit FilterList(const std::vector<FileDialogFilter>& filters)
				: Filters(filters)
			{
				for (const FileDialogFilter& filter : Filters)
					Items.push_back({ filter.Name.c_str(), filter.Extensions.c_str() });
			}
		};

		// The directory, or its nearest parent that exists, in the system's own spelling: the dialogs fail on a default
		// directory whose parent is missing (e.g. a remembered location that was deleted). Empty for none.
		std::string ToDefaultDirectory(const std::filesystem::path& directory)
		{
			std::filesystem::path existing = directory;
			while (!existing.empty() && !FileSystem::IsDirectory(existing))
			{
				std::filesystem::path parent = existing.parent_path();
				if (parent == existing)
					return {};
				existing = std::move(parent);
			}
			return existing.empty() ? std::string() : UI::DisplayPath(existing);
		}

		std::optional<std::filesystem::path> TakeResult(nfdresult_t result, nfdu8char_t* path, const char* dialog)
		{
			if (result == NFD_OKAY)
			{
				std::filesystem::path selected = FileSystem::FromUTF8(path);
				NFD_FreePathU8(path);
				return selected;
			}
			if (result == NFD_ERROR)
				ST_ERROR("{} dialog failed: {}", dialog, NFD_GetError());
			return std::nullopt;
		}

	}

	namespace FileDialogs
	{

		bool Init()
		{
			if (s_Initialized)
				return true;
			s_Initialized = NFD_Init() == NFD_OKAY;
			if (!s_Initialized)
				ST_ERROR("File dialogs are unavailable: {}", NFD_GetError());
			return s_Initialized;
		}

		void Shutdown()
		{
			if (s_Initialized)
				NFD_Quit();
			s_Initialized = false;
		}

		std::optional<std::filesystem::path> OpenFile(const std::vector<FileDialogFilter>& filters, const std::filesystem::path& defaultDirectory)
		{
			if (!s_Initialized)
				return std::nullopt;
			const FilterList list(filters);
			const std::string directory = ToDefaultDirectory(defaultDirectory);
			nfdu8char_t* path = nullptr;
			const nfdresult_t result = NFD_OpenDialogU8(&path, list.Items.empty() ? nullptr : list.Items.data(), static_cast<nfdfiltersize_t>(list.Items.size()),
				directory.empty() ? nullptr : directory.c_str());
			return TakeResult(result, path, "Open");
		}

		std::optional<std::filesystem::path> SaveFile(const std::vector<FileDialogFilter>& filters, const std::string& defaultName, const std::filesystem::path& defaultDirectory)
		{
			if (!s_Initialized)
				return std::nullopt;
			const FilterList list(filters);
			const std::string directory = ToDefaultDirectory(defaultDirectory);
			nfdu8char_t* path = nullptr;
			const nfdresult_t result = NFD_SaveDialogU8(&path, list.Items.empty() ? nullptr : list.Items.data(), static_cast<nfdfiltersize_t>(list.Items.size()),
				directory.empty() ? nullptr : directory.c_str(), defaultName.empty() ? nullptr : defaultName.c_str());
			return TakeResult(result, path, "Save");
		}

		std::optional<std::filesystem::path> PickFolder(const std::filesystem::path& defaultDirectory)
		{
			if (!s_Initialized)
				return std::nullopt;
			const std::string directory = ToDefaultDirectory(defaultDirectory);
			nfdu8char_t* path = nullptr;
			const nfdresult_t result = NFD_PickFolderU8(&path, directory.empty() ? nullptr : directory.c_str());
			return TakeResult(result, path, "Folder");
		}

	}

}
