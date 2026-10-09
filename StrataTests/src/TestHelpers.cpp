#include "TestHelpers.h"

#include "Strata/Core/UUID.h"

#include <mutex>
#include <vector>

namespace Strata::Tests
{

	static std::mutex s_TemporaryDirectoriesMutex;
	static std::vector<std::filesystem::path> s_TemporaryDirectories;

	std::filesystem::path CreateTemporaryDirectory(const std::string& name)
	{
		std::filesystem::path directory = std::filesystem::temp_directory_path() / "StrataTests" / (name + "_" + UUID().ToString());
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		std::filesystem::create_directories(directory, error);

		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		s_TemporaryDirectories.push_back(directory);
		return directory;
	}

	void CleanupTemporaryDirectories()
	{
		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		for (const std::filesystem::path& directory : s_TemporaryDirectories)
		{
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
		s_TemporaryDirectories.clear();
	}

}
