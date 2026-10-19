#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The memory an import of a folder takes (Perf.Editor.ImportFolder24x4kPeakPrivate, WorldIOPerfTests.cpp): an asset manager
// scans an empty project, its import memory budget is set, and EditorAssetManager::ImportExternal copies the folder in and
// imports it in the background until every import is applied. The peak is the kernel's peak of the process, so the
// measurement runs in a helper process of its own.
namespace Strata::Tests
{

	struct ImportMemoryMeasurement
	{
		double PrivateMBBefore = 0.0;       // After the scan, before the import
		double PeakPrivateIncreaseMB = 0.0; // The process's peak private memory over PrivateMBBefore
		double Seconds = 0.0;               // From the import's start until every import was applied
		uint64_t Imported = 0;              // Assets imported
	};

	std::optional<ImportMemoryMeasurement> MeasureFolderImport(const std::filesystem::path& projectDirectory, const std::filesystem::path& folder,
		uint64_t budgetBytes, std::string& error);
	// MeasureFolderImport in a helper process:
	//   --strata-test-helper=import-folder <project directory> <folder> <budget MB> <result file>
	std::optional<ImportMemoryMeasurement> MeasureFolderImportInHelper(const std::filesystem::path& projectDirectory, const std::filesystem::path& folder,
		uint64_t budgetBytes, std::string& error);
	// The helper mode above; nullopt for other modes.
	std::optional<int> RunImportMemoryHelper(std::string_view mode, int argc, char** argv);

}
