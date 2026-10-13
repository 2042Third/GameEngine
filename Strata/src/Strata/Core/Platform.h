#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// How the operating system schedules a thread against the process's other threads (Platform::SetCurrentThreadPriority).
	//   Normal:      what new threads get (Windows THREAD_PRIORITY_NORMAL, Linux nice 0, macOS the default scheduling
	//                priority of the time-sharing policy).
	//   BelowNormal: ranked below Normal threads, which take the processor first when there are not enough for all: the job
	//                system's background threads, so that long jobs (an import's block compression on every core) never
	//                take it from the main thread, the workers' frame work or the graphics driver's threads (Windows
	//                THREAD_PRIORITY_BELOW_NORMAL, Linux nice 5, macOS four steps below the default scheduling priority).
	enum class ThreadPriority : uint8_t
	{
		Normal = 0,
		BelowNormal
	};

	// What a run of a program is for, as its command line says (Platform::ConnectStandardStreams).
	//   Console:     its output is the point: it prints information (--version, --help) or a usage error, or runs headless or
	//                scripted for a tool or for a person at a terminal. It prints into the terminal that started it, and the
	//                terminal's Ctrl+C asks it to quit (InstallInterruptHandler).
	//   Interactive: a person's windowed session. It leaves the terminal that started it alone: on Windows it does not attach
	//                to its console, so that the terminal's Ctrl+C or closing it do not end the program and its log does not
	//                run into the shell's prompt. A fatal error that no stream shows goes to a dialog (ShouldShowErrorDialogs).
	enum class ProgramRun : uint8_t
	{
		Console = 0,
		Interactive
	};

	// Memory of the current process in bytes, as the operating system accounts it (Platform::GetProcessMemory).
	// PrivateBytes is memory that only this process uses; WorkingSet is what is resident in physical memory, including
	// pages shared with other processes (code, mapped files). Peaks are the largest values since the process started.
	//   Windows: PrivateBytes is the commit charge (committed private memory, also pages never touched yet), WorkingSet
	//            the working set; the system tracks both peaks.
	//   Linux:   PrivateBytes is anonymous resident memory plus swapped-out memory (RssAnon + VmSwap of
	//            /proc/self/status: every private page the process has touched), WorkingSet is VmRSS and its peak VmHWM.
	//            The kernel keeps no peak of private memory: PeakPrivateBytes is the largest PrivateBytes any call in this
	//            process has seen, so a spike between two calls is missed.
	//   macOS:   PrivateBytes is the physical footprint (dirty private and compressed memory, what Activity Monitor
	//            shows), WorkingSet the resident size; the kernel tracks both peaks (kernels too old to report the
	//            footprint's peak fall back to the largest value seen, as on Linux).
	struct ProcessMemoryInfo
	{
		uint64_t PrivateBytes = 0;
		uint64_t WorkingSet = 0;
		uint64_t PeakPrivateBytes = 0;
		uint64_t PeakWorkingSet = 0;
	};

	// Operating-system services. Implemented per platform in Platform/<OS>/.
	class Platform
	{
	public:
		static std::string_view GetName(); // "Windows", "Linux" or "macOS"

		static std::filesystem::path GetExecutablePath();
		static std::filesystem::path GetExecutableDirectory();

		// Per-user writable directory for application data (logs, settings). Created if missing.
		//   Windows: %LOCALAPPDATA%/<applicationName>
		//   Linux:   $XDG_DATA_HOME/<applicationName> (default ~/.local/share/<applicationName>)
		//   macOS:   ~/Library/Application Support/<applicationName>
		// Falls back to the system temp directory when the platform reports no per-user location.
		static std::filesystem::path GetUserDataDirectory(std::string_view applicationName);
		// Like GetUserDataDirectory, but without the shared temp-directory fallback: nullopt when no per-user
		// location exists (or it cannot be created). Use it for anything secret.
		static std::optional<std::filesystem::path> FindUserDataDirectory(std::string_view applicationName);
		// The user's home directory (Windows: the profile folder, e.g. C:\Users\<name>; POSIX: $HOME), e.g. as the root of
		// suggested project locations. nullopt when there is none (an empty or relative $HOME). Not created.
		static std::optional<std::filesystem::path> FindHomeDirectory();
		// Font files of the system for text in scripts that fonts made for Latin text lack: Chinese, Japanese and Korean.
		// At most one file per group of scripts (Han characters and kana first, then Hangul where those lack it), the
		// most complete the system has; only files that exist. Empty when the usual places hold none.
		//   Windows: Microsoft YaHei, JhengHei, Yu Gothic, Meiryo or SimSun, then Malgun Gothic or Gulim
		//   Linux:   Noto Sans CJK, WenQuanYi Micro Hei or Droid Sans Fallback (all with Hangul)
		//   macOS:   PingFang, Hiragino Sans GB or STHeiti, then Apple SD Gothic Neo
		static std::vector<std::filesystem::path> FindFallbackFontFiles();

		// Private directories hold files that nobody but the current user may add, replace or rename: editor session
		// files (tokens), the copies of script modules the engine loads. EnsurePrivateDirectory, CreatePrivateDirectory and
		// GetUserRuntimeDirectory share one contract:
		//  - A directory they create is owner-only from the start. POSIX: mode 0700. Windows: a protected DACL (nothing
		//    inherited from the parent) whose single entry grants the current user full access and is inherited by what
		//    is created inside.
		//  - An existing directory qualifies only if it is a real directory (not a symbolic link or junction) that only
		//    the current user can modify.
		//      POSIX:   owned by the current user. If group or others have any permission on it, it is tightened to
		//               mode 0700 (they could plant or replace files in it, or read what it holds).
		//      Windows: owned by the current user, the Administrators or SYSTEM (an elevated run creates objects owned by
		//               the Administrators), with no access control entry that grants anyone else modify rights; its
		//               permissions are never changed. Reparse points that hold the object's own data (such as cloud
		//               placeholders) are accepted.
		//  - The directory containing it is not checked, and missing parents are created with default permissions:
		//    callers that do not control the parent check it themselves (GetUserRuntimeDirectory does).

		// Creates `directory` if it is missing and makes sure it is private (see above). False, with the reason in error,
		// if it is not and cannot be made so.
		static bool EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error = nullptr);
		// Creates a new private directory (see above) "<prefix><random characters>" in `parent`, which should be private
		// itself. Never reuses an existing directory. Returns an empty path on failure.
		static std::filesystem::path CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix);
		// Per-user private directory (see above) for files that only matter while the application runs, such as private
		// copies of libraries it loads: <location>/<name> in the first location that qualifies. Created if missing;
		// empty if no location qualifies.
		//   Windows: %LOCALAPPDATA%/<applicationName>, name "Runtime"
		//   Linux:   $XDG_RUNTIME_DIR, else $XDG_CACHE_HOME or ~/.cache; name <applicationName>
		//   macOS:   the per-user temporary directory, else ~/Library/Caches; name <applicationName>
		//   POSIX, when none of those qualifies: the temporary directory, name <applicationName>-<user id>
		// The environment variable STRATA_RUNTIME_DIR replaces the search with the location <STRATA_RUNTIME_DIR>, name
		// <applicationName> (tests use it to stay out of the user's real directory).
		// A location qualifies if it is an absolute path to a directory that only the current user can modify, by the
		// rules above, except that it may be a symbolic link or junction (a relocated cache folder, say) and is never
		// changed; a missing one is created (POSIX: mode 0700). On POSIX it must also be on a file system that allows
		// executing files, and the shared temporary directory of the last fallback qualifies if it has the sticky bit
		// (users cannot rename or remove each other's entries).
		static std::filesystem::path GetUserRuntimeDirectory(std::string_view applicationName);

		static bool IsDebuggerAttached();
		static void SetCurrentThreadName(std::string_view name);
		// Sets the calling thread's scheduling priority (ThreadPriority). Returns false if the system refused it: the thread
		// keeps its priority (Linux lets a thread lower its priority, but raising it again may need privileges).
		static bool SetCurrentThreadPriority(ThreadPriority priority);
		// The calling thread's scheduling priority: BelowNormal if it is below what new threads get, else Normal.
		static ThreadPriority GetCurrentThreadPriority();
		// Asks the system to run the current process at full speed whatever the state of its windows. Windows 11 lowers the
		// quality of service of processes whose windows are not in the foreground, covered or minimized (lower clocks, and
		// on processors with performance and efficiency cores, the efficiency cores, where an editor frame of the
		// reference machine takes 1.3 to 1.9 times as long); this opts the process out of that execution-speed power
		// throttling (SetProcessInformation, ProcessPowerThrottling). Applications call it at startup: an editor or a game
		// is interactive, and keeps playing a game, building or measuring while another window has the focus. Other
		// systems have no such per-process setting: nothing to do there, returns true. Returns false if the system
		// refused it.
		static bool DisableExecutionSpeedThrottling();
		// Whether the current process opted out of execution-speed power throttling (DisableExecutionSpeedThrottling), as
		// the system reports it; nullopt where there is no such throttling (Linux, macOS) or the system does not tell.
		static std::optional<bool> IsExecutionSpeedThrottlingDisabled();
		static uint32_t GetProcessID();
		// Whether a process with this id is running (false once it has exited, even while a handle keeps its id).
		static bool IsProcessAlive(uint32_t processId);
		// An opaque value identifying one run of a process: the same for the same process, different for a later
		// process that reuses the id. nullopt if no such process exists or it cannot be inspected.
		static std::optional<uint64_t> GetProcessStartTime(uint32_t processId);
		// Seconds since the current process was created, by the operating system's record of its creation, so the time the
		// loader and static initialization took counts too (e.g. for an application's startup time). nullopt when the
		// system does not tell, or its clock went back since.
		static std::optional<double> GetProcessUptime();

		// Fills buffer from the operating system's cryptographically secure random number generator.
		static bool GenerateSecureRandom(std::span<uint8_t> buffer);

		// Atomically replaces path with contents, readable and writable only by the current user (POSIX mode 0600,
		// a protected owner-only DACL on Windows). The data is written to a freshly created temporary file with a
		// random name (exclusive create, never following an existing file or link) that is then renamed over path;
		// the rename is retried briefly while readers hold the destination open (Windows). Parent directories are
		// created.
		static bool WritePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error = nullptr);
		// Whether a file can be trusted as written by the current user: a regular file (not a link) that only the
		// current user can modify, by the rules private directories follow (see EnsurePrivateDirectory; POSIX: owned by
		// the current user and writable by nobody else, never changed). To read such a file, use
		// ReadTrustedFile, which checks and reads through one handle.
		static bool IsTrustedFile(const std::filesystem::path& path, std::string* error = nullptr);
		// Reads a regular file to its end through a single open handle, so it cannot be swapped between the checks
		// and the read; a file larger than maxSize bytes (also one that grows while it is read) is refused. Links
		// (final component), directories, FIFOs and devices are rejected without blocking. nullopt (with the reason
		// in error) otherwise.
		static std::optional<std::string> ReadRegularFile(const std::filesystem::path& path, size_t maxSize, std::string* error = nullptr);
		// ReadRegularFile that also requires the file to be trusted (see IsTrustedFile), checked on the same handle
		// the file is read through.
		static std::optional<std::string> ReadTrustedFile(const std::filesystem::path& path, size_t maxSize, std::string* error = nullptr);
		// Renames from to to, failing (without touching either file) if to already exists.
		static bool RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to);

		// Switches stdin and stdout to binary mode (no newline translation) for machine-readable protocols such as
		// MCP over stdio. No-op on POSIX, where streams are always binary.
		static void SetBinaryStandardStreams();
		// Gives stdout and stderr of a program without a console of its own (the editor and the runtime, GUI-subsystem
		// programs on Windows, so that no console window opens with them) somewhere to go: streams its parent passed (pipes,
		// files, the null device: CTest, StrataCLI, CI) stay as they are, and for a Console run the others go to the console
		// of the process that started it, if it has one (a terminal), so what the program prints shows there. Interactive
		// runs, and runs started from the desktop, print nowhere but their log. Call first in main, before anything prints
		// or logging starts. On POSIX, where programs keep the streams they are given, it only notes the run and whether
		// anything reads stderr (ShouldShowErrorDialogs).
		static void ConnectStandardStreams(ProgramRun run);
		// Whether a fatal error should also show in a dialog: an Interactive run whose stderr reaches no reader (started from
		// the desktop or a file manager, from a terminal it leaves alone, or with its output sent to the null device; a
		// terminal, a pipe or a file are read). Console runs never show dialogs, so tools, CI and servers are never stopped
		// by one.
		static bool ShouldShowErrorDialogs();
		// Shows a modal error message and returns once the person dismissed it: MessageBoxW on Windows, osascript on macOS,
		// zenity or kdialog on Linux where one is installed. False when no dialog could be shown (the caller has logged the
		// error, which is all that is left).
		static bool ShowErrorDialog(std::string_view title, std::string_view message);

		// Turns a request to stop from outside into a request to quit that the application's main loop honors in order
		// (IsInterruptRequested): on Windows the console's Ctrl+C, Ctrl+Break and closing it (a Console run attached to a
		// terminal), on POSIX SIGINT and SIGTERM. A second Ctrl+C or signal while the first is honored ends the process at
		// once, as it would without the handler. Call once, in main.
		static void InstallInterruptHandler();
		static bool IsInterruptRequested();

		static std::optional<std::string> GetEnvVar(std::string_view name);
		static bool SetEnvVar(std::string_view name, std::string_view value);

		// Opens a file, folder or URL with the system's default handler.
		static bool OpenWithDefaultApplication(const std::string& pathOrUrl);

		// The file a program name stands for, as people type it in a command: a path with a directory as it is (on Windows
		// with ".exe" added when it has no extension, as starting it would), a bare name searched in the directories of PATH in
		// their order (on Windows with each extension of PATHEXT unless it has one; on POSIX only executable files), never in
		// the current directory. Nothing when no such file exists; outSearched gets the directories that were searched.
		static std::optional<std::filesystem::path> FindProgram(std::string_view program, std::vector<std::filesystem::path>* outSearched = nullptr);

		// Current and peak memory of this process (see ProcessMemoryInfo); nullopt if the system does not report it. (Not
		// named GetProcessMemoryInfo: <psapi.h> defines that name as a macro.)
		static std::optional<ProcessMemoryInfo> GetProcessMemory();
	};

}
