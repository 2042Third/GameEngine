#pragma once

#include <Strata/Core/Base.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// A file of a project's script directory that the script build reads: code (.cpp, .h and their kin) or CMake.
	struct ScriptSourceFile
	{
		std::string Path;                 // Relative to the script directory, '/' separators
		std::vector<std::string> Classes; // The script classes it registers (ST_SCRIPT_CLASS), in the order it does
		uint64_t Size = 0;
		int64_t LastWriteTime = 0;        // FileSystem::GetLastWriteTime

		bool operator==(const ScriptSourceFile&) const = default;
	};

	struct ScriptSourceListing
	{
		std::vector<ScriptSourceFile> Files; // Sorted by path, ignoring case
		bool Truncated = false;              // The directory holds more than c_MaxListedScriptSources files (or a huge tree)

		bool operator==(const ScriptSourceListing&) const = default;
	};

	// The most files a listing holds, and the most directory entries it looks at, so a script directory someone filled
	// with other things (a dependency checked out into it) cannot make a listing take long.
	inline constexpr size_t c_MaxListedScriptSources = 4096;
	inline constexpr size_t c_MaxScannedScriptEntries = 65536;
	// Code files up to this size are read for their script classes (larger ones are generated or vendored code).
	inline constexpr uint64_t c_MaxClassScanBytes = 1024 * 1024;

	// Whether a file name is C or C++ code the script module compiles or includes (.cpp, .cc, .cxx, .c, .h, .hpp, .hh,
	// .hxx, .inl, .ipp; ignoring case).
	bool IsScriptCodeFileName(std::string_view fileName);
	// Whether a file name is one of the script build's sources: code, CMakeLists.txt or a .cmake file.
	bool IsScriptSourceFileName(std::string_view fileName);

	// The classes a script source registers: the names in its ST_SCRIPT_CLASS(<Name>) lines, outside comments, string
	// literals and preprocessor lines (the SDK's own definition of the macro).
	std::vector<std::string> FindScriptClassRegistrations(std::string_view code);

	// The code and CMake files below a script directory. Folders whose names start with '.' (an IDE's settings, version
	// control) and build trees (folders with a CMakeCache.txt, e.g. an IDE's build inside the scripts) are left out. Code
	// files are read for their ST_SCRIPT_CLASS registrations; `previous` (an earlier listing of the same directory) lends
	// the classes of files whose size and write time did not change, so listing again reads only what changed. A missing
	// directory lists nothing. The limits are c_MaxListedScriptSources and c_MaxScannedScriptEntries unless given.
	ScriptSourceListing ListScriptSources(const std::filesystem::path& directory, const ScriptSourceListing* previous = nullptr,
		size_t maxFiles = c_MaxListedScriptSources, size_t maxEntries = c_MaxScannedScriptEntries);
	// The same files with their sizes and write times, read from the directory alone (Classes stay empty): what a script
	// build compiles, to compare with what an earlier build compiled (FindScriptSourceChange).
	ScriptSourceListing StampScriptSources(const std::filesystem::path& directory);

	enum class ScriptSourceChangeKind : uint8_t
	{
		Changed = 0, // Another size or write time
		Added,
		Removed
	};

	// "changed", "was added", "was removed".
	const char* ScriptSourceChangeKindToString(ScriptSourceChangeKind kind);

	struct ScriptSourceChange
	{
		std::string Path; // Relative to the script directory (ScriptSourceFile::Path)
		ScriptSourceChangeKind Kind = ScriptSourceChangeKind::Changed;
	};

	// The first difference between two listings of a script directory (a file whose size or write time changed, then one
	// that was added, then one that was removed), or nothing when they hold the same files.
	std::optional<ScriptSourceChange> FindScriptSourceChange(const ScriptSourceListing& before, const ScriptSourceListing& after);

	// Lists a script directory on an I/O thread (ListScriptSources) for the main thread, which polls Update once per frame
	// while it shows the files: nothing runs between refreshes, and a refresh asked for while a listing runs starts when it
	// finished. The job compares its listing with the one before, so the main thread neither copies nor compares files:
	// GetVersion changes only when the files differ, and views rebuild their rows only then. The job shares only its own
	// state, so destroying the list never waits for it. Main thread only.
	class ScriptSourceList
	{
	public:
		ScriptSourceList();
		ScriptSourceList(const ScriptSourceList&) = delete;
		ScriptSourceList& operator=(const ScriptSourceList&) = delete;

		// Lists `directory` again. Another directory than before forgets the files listed so far (and a listing of the old one
		// that is still running).
		void Refresh(const std::filesystem::path& directory);
		// Takes a finished listing; true when the files changed.
		bool Update();
		// Forgets the directory and its files (the project closed).
		void Clear();

		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		const ScriptSourceListing& GetListing() const { return *m_Listing; }
		uint64_t GetVersion() const { return m_Version; }
		bool IsListing() const { return m_Job != nullptr; }
		// Whether a listing of the directory arrived (until then the files are unknown, not absent).
		bool HasListed() const { return m_HasListed; }
	private:
		struct Job;
		void Start();
		void Forget();
	private:
		std::filesystem::path m_Directory;
		// Shared with the job that lists again (its `previous`), never changed once made.
		Ref<const ScriptSourceListing> m_Listing;
		uint64_t m_Version = 0;
		bool m_HasListed = false;
		Ref<Job> m_Job;
		bool m_RefreshPending = false;
	};

}
