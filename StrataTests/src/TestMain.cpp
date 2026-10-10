#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Network/FakeEditorProcess.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptModule.h"
#include "TestHelpers.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(ST_PLATFORM_LINUX)
	#include <sys/prctl.h>
#endif

#if defined(ST_PLATFORM_POSIX)
	#include <cerrno>
	#include <csetjmp>
	#include <csignal>
	#include <pthread.h>
	#include <sys/types.h>
	#include <sys/wait.h>
	#include <unistd.h>

namespace
{

	sigjmp_buf s_ForeignHandlerJump;
	volatile sig_atomic_t s_ForeignHandlerCalls = 0;

	// A SIGSEGV handler some other component installed before the crash guard; it recovers by jumping back.
	void ForeignSegfaultHandler(int)
	{
		s_ForeignHandlerCalls = s_ForeignHandlerCalls + 1;
		siglongjmp(s_ForeignHandlerJump, 1);
	}

	void WriteToNull(void*)
	{
		volatile int* pointer = nullptr;
		*pointer = 42;
	}

	// Faults outside any guarded call; the foreign handler jumps back here.
	void FaultOutsideGuard()
	{
		if (sigsetjmp(s_ForeignHandlerJump, 1) == 0)
			WriteToNull(nullptr);
	}

	volatile sig_atomic_t s_RecordedSignalCode = 0;
	volatile sig_atomic_t s_RecordedSignalSender = 0;
	volatile sig_atomic_t s_SignalRecorded = 0;

	void RecordSignal(int, siginfo_t* info, void*)
	{
		s_RecordedSignalCode = info ? info->si_code : 0;
		s_RecordedSignalSender = info ? info->si_pid : 0;
		s_SignalRecorded = 1;
	}

	// Prints what this platform reports for a SIGFPE another process sends (si_code, si_pid), which the crash guard uses
	// to tell it from a fault: a test that fails shows it in its output. Leaves the signal's handler as it was.
	void ReportSentSignalInformation()
	{
		struct sigaction record = {};
		record.sa_sigaction = RecordSignal;
		record.sa_flags = SA_SIGINFO;
		sigemptyset(&record.sa_mask);
		struct sigaction previous = {};
		if (sigaction(SIGFPE, &record, &previous) != 0)
		{
			std::printf("SIGFPE from another process: cannot install a handler to record it\n");
			std::fflush(stdout);
			return;
		}

		const pid_t sender = fork();
		if (sender == 0)
		{
			kill(getppid(), SIGFPE);
			_exit(0);
		}
		// The signal interrupts the wait (the handler is installed without SA_RESTART).
		while (sender > 0 && waitpid(sender, nullptr, 0) < 0 && errno == EINTR)
		{
		}
		for (int attempt = 0; attempt < 500 && sender > 0 && !s_SignalRecorded; attempt++)
			usleep(10000);
		sigaction(SIGFPE, &previous, nullptr);
		if (sender > 0 && s_SignalRecorded)
		{
			std::printf("SIGFPE from another process: si_code %d, si_pid %d (sender %d)\n", static_cast<int>(s_RecordedSignalCode),
				static_cast<int>(s_RecordedSignalSender), static_cast<int>(sender));
		}
		else
		{
			std::printf("SIGFPE from another process: not recorded\n");
		}
		std::fflush(stdout);
	}

}
#endif
// Helper processes that could outlive their test stop when it says so: once the stop file exists or its directory is gone
// (temporary directories are removed when the tests end), and after a minute at the latest.
static bool KeepHelperRunning(const std::filesystem::path& stop, std::chrono::steady_clock::time_point deadline)
{
	return !Strata::FileSystem::Exists(stop) && Strata::FileSystem::IsDirectory(stop.parent_path()) && std::chrono::steady_clock::now() < deadline;
}

// When launched with --strata-test-helper=<mode>, the test executable acts as a child process for the
// Process tests (and checks build products for CTest scripts) instead of running the test suites. This keeps
// those tests free of external programs.
// What a process running a script module from a private copy relies on: the copy in its copy directory, and the owner
// lock it holds there. Empty if all is well, else what is wrong.
static std::string CheckOwnModuleCopy(const Strata::ScriptEngine& engine, const std::filesystem::path& directory)
{
	const Strata::ScriptModule* module = engine.GetModule();
	if (!module || !module->IsLoadedFromCopy())
		return "the module does not run from a copy";
	if (module->GetLoadedPath().parent_path() != directory)
		return "the module moved to another copy directory";
	if (!Strata::FileSystem::Exists(module->GetLoadedPath()))
		return "the copy is gone";
	const std::filesystem::path ownerLock = directory / "Owner.lock";
	if (!Strata::FileSystem::Exists(ownerLock))
		return "the owner lock file is gone";
	if (Strata::FileLock::TryAcquire(ownerLock))
		return "the owner lock is not held";
	return {};
}

static int RunHelperMode(std::string_view mode, int argc, char** argv)
{
	if (mode == "echo")
	{
		for (int index = 2; index < argc; index++)
			std::printf("%s%s", index > 2 ? " " : "", argv[index]);
		std::printf("\n");
		std::fflush(stdout);
		return 0;
	}
	if (mode == "stderr")
	{
		std::fprintf(stderr, "error-output\n");
		std::fflush(stderr);
		return 0;
	}
	if (mode == "split-output")
	{
		std::fprintf(stdout, "to-stdout\n");
		std::fflush(stdout);
		std::fprintf(stderr, "to-stderr\n");
		std::fflush(stderr);
		return 0;
	}
	if (mode == "cat")
	{
		// Copies stdin (text without NUL characters) to stdout line by line as it arrives, until the end of the input.
		Strata::Platform::SetBinaryStandardStreams();
		char buffer[4096];
		while (std::fgets(buffer, sizeof(buffer), stdin))
		{
			if (std::fputs(buffer, stdout) < 0)
				return 1;
			std::fflush(stdout);
		}
		return std::ferror(stdin) ? 1 : 0;
	}
	if (mode == "exit-code")
		return argc > 2 ? std::atoi(argv[2]) : 0;
	if (mode == "sleep")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(argc > 2 ? std::atoi(argv[2]) : 10000));
		return 0;
	}
	if (mode == "heartbeat" && argc > 3)
	{
		// <file> <stop file>: appends a byte to the file every 10 ms (see KeepHelperRunning).
		const std::filesystem::path beats = Strata::FileSystem::FromUTF8(argv[2]);
		const std::filesystem::path stop = Strata::FileSystem::FromUTF8(argv[3]);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (KeepHelperRunning(stop, deadline))
		{
			{
				std::ofstream file(beats, std::ios::binary | std::ios::app);
				file.put('.');
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return 0;
	}
	if (mode == "spawn-heartbeat" && argc > 3)
	{
		// <file> <stop file>: starts a "heartbeat" child (a grandchild of the test) and waits (see KeepHelperRunning).
		Strata::ProcessSpecification specification;
		specification.Executable = Strata::Platform::GetExecutablePath();
		specification.Arguments = { "--strata-test-helper=heartbeat", argv[2], argv[3] };
		specification.Output = Strata::ProcessOutputMode::Discard;
		Strata::Process heartbeat;
		if (!heartbeat.Start(specification))
			return 1;
		const std::filesystem::path stop = Strata::FileSystem::FromUTF8(argv[3]);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (KeepHelperRunning(stop, deadline))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return 0;
	}
	if (mode == "cwd")
	{
		const std::u8string currentDirectory = std::filesystem::current_path().generic_u8string();
		std::printf("%s\n", reinterpret_cast<const char*>(currentDirectory.c_str()));
		std::fflush(stdout);
		return 0;
	}
	if (mode == "guarded-abort")
	{
		// Calls abort() inside a crash guard. Printing "contained" means the guard swallowed it.
		Strata::CrashGuard::Invoke([](void*) { std::abort(); }, nullptr);
		std::printf("contained\n");
		std::fflush(stdout);
		return 0;
	}
#if defined(ST_PLATFORM_POSIX)
	if (mode == "signal-chaining")
	{
		// A handler installed before the guard gets every fault outside guarded calls, and guarded calls stay contained
		// in between (the guard keeps its own handler). Exit code 0 if both happened twice.
		struct sigaction action = {};
		action.sa_handler = ForeignSegfaultHandler;
		sigemptyset(&action.sa_mask);
		sigaction(SIGSEGV, &action, nullptr);
		Strata::CrashGuard::Invoke([](void*) {}, nullptr); // Installs the guard's handlers
		int contained = 0;
		for (int round = 0; round < 2; round++)
		{
			FaultOutsideGuard();
			if (!Strata::CrashGuard::Invoke(WriteToNull, nullptr))
				contained++;
		}
		std::printf("foreign handler calls: %d, contained: %d\n", static_cast<int>(s_ForeignHandlerCalls), contained);
		std::fflush(stdout);
		return s_ForeignHandlerCalls == 2 && contained == 2 ? 0 : 1;
	}
	if (mode == "external-signal")
	{
		// A guarded call during which another process (a child) sends SIGFPE: not a fault of the call, so the signal takes
		// its default action and ends this process instead of being contained.
		ReportSentSignalInformation();
		const pid_t parent = getpid();
		const pid_t child = fork();
		if (child < 0)
			return 2;
		if (child == 0)
		{
			usleep(300000);
			kill(parent, SIGFPE);
			_exit(0);
		}
		const bool completed = Strata::CrashGuard::Invoke([](void*)
		{
			for (int index = 0; index < 100; index++)
				usleep(100000);
		}, nullptr);
		std::printf("%s\n", completed ? "completed" : "contained");
		std::fflush(stdout);
		return 0;
	}
	if (mode == "guarded-thread-exit")
	{
		// pthread_exit inside a guarded call on another thread: the thread must end normally (exit code 0).
		pthread_t thread;
		auto run = [](void*) -> void*
		{
			Strata::CrashGuard::Invoke([](void*) { pthread_exit(nullptr); }, nullptr);
			return reinterpret_cast<void*>(1);
		};
		if (pthread_create(&thread, nullptr, run, nullptr) != 0)
			return 2;
		void* result = reinterpret_cast<void*>(1);
		pthread_join(thread, &result);
		std::printf("%s\n", result == nullptr ? "exited" : "returned");
		std::fflush(stdout);
		return result == nullptr ? 0 : 1;
	}
#endif
	if (mode == "play-faulty-script")
	{
		// <faults module> <fault>: plays a scene whose Faulty script crashes with <fault> in OnUpdate. Prints "contained"
		// (and returns 0) if the engine faulted the module and kept running.
		if (argc < 4)
			return 2;
		Strata::LogSpecification logSpecification;
		logSpecification.Level = Strata::LogLevel::Warn;
		Strata::Log::Init(logSpecification);
		const Strata::Ref<Strata::ScriptEngine> engine = Strata::CreateRef<Strata::ScriptEngine>();
		std::string error;
		if (!engine->LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
		{
			std::printf("%s\n", error.c_str());
			return 1;
		}
		Strata::ScriptEngine::SetActive(engine);
		bool contained = false;
		{
			Strata::Scene scene;
			Strata::Entity entity = scene.CreateEntity("Faulty");
			Strata::ScriptEntry& entry = entity.AddComponent<Strata::ScriptComponent>().Scripts.emplace_back();
			entry.ClassName = "Faulty";
			entry.Fields.push_back(Strata::ScriptFieldValue { "Fault", Strata::PropertyType::String, std::string(argv[3]) });
			entry.Fields.push_back(Strata::ScriptFieldValue { "FaultIn", Strata::PropertyType::String, std::string("OnUpdate") });
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(0.0f);
			contained = engine->IsFaulted();
			scene.OnRuntimeStop();
		}
		Strata::ScriptEngine::SetActive(nullptr);
		std::printf("%s\n", contained ? "contained" : "not faulted");
		std::fflush(stdout);
		Strata::Log::Shutdown();
		return contained ? 0 : 1;
	}
	if (mode == "hold-file-lock")
	{
		// <path> [create]: locks the existing file (or creates it locked), reports "locked" and holds the lock until the
		// process is ended (at most a minute).
		if (argc < 3)
			return 2;
		const std::filesystem::path path = Strata::FileSystem::FromUTF8(argv[2]);
		const bool create = argc > 3 && std::string_view(argv[3]) == "create";
		const Strata::Scope<Strata::FileLock> lock = create ? Strata::FileLock::Create(path) : Strata::FileLock::TryAcquire(path);
		if (!lock)
			return 1;
		std::printf("locked\n");
		std::fflush(stdout);
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 0;
	}
	if (mode == "script-module-reloads")
	{
		// <module path> <rounds>: every round loads the module from a private copy (as with hot reload), reloads it and
		// unloads it again, so each round creates a new copy directory - and removes the stale ones of other processes.
		// After every load the process checks that its copy, the copy directory and the owner lock it holds there are
		// intact: other processes doing the same at the same time must never remove them.
		if (argc < 4)
			return 2;
		const int rounds = std::atoi(argv[3]);
		for (int round = 0; round < rounds; round++)
		{
			Strata::ScriptEngine engine;
			engine.SetHotReloadEnabled(true);
			std::string error;
			if (!engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
			{
				std::fprintf(stderr, "Round %d: the load failed: %s\n", round, error.c_str());
				return 1;
			}
			const std::filesystem::path directory = engine.GetModule()->GetLoadedPath().parent_path();
			for (int reload = 0; reload <= 2; reload++)
			{
				if (reload > 0 && !engine.Reload(&error))
				{
					std::fprintf(stderr, "Round %d: reload %d failed: %s\n", round, reload, error.c_str());
					return 1;
				}
				const std::string problem = CheckOwnModuleCopy(engine, directory);
				if (!problem.empty() || engine.GetClasses().empty())
				{
					std::fprintf(stderr, "Round %d, reload %d: %s\n", round, reload, problem.empty() ? "the module has no classes" : problem.c_str());
					return 1;
				}
			}
		}
		return 0;
	}
	if (mode == "script-module-lifecycle")
	{
		// <module> <healthy module>: loads and unloads the first module (whatever happens), loads its file once more the
		// way shipped games do (in place unless that is unsafe) and reports how, then shows that the engine still works by
		// loading the second. Engine messages go to the output too. The process ends without exit handlers: a library
		// abandoned after a crash in its static destructors must not run them again.
		if (argc < 4)
			return 2;
		Strata::LogSpecification logSpecification;
		logSpecification.Level = Strata::LogLevel::Warn;
		Strata::Log::Init(logSpecification);
		int result = 0;
		{
			Strata::ScriptEngine engine;
			std::string error;
			const bool loaded = engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error);
			std::printf("first module: %s\n", loaded ? "loaded" : error.c_str());
			std::fflush(stdout);
			engine.UnloadModule();
			std::printf("first module: unloaded\n");
			std::fflush(stdout);
			{
				Strata::ScriptModuleLoadError againError;
				const Strata::Scope<Strata::ScriptModule> again = Strata::ScriptModule::Load(Strata::FileSystem::FromUTF8(argv[2]),
					Strata::ScriptModuleLoadMode::InPlace, nullptr, &againError);
				std::printf("first module again: %s\n", again ? (again->IsLoadedFromCopy() ? "loaded from a copy" : "loaded in place") : againError.Message.c_str());
				std::fflush(stdout);
			}
			if (engine.LoadModule(Strata::FileSystem::FromUTF8(argv[3]), &error) && !engine.GetClasses().empty())
			{
				std::printf("second module: loaded\n");
			}
			else
			{
				std::printf("second module: %s\n", error.c_str());
				result = 1;
			}
		}
		Strata::Log::Shutdown();
		std::fflush(stdout);
		std::fflush(stderr);
		std::_Exit(result);
	}
	if (mode == "active-engine-at-exit")
	{
		// <module path>: leaves an engine with a loaded module active, so it is destroyed - unloading the module - while
		// the program's static objects are destroyed. Exit code 0 if that goes well.
		if (argc < 3)
			return 2;
		const Strata::Ref<Strata::ScriptEngine> engine = Strata::CreateRef<Strata::ScriptEngine>();
		engine->SetHotReloadEnabled(true); // From a copy, which is removed on unload as well
		if (!engine->LoadModule(Strata::FileSystem::FromUTF8(argv[2])))
			return 1;
		Strata::ScriptEngine::SetActive(engine);
		return 0;
	}
	if (mode == "runtime-directory")
	{
		// <application> <XDG_RUNTIME_DIR> <XDG_CACHE_HOME> <TMPDIR>: prints the runtime directory found with that
		// environment (and no STRATA_RUNTIME_DIR).
		if (argc < 6)
			return 2;
		Strata::Platform::SetEnvVar("STRATA_RUNTIME_DIR", "");
		Strata::Platform::SetEnvVar("XDG_RUNTIME_DIR", argv[3]);
		Strata::Platform::SetEnvVar("XDG_CACHE_HOME", argv[4]);
		Strata::Platform::SetEnvVar("TMPDIR", argv[5]);
		std::printf("[%s]\n", Strata::FileSystem::ToUTF8(Strata::Platform::GetUserRuntimeDirectory(argv[2])).c_str());
		std::fflush(stdout);
		return 0;
	}
	if (mode == "load-script-module")
	{
		// <module path> <class name>...: succeeds if the module loads and contains every class.
		if (argc < 3)
			return 2;
		Strata::ScriptEngine engine;
		std::string error;
		if (!engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		for (int index = 3; index < argc; index++)
		{
			if (!engine.FindClass(argv[index]))
			{
				std::fprintf(stderr, "The module has no script class '%s'\n", argv[index]);
				return 1;
			}
		}
		return 0;
	}
	if (mode == "check-image")
	{
		// <png> [--dominant red|green|blue <percent>]: succeeds if the image decodes and shows something: it is neither
		// (nearly) black nor a single color, and with --dominant at least <percent> of its pixels are clearly of that color
		// (the channel exceeds both others by more than 40), e.g. a known object of the scene.
		if (argc != 3 && argc != 6)
			return 2;
		int dominantChannel = -1;
		double dominantPercent = 0.0;
		if (argc == 6)
		{
			const std::string_view option(argv[3]);
			const std::string_view color(argv[4]);
			dominantChannel = color == "red" ? 0 : (color == "green" ? 1 : (color == "blue" ? 2 : -1));
			dominantPercent = std::atof(argv[5]);
			if (option != "--dominant" || dominantChannel < 0 || !(dominantPercent > 0.0 && dominantPercent <= 100.0))
				return 2;
		}
		const std::optional<std::vector<uint8_t>> data = Strata::FileSystem::ReadBytes(Strata::FileSystem::FromUTF8(argv[2]));
		if (!data || data->empty())
		{
			std::fprintf(stderr, "Cannot read '%s'\n", argv[2]);
			return 1;
		}
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(data->data(), static_cast<int>(data->size()), &width, &height, &channels, 4);
		if (!pixels || width <= 0 || height <= 0)
		{
			std::fprintf(stderr, "'%s' is not a valid image\n", argv[2]);
			stbi_image_free(pixels);
			return 1;
		}
		int brightest = 0;
		int64_t differing = 0; // Pixels that differ from the top-left one
		int64_t dominant = 0;  // Pixels clearly of the --dominant color
		const int64_t count = static_cast<int64_t>(width) * height;
		for (int64_t index = 0; index < count; index++)
		{
			const stbi_uc* pixel = pixels + index * 4;
			brightest = std::max({ brightest, static_cast<int>(pixel[0]), static_cast<int>(pixel[1]), static_cast<int>(pixel[2]) });
			if (dominantChannel >= 0)
			{
				const int value = pixel[dominantChannel];
				const int other1 = pixel[(dominantChannel + 1) % 3];
				const int other2 = pixel[(dominantChannel + 2) % 3];
				dominant += value > other1 + 40 && value > other2 + 40 ? 1 : 0;
			}
			for (int channel = 0; channel < 3; channel++)
			{
				if (std::abs(static_cast<int>(pixel[channel]) - static_cast<int>(pixels[channel])) > 24)
				{
					differing++;
					break;
				}
			}
		}
		stbi_image_free(pixels);
		std::printf("%dx%d pixels, brightest channel %d, %lld pixels differ from the corner\n", width, height, brightest, static_cast<long long>(differing));
		if (brightest < 32)
		{
			std::fprintf(stderr, "The image is black\n");
			return 1;
		}
		if (differing < count / 100)
		{
			std::fprintf(stderr, "The image is (nearly) a single color\n");
			return 1;
		}
		if (dominantChannel >= 0)
		{
			const double percent = 100.0 * static_cast<double>(dominant) / static_cast<double>(count);
			std::printf("%.2f%% of the pixels are %s\n", percent, argv[4]);
			if (percent < dominantPercent)
			{
				std::fprintf(stderr, "Fewer than %.2f%% of the pixels are %s\n", dominantPercent, argv[4]);
				return 1;
			}
		}
		return 0;
	}

#if defined(ST_PLATFORM_WINDOWS)
	if (mode == "check-icon")
	{
		// <executable>: succeeds if its icon (the first RT_GROUP_ICON) holds the strata mark's 16, 32, 48 and 256 pixel
		// images (StrataEditor/Resources/Brand/StrataMark.ico).
		if (argc != 3)
			return 2;
		std::string error;
		const std::optional<std::vector<uint32_t>> sizes = Strata::Tests::ReadExecutableIconSizes(Strata::FileSystem::FromUTF8(argv[2]), &error);
		if (!sizes)
		{
			std::fprintf(stderr, "'%s': %s\n", argv[2], error.c_str());
			return 1;
		}
		std::printf("'%s' has an icon with %zu images:", argv[2], sizes->size());
		for (uint32_t size : *sizes)
			std::printf(" %u", size);
		std::printf("\n");
		for (uint32_t expected : { 16u, 32u, 48u, 256u })
		{
			if (std::find(sizes->begin(), sizes->end(), expected) == sizes->end())
			{
				std::fprintf(stderr, "The icon has no %u pixel image\n", expected);
				return 1;
			}
		}
		return 0;
	}
#endif

#if defined(ST_PLATFORM_LINUX)
	if (mode == "rename-when-file-exists" && argc > 2)
	{
		// Once the parent creates the file, takes a name containing ')' and a newline (which /proc/<pid>/stat shows
		// unescaped inside its parentheses), then runs until it is terminated.
		const std::filesystem::path trigger(argv[2]);
		std::error_code error;
		for (int attempt = 0; attempt < 1000 && !std::filesystem::exists(trigger, error); attempt++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// prctl reads its arguments as unsigned long, which is uintptr_t on Linux.
		if (prctl(PR_SET_NAME, reinterpret_cast<uintptr_t>("a)\nb"), 0UL, 0UL, 0UL) != 0)
			return 1;
		std::this_thread::sleep_for(std::chrono::seconds(30));
		return 0;
	}
#endif
	return 99;
}

// With STRATA_TEST_FAKE_CMAKE=succeed, the test executable run with CMake's arguments ("-S ..." to configure,
// "--build ..." to build) stands in for CMake in script builds (ScriptBuildSettings::CMake): it succeeds without building
// anything, so the module file stays as the test left it.
static std::optional<int> RunAsFakeCMake(int argc, char** argv)
{
	const std::optional<std::string> mode = Strata::Platform::GetEnvVar("STRATA_TEST_FAKE_CMAKE");
	if (!mode || argc < 2 || (std::string_view(argv[1]) != "-S" && std::string_view(argv[1]) != "--build"))
		return std::nullopt;
	if (*mode != "succeed")
		return std::nullopt;
	std::printf("-- Fake CMake: %s\n", argv[1]);
	std::fflush(stdout);
	return 0;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);
	if (const std::optional<int> fakeCMake = RunAsFakeCMake(argc, argv))
		return *fakeCMake;

	// Launched as an editor by the CLI launch tests (see Network/FakeEditorProcess.h).
	if (Strata::Tests::IsFakeEditorLaunch(argc, argv))
		return Strata::Tests::RunFakeEditor(argc, argv);

	Strata::LogSpecification logSpecification;
	logSpecification.Level = Strata::LogLevel::Warn;
	Strata::Log::Init(logSpecification);

	// The tests (and the helper processes they start, which inherit the environment) keep their runtime files - script
	// module copies, private directories - in a private directory of their own, never in the user's.
	const std::filesystem::path runtimeDirectory = Strata::Tests::CreateTemporaryDirectory("Runtime");
	std::error_code permissionError;
	std::filesystem::permissions(runtimeDirectory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, permissionError);
	Strata::Platform::SetEnvVar("STRATA_RUNTIME_DIR", Strata::FileSystem::ToUTF8(runtimeDirectory));
	// Editors the tests start keep their recent projects in the test's directory, never in the user's list.
	Strata::Platform::SetEnvVar("STRATA_RECENT_PROJECTS", Strata::FileSystem::ToUTF8(runtimeDirectory / "RecentProjects.json"));

	doctest::Context context(argc, argv);
	const int result = context.run();

	Strata::Tests::GPUContext::ShutdownShared();
	Strata::Log::Shutdown();
	Strata::Tests::CleanupTemporaryDirectories();
	return result;
}
