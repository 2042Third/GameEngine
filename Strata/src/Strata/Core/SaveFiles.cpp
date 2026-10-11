#include "stpch.h"
#include "Strata/Core/SaveFiles.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace Strata::SaveFiles
{

	namespace
	{

		constexpr std::string_view c_Extension = ".save";

		bool IsAllowedCharacter(char character)
		{
			const unsigned char value = static_cast<unsigned char>(character);
			return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == ' ' || value == '-'
				|| value == '_' || value == '.';
		}

		bool EqualsIgnoringCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
			{
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		// The device names Windows reserves, which name no file whatever their extension.
		bool IsReservedDeviceName(std::string_view name)
		{
			const std::string_view stem = name.substr(0, name.find('.'));
			constexpr std::array<std::string_view, 4> c_Devices = { "CON", "PRN", "AUX", "NUL" };
			for (std::string_view device : c_Devices)
			{
				if (EqualsIgnoringCase(stem, device))
					return true;
			}
			if (stem.size() == 4 && (EqualsIgnoringCase(stem.substr(0, 3), "COM") || EqualsIgnoringCase(stem.substr(0, 3), "LPT")))
				return stem[3] >= '0' && stem[3] <= '9';
			return false;
		}

		std::filesystem::path GetPath(const std::filesystem::path& directory, std::string_view name)
		{
			return directory / FileSystem::FromUTF8(std::string(name) + std::string(c_Extension));
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	std::optional<std::string> ValidateName(std::string_view name)
	{
		if (name.empty())
			return std::string("the name is empty");
		if (name.size() > c_MaxNameLength)
			return fmt::format("the name is longer than {} characters", c_MaxNameLength);
		for (char character : name)
		{
			if (character == '/' || character == '\\')
				return std::string("the name contains a path separator");
			if (!IsAllowedCharacter(character))
				return std::string("the name may contain only ASCII letters, digits, spaces, '-', '_' and '.'");
		}
		if (name.find("..") != std::string_view::npos)
			return std::string("the name contains \"..\"");
		if (name.front() == ' ' || name.front() == '.' || name.back() == ' ' || name.back() == '.')
			return std::string("the name starts or ends with a space or a dot");
		if (IsReservedDeviceName(name))
			return fmt::format("'{}' is a device name", name);
		return std::nullopt;
	}

	bool Exists(const std::filesystem::path& directory, std::string_view name)
	{
		return !ValidateName(name) && FileSystem::IsRegularFile(GetPath(directory, name));
	}

	bool Write(const std::filesystem::path& directory, std::string_view name, std::span<const uint8_t> bytes, std::string* outError)
	{
		if (std::optional<std::string> problem = ValidateName(name))
			return Fail(outError, *problem);
		if (bytes.size() > c_MaxFileSize)
			return Fail(outError, fmt::format("{} bytes are more than a save file may hold ({} bytes)", bytes.size(), c_MaxFileSize));
		if (!FileSystem::CreateDirectories(directory))
			return Fail(outError, fmt::format("cannot create the save directory '{}'", FileSystem::ToUTF8(directory)));
		if (!FileSystem::WriteBytes(GetPath(directory, name), bytes))
			return Fail(outError, fmt::format("cannot write save file '{}' in '{}'", name, FileSystem::ToUTF8(directory)));
		return true;
	}

	std::optional<std::vector<uint8_t>> Read(const std::filesystem::path& directory, std::string_view name, std::string* outError)
	{
		if (std::optional<std::string> problem = ValidateName(name))
		{
			Fail(outError, *problem);
			return std::nullopt;
		}
		const std::filesystem::path path = GetPath(directory, name);
		if (!FileSystem::IsRegularFile(path))
		{
			Fail(outError, fmt::format("there is no save file '{}'", name));
			return std::nullopt;
		}
		const std::optional<uint64_t> size = FileSystem::GetFileSize(path);
		if (size && *size > c_MaxFileSize)
		{
			Fail(outError, fmt::format("save file '{}' is larger than a save file may be ({} bytes)", name, c_MaxFileSize));
			return std::nullopt;
		}
		std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		if (!bytes)
			Fail(outError, fmt::format("cannot read save file '{}'", name));
		return bytes;
	}

	bool Delete(const std::filesystem::path& directory, std::string_view name, std::string* outError)
	{
		if (std::optional<std::string> problem = ValidateName(name))
			return Fail(outError, *problem);
		const std::filesystem::path path = GetPath(directory, name);
		if (!FileSystem::IsRegularFile(path))
			return Fail(outError, fmt::format("there is no save file '{}'", name));
		std::error_code error;
		if (!std::filesystem::remove(path, error) || error)
			return Fail(outError, fmt::format("cannot delete save file '{}': {}", name, error.message()));
		return true;
	}

	std::string MakeGameDirectoryName(std::string_view gameName)
	{
		std::string name;
		for (size_t index = 0; index < gameName.size(); index++)
		{
			const unsigned char value = static_cast<unsigned char>(gameName[index]);
			// Bytes of multi-byte UTF-8 characters are kept (every platform's file names take them); ASCII is kept where every
			// platform takes it in a directory name.
			const bool refused = value < 0x20 || value == 0x7F || std::string_view("<>:\"/\\|?*").find(static_cast<char>(value)) != std::string_view::npos;
			name.push_back(refused ? '_' : static_cast<char>(value));
		}
		for (size_t dots = name.find(".."); dots != std::string::npos; dots = name.find("..", dots))
			name.replace(dots, 2, "__");
		const size_t first = name.find_first_not_of(" .");
		const size_t last = name.find_last_not_of(" .");
		name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
		if (name.size() > c_MaxNameLength)
		{
			// Cut before the character that would not fit whole: continuation bytes are 10xxxxxx.
			size_t length = c_MaxNameLength;
			while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80)
				length--;
			name.resize(length);
			name.erase(name.find_last_not_of(" .") + 1);
		}
		if (name.empty())
			return "Game";
		if (IsReservedDeviceName(name))
			name.insert(name.begin(), '_');
		return name;
	}

	std::vector<std::string> List(const std::filesystem::path& directory)
	{
		std::vector<std::string> names;
		std::error_code error;
		// Iterated with error codes: the range form's increment throws.
		for (std::filesystem::directory_iterator it(directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
		{
			std::error_code typeError;
			if (!it->is_regular_file(typeError) || typeError)
				continue;
			const std::string file = FileSystem::ToUTF8(it->path().filename());
			if (file.size() <= c_Extension.size() || file.compare(file.size() - c_Extension.size(), c_Extension.size(), c_Extension) != 0)
				continue;
			std::string name = file.substr(0, file.size() - c_Extension.size());
			// Files the game did not write (other names, copies left by interrupted writes) are not save files.
			if (!ValidateName(name))
				names.push_back(std::move(name));
		}
		std::sort(names.begin(), names.end());
		return names;
	}

}

namespace Strata
{

	SaveDirectory::SaveDirectory(std::filesystem::path directory)
		: m_Directory(std::move(directory))
	{
	}

	Scope<SaveDirectory> SaveDirectory::ForGame(std::string gameName)
	{
		Scope<SaveDirectory> saves = CreateScope<SaveDirectory>();
		saves->m_GameName = std::move(gameName);
		return saves;
	}

	void SaveDirectory::SetDirectory(std::filesystem::path directory)
	{
		m_Directory = std::move(directory);
		m_GameName.clear();
	}

	std::optional<std::filesystem::path> SaveDirectory::GetSaveDirectory()
	{
		if (m_Directory.empty() && !m_GameName.empty())
			m_Directory = Platform::GetUserDataDirectory(SaveFiles::MakeGameDirectoryName(m_GameName)) / "Saves";
		if (m_Directory.empty())
			return std::nullopt;
		return m_Directory;
	}

}
