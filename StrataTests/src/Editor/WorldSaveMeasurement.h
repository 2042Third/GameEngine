#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// A save of a world scene measured as the person meets it (the Perf.Editor.Save* metrics, EditorWorldPerfTests.cpp): an
// editor with its UI (HarnessEditor, no GPU) opens the scene of a world project (Perf/WorldFixture.h), one edit makes it
// modified, and scene.save {wait: false} runs through the editor's command runner inside a frame, which is timed with every
// frame after it until the save was published. Private memory is sampled every millisecond meanwhile (and the kernel's
// peak read where it keeps one), so the save's peak is its own when the measurement runs in a helper process.
namespace Strata::Tests
{

	struct WorldSaveMeasurement
	{
		uint64_t Entities = 0;              // Written
		uint64_t Bytes = 0;                 // Written
		double OpenSeconds = 0.0;           // scene.open of the world before the save
		double SaveFrameMs = 0.0;           // The frame that started the save (its snapshot)
		double MaxFrameMs = 0.0;            // The longest frame from that one to the one that published the save
		uint64_t Frames = 0;                // From the save's frame to the publishing one, both included
		double Seconds = 0.0;               // From the start of the save's frame to the end of the publishing one
		double PrivateMBBefore = 0.0;       // Private memory (Platform::GetProcessMemory) before the save's frame
		double PeakPrivateIncreaseMB = 0.0; // The peak of private memory during the save over PrivateMBBefore
	};

	// Measures a save in this process. nullopt with the reason when the project, the scene's open, the edit or the save
	// failed.
	std::optional<WorldSaveMeasurement> MeasureWorldSave(const std::filesystem::path& projectDirectory, AssetHandle scene, std::string& error);

	// MeasureWorldSave in a helper process, whose memory is the editor's and the save's alone:
	//   --strata-test-helper=save-world <project directory> <scene handle> <result file>
	std::optional<WorldSaveMeasurement> MeasureWorldSaveInHelper(const std::filesystem::path& projectDirectory, AssetHandle scene, std::string& error);

	// The helper mode above; nullopt for other modes. Exit code 0 with the result file written, 1 when the measurement failed
	// (the reason on stderr), 2 for wrong arguments.
	std::optional<int> RunWorldSaveHelper(std::string_view mode, int argc, char** argv);

}
