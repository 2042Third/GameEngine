#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// A finished project that ships with the editor to learn from (e.g. Tetris).
	struct ProjectSample
	{
		std::string Id;                    // Its directory in the samples directory, e.g. "Tetris"
		std::string Name;
		std::string Description;
		std::filesystem::path ProjectFile; // The sample's .stproj (never opened in place: Copy it)
		// Its picture (the index's optional "Picture": a PNG inside its directory, ProjectThumbnail's size), which the launcher's
		// card shows and a copy starts with as its thumbnail; empty without one.
		std::filesystem::path Picture;
	};

	// The samples the editor offers (project.samples, project.openSample, the launcher's Open Sample). They live in a samples
	// directory next to the editor executable, which StrataEditor's build fills from the repository's Samples/, listed by
	// its index:
	//   Samples.json: {"Strata": {"Format": "Samples", "Version": 1},
	//                  "Samples": [{"Directory": "Tetris", "Name": "Tetris", "Description": "...", "Picture": "Picture.png"}]}
	// Samples are opened as copies, so the originals stay as they shipped.
	class ProjectSamples
	{
	public:
		static constexpr const char* c_IndexFileName = "Samples.json";
		// Local editor data (import cache, script builds, editor state) that a copy leaves out.
		static constexpr const char* c_LocalDataDirectory = ".strata";

		// <the executable's directory>/Samples.
		static std::filesystem::path GetDefaultDirectory();

		// The samples of the directory's index whose project exists, in the index's order. nullopt, with the reason, when the
		// index is missing or invalid (entries need a Directory naming one directory of the samples directory, a Name and,
		// optionally, a Description and a Picture). A picture that is not a relative path inside the sample's directory naming
		// a PNG file leaves the sample without one (the log says why): the sample itself stays usable.
		static std::optional<std::vector<ProjectSample>> List(const std::filesystem::path& directory, std::string* outError = nullptr);

		// Copies the sample's project into `destination` (created; it may exist only as an empty directory), without its
		// local editor data. The sample's picture becomes the copy's thumbnail (ProjectThumbnail::GetFile), so the launcher shows
		// it before the copy's first save. Returns the copy's project file, or an empty path with the reason; a failed copy
		// removes what it created.
		static std::filesystem::path Copy(const ProjectSample& sample, const std::filesystem::path& destination, std::string* outError = nullptr);
	};

}
