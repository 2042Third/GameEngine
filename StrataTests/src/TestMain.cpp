#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Editor/SceneComparison.h"
#include "Engine/ModuleRegistrationHelpers.h"
#include "Network/FakeEditorProcess.h"
#include "Perf/StressProject.h"
#include "Reflection/ComponentSchema.h"
#include "Renderer/GPUTestUtils.h"
#include "Runtime/PerfLogReader.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Core/BackgroundLogSink.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "Strata/Engine/BuiltinModules.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptModule.h"
#include "Strata/Scripting/ScriptingComponents.h"
#include "TestHelpers.h"

#if ST_WITH_PHYSICS
	#include "Physics/PhysicsTestUtils.h"
#endif

#include <stb_image.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
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

// Logs from its destructor, as a static object of a program does that logs after main called Log::Shutdown: constructed
// before main, it is destroyed after the log's exit handler, which main registers, has run. Armed by the "log-after-exit"
// helper mode; it prints how its line was written.
class LateLogProbe
{
public:
	void Arm(std::filesystem::path logFile)
	{
		m_LogFile = std::move(logFile);
	}

	~LateLogProbe()
	{
		if (m_LogFile.empty())
			return;
		Strata::LogSpecification specification;
		specification.ConsoleOutput = false;
		specification.LogFile = m_LogFile;
		Strata::Log::Init(specification);
		ST_CORE_INFO("Logged after exit");
		const std::vector<spdlog::sink_ptr>& sinks = Strata::Log::GetCoreLogger()->sinks();
		const bool background = std::any_of(sinks.begin(), sinks.end(), [](const spdlog::sink_ptr& sink)
		{
			return std::dynamic_pointer_cast<Strata::BackgroundLogSink>(sink) != nullptr;
		});
		Strata::Log::Flush();
		std::printf("late log: %s\n", background ? "background writer" : "written by the thread that logs");
		std::fflush(stdout);
	}
private:
	std::filesystem::path m_LogFile;
};

static LateLogProbe s_LateLogProbe;

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
	if (mode == "write-component-schema" && argc > 2)
	{
		// Writes the registry's component metadata (Tests::DescribeComponentSchema) to the file named: how
		// StrataTests/Architecture/ComponentSchema.json was made.
		const std::string text = Strata::JsonUtils::Dump(Strata::Tests::DescribeComponentSchema(), 1, '\t') + "\n";
		return Strata::FileSystem::WriteText(Strata::FileSystem::FromUTF8(argv[2]), text) ? 0 : 1;
	}
	if (mode == "record-arguments" && argc > 2)
	{
		// Stands in for a program the editor starts (an external code editor): writes its other arguments, one per line, to
		// the file named first, whole (written beside it, then renamed), so a test can wait for the file and read it.
		const std::filesystem::path file = Strata::FileSystem::FromUTF8(argv[2]);
		std::string text;
		for (int index = 3; index < argc; index++)
			text += std::string(argv[index]) + "\n";
		const std::filesystem::path partial = file.parent_path() / (file.filename().native() + Strata::FileSystem::FromUTF8(".partial").native());
		if (!Strata::FileSystem::WriteText(partial, text))
			return 1;
		std::error_code error;
		std::filesystem::rename(partial, file, error);
		return error ? 1 : 0;
	}
#if defined(ST_PLATFORM_WINDOWS)
	if (mode == "console-output" && argc > 2)
	{
		// [--interrupt-when <text>] <program> [arguments]: runs the program as a terminal does, with this helper's console
		// (hidden: Process starts the helper without a window) as its output, pressing Ctrl+C once the console shows the text;
		// prints what the console shows and ends with the program's exit code.
		int first = 2;
		std::string interruptWhen;
		if (std::string_view(argv[2]) == "--interrupt-when" && argc > 4)
		{
			interruptWhen = argv[3];
			first = 4;
		}
		std::vector<std::string> arguments(argv + first + 1, argv + argc);
		std::string error;
		const std::optional<Strata::Tests::ConsoleRun> run = Strata::Tests::RunInConsole(Strata::FileSystem::FromUTF8(argv[first]), arguments, &error,
			interruptWhen);
		if (!run)
		{
			std::fprintf(stderr, "console-output: %s\n", error.c_str());
			return 99;
		}
		std::fputs(run->Text.c_str(), stdout);
		std::fflush(stdout);
		return run->ExitCode;
	}
#endif
	if (mode == "run-with-environment")
	{
		// <NAME=VALUE>... -- <program> [arguments]: runs the program with these environment variables (and this helper's
		// standard streams) and ends with its exit code, so tests change a child's environment without changing their own.
		int index = 2;
		for (; index < argc && std::string_view(argv[index]) != "--"; index++)
		{
			const std::string_view assignment = argv[index];
			const size_t equals = assignment.find('=');
			if (equals == std::string_view::npos || equals == 0)
			{
				std::fprintf(stderr, "run-with-environment: '%s' is not NAME=VALUE\n", argv[index]);
				return 99;
			}
			Strata::Platform::SetEnvVar(assignment.substr(0, equals), assignment.substr(equals + 1));
		}
		if (index + 1 >= argc)
		{
			std::fprintf(stderr, "run-with-environment: no program after --\n");
			return 99;
		}
		Strata::ProcessSpecification specification;
		specification.Executable = Strata::FileSystem::FromUTF8(argv[index + 1]);
		specification.Arguments.assign(argv + index + 2, argv + argc);
		specification.Output = Strata::ProcessOutputMode::Inherit;
		Strata::Process process;
		if (!process.Start(specification))
		{
			std::fprintf(stderr, "run-with-environment: %s\n", process.GetLastError().c_str());
			return 99;
		}
		return process.Wait().value_or(99);
	}
	if (mode == "create-graphics-device")
	{
		// [--validation]: creates a headless graphics device as the GPU tests' does, with or without validation, and says
		// whether it could and with how many errors.
		Strata::GraphicsDeviceSpecification specification;
		specification.ApplicationName = "StrataTests.Helper";
		specification.Headless = true;
		specification.EnableValidation = argc > 2 && std::string_view(argv[2]) == "--validation";
		const Strata::Scope<Strata::GraphicsDevice> device = Strata::GraphicsDevice::Create(specification);
		if (device)
			std::printf("Graphics device created (errors: %u)\n", device->GetErrorCount());
		else
			std::printf("No graphics device\n");
		std::fflush(stdout);
		return device ? 0 : 1;
	}
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
	if (mode == "log-after-exit" && argc > 2)
	{
		// <directory>: logs into <directory>/Main.log and shuts the log down as programs do at the end of main; then a static
		// object logs into <directory>/Late.log after the log's exit handler ran (LateLogProbe).
		const std::filesystem::path directory = Strata::FileSystem::FromUTF8(argv[2]);
		Strata::LogSpecification specification;
		specification.ConsoleOutput = false;
		specification.LogFile = directory / "Main.log";
		Strata::Log::Init(specification);
		ST_CORE_INFO("Logged in main");
		Strata::Log::Shutdown();
		s_LateLogProbe.Arm(directory / "Late.log");
		return 0;
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
#if ST_WITH_PHYSICS
	if (mode == "broad-phase-churn")
		return Strata::Tests::RunBroadPhaseChurnProcess();
#endif
	if (mode == "streaming-sweep")
	{
		// <settings file> <result file>: runs a camera sweep over a stress project's pack (Perf/StressProject.h) and writes
		// what it measured, peak memory included, for the streaming perf test. A measurement runs at full speed, like the
		// applications it stands for.
		Strata::Platform::DisableExecutionSpeedThrottling();
		return Strata::Tests::RunStreamingSweepProcess(argc, argv);
	}
	if (mode == "execution-speed-throttling")
	{
		// Prints whether the process is exempt from execution-speed throttling as it starts, the result of asking for that,
		// and the state afterwards ("unknown" where the system has no such setting).
		const auto describe = [](std::optional<bool> disabled) { return disabled ? (*disabled ? "disabled" : "default") : "unknown"; };
		const char* before = describe(Strata::Platform::IsExecutionSpeedThrottlingDisabled());
		const bool requested = Strata::Platform::DisableExecutionSpeedThrottling();
		std::printf("before=%s requested=%d after=%s\n", before, requested ? 1 : 0, describe(Strata::Platform::IsExecutionSpeedThrottlingDisabled()));
		std::fflush(stdout);
		return 0;
	}
	if (mode == "check-image")
	{
		// <png> [--dominant red|green|blue <percent>] [--size <width>x<height>]: succeeds if the image decodes and shows
		// something: it is neither (nearly) black nor a single color, with --dominant at least <percent> of its pixels are
		// clearly of that color (the channel exceeds both others by more than 40), e.g. a known object of the scene, and with
		// --size it has exactly that many pixels.
		if (argc < 3)
			return 2;
		int dominantChannel = -1;
		double dominantPercent = 0.0;
		const char* dominantName = "";
		std::optional<std::pair<uint32_t, uint32_t>> expectedSize;
		for (int argument = 3; argument < argc; argument++)
		{
			const std::string_view option(argv[argument]);
			if (option == "--dominant" && argument + 2 < argc)
			{
				dominantName = argv[argument + 1];
				const std::string_view color(dominantName);
				dominantChannel = color == "red" ? 0 : (color == "green" ? 1 : (color == "blue" ? 2 : -1));
				dominantPercent = std::atof(argv[argument + 2]);
				if (dominantChannel < 0 || !(dominantPercent > 0.0 && dominantPercent <= 100.0))
					return 2;
				argument += 2;
			}
			else if (option == "--size" && argument + 1 < argc)
			{
				expectedSize = Strata::CommandLine(std::vector<std::string> { "check-image", "--size", argv[argument + 1] }).GetSizeOption("--size");
				if (!expectedSize)
					return 2;
				argument++;
			}
			else
			{
				return 2;
			}
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
		if (expectedSize && (static_cast<uint32_t>(width) != expectedSize->first || static_cast<uint32_t>(height) != expectedSize->second))
		{
			std::fprintf(stderr, "The image is not %ux%u pixels\n", expectedSize->first, expectedSize->second);
			return 1;
		}
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
			std::printf("%.2f%% of the pixels are %s\n", percent, dominantName);
			if (percent < dominantPercent)
			{
				std::fprintf(stderr, "Fewer than %.2f%% of the pixels are %s\n", dominantPercent, dominantName);
				return 1;
			}
		}
		return 0;
	}

	if (mode == "write-stress-project")
	{
		// <directory>: writes the streaming stress project (Perf/StressProject.h: 111.8 MB of block-compressed textures, the
		// World, TopDown and Flight scenes) into a new directory, to look at it in the editor (StrataEditor --project
		// <directory>), e.g. with the Streaming panel.
		if (argc != 3)
			return 2;
		std::string error;
		const std::filesystem::path directory = Strata::FileSystem::FromUTF8(argv[2]);
		const Strata::Tests::StressProjectSpec spec;
		std::optional<Strata::Tests::StressProject> project = Strata::Tests::WriteStressProject(directory, spec, error);
		if (!project || !Strata::Tests::WriteStressTopDownScene(*project, spec, error) || !Strata::Tests::WriteStressFlightScene(*project, spec, error))
		{
			std::fprintf(stderr, "Cannot write the stress project: %s\n", error.c_str());
			return 1;
		}
		std::printf("Wrote the stress project to '%s'\n", argv[2]);
		return 0;
	}
	if (mode == "write-stress-game")
	{
		// <directory>: writes the streaming stress project into <directory>/Stress, imports it and builds its pack with the
		// manifests PerfGPU.Streaming runs (Stress.stgame from the world's overview camera, StressTopDown.stgame from above;
		// StressFlight.stgame starts the flight scene, whose terrain is textured), to run the exported stress game by hand:
		// StrataRuntime --game <directory>/StressTopDown.stgame --perf-log <csv>.
		if (argc != 3)
			return 2;
		Strata::LogSpecification logSpecification;
		logSpecification.Level = Strata::LogLevel::Warn;
		Strata::Log::Init(logSpecification);
		Strata::JobSystem::Init();
		std::string error;
		const std::filesystem::path directory = Strata::FileSystem::FromUTF8(argv[2]);
		const Strata::Tests::StressProjectSpec spec;
		std::optional<Strata::Tests::StressProject> project = Strata::Tests::WriteStressProject(directory / "Stress", spec, error);
		bool written = project && Strata::Tests::WriteStressTopDownScene(*project, spec, error) && Strata::Tests::WriteStressFlightScene(*project, spec, error)
			&& Strata::Tests::BuildStressPack(*project, directory / "Stress.stpak", error);
		for (const auto& [scene, file] : { std::pair { project ? project->WorldScene : Strata::UUID::Null(), "Stress.stgame" },
			std::pair { project ? project->TopDownScene : Strata::UUID::Null(), "StressTopDown.stgame" },
			std::pair { project ? project->FlightScene : Strata::UUID::Null(), "StressFlight.stgame" } })
		{
			if (!written)
				break;
			Strata::GameManifest manifest;
			manifest.Name = "StrataStressWorld";
			manifest.AssetPack = "Stress.stpak";
			manifest.StartScene = scene;
			written = manifest.Save(directory / file, &error);
		}
		Strata::JobSystem::Shutdown();
		Strata::Log::Shutdown();
		if (!written)
		{
			std::fprintf(stderr, "Cannot write the stress game: %s\n", error.c_str());
			return 1;
		}
		std::printf("Wrote the stress game to '%s'\n", argv[2]);
		return 0;
	}
	if (mode == "check-perf-log")
	{
		// <csv> <rows> [--nonzero <column>...]: succeeds if the file is a perf log of StrataRuntime --perf-log with exactly
		// <rows> rows of consecutive frames and every built-in stage among its columns (Runtime/PerfLogReader.h), and each
		// --nonzero column is above 0 in at least one row (e.g. a stage that must have run).
		if (argc < 4)
			return 2;
		const std::string_view rowsText(argv[3]);
		uint64_t rows = 0;
		const auto [rowsEnd, rowsError] = std::from_chars(rowsText.data(), rowsText.data() + rowsText.size(), rows);
		if (rowsError != std::errc() || rowsEnd != rowsText.data() + rowsText.size())
			return 2;
		std::string error;
		const std::optional<Strata::Tests::PerfLogTable> table = Strata::Tests::ReadPerfLog(Strata::FileSystem::FromUTF8(argv[2]), error);
		if (!table || !Strata::Tests::ValidatePerfLog(*table, rows, error))
		{
			std::fprintf(stderr, "'%s' is not the expected perf log: %s\n", argv[2], error.c_str());
			return 1;
		}
		for (int index = 4; index < argc; index++)
		{
			if (std::string_view(argv[index]) != "--nonzero" || index + 1 >= argc)
				return 2;
			const char* columnName = argv[++index];
			const std::optional<size_t> column = table->FindColumn(columnName);
			const bool nonzero = column && std::any_of(table->Rows.begin(), table->Rows.end(), [&](const std::vector<double>& row) { return row[*column] > 0.0; });
			if (!nonzero)
			{
				std::fprintf(stderr, "'%s': %s\n", argv[2], column ? fmt::format("'{}' is 0 in every row", columnName).c_str()
					: fmt::format("no column '{}'", columnName).c_str());
				return 1;
			}
		}
		std::printf("'%s': %zu frames, %zu columns\n", argv[2], table->Rows.size(), table->Columns.size());
		return 0;
	}

	if (mode == "compare-documents")
	{
		// <first asset directory> <second asset directory> <document>...: succeeds if each document (a scene, prefab or
		// material, by its path relative to the asset directories) holds the same content in both projects once references to
		// their assets are named by path and the other IDs are numbered by first appearance across the documents
		// (Tests::ProjectNormalizer): what a replay of a session on a copy of the project made against what the session made.
		// Prints the first difference otherwise.
		if (argc < 5)
			return 2;
		const std::filesystem::path assetDirectories[2] = { Strata::FileSystem::FromUTF8(argv[2]), Strata::FileSystem::FromUTF8(argv[3]) };
		std::optional<Strata::Tests::ProjectNormalizer> normalizers[2];
		for (int side = 0; side < 2; side++)
		{
			std::optional<std::unordered_map<std::string, std::string>> assets = Strata::Tests::ReadAssetNames(assetDirectories[side]);
			if (!assets)
			{
				std::fprintf(stderr, "The .meta files below '%s' cannot be read\n", argv[2 + side]);
				return 1;
			}
			normalizers[side].emplace(std::move(*assets));
		}
		for (int index = 4; index < argc; index++)
		{
			nlohmann::json documents[2];
			for (int side = 0; side < 2; side++)
			{
				const std::optional<std::string> text = Strata::FileSystem::ReadText(assetDirectories[side] / Strata::FileSystem::FromUTF8(argv[index]));
				const nlohmann::json document = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json(nlohmann::json::value_t::discarded);
				if (document.is_discarded())
				{
					std::fprintf(stderr, "'%s' of '%s' is not a readable JSON document\n", argv[index], argv[2 + side]);
					return 1;
				}
				documents[side] = normalizers[side]->Normalize(document);
			}
			if (documents[0] != documents[1])
			{
				std::fprintf(stderr, "'%s' differs at %s\n", argv[index], Strata::Tests::DescribeFirstDifference(documents[0], documents[1]).c_str());
				return 1;
			}
			std::printf("'%s' matches\n", argv[index]);
		}
		return 0;
	}

#if defined(ST_PLATFORM_WINDOWS)
	if (mode == "check-icon")
	{
		// <executable> <icon file>: succeeds if the executable's icon (the first RT_GROUP_ICON) holds an image of every size
		// the .ico file has (StrataEditor/Resources/Brand/StrataMark.ico: the strata mark).
		if (argc != 4)
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
		const std::optional<std::vector<uint32_t>> expectedSizes = Strata::Tests::ReadIconFileSizes(Strata::FileSystem::FromUTF8(argv[3]), &error);
		if (!expectedSizes || expectedSizes->empty())
		{
			std::fprintf(stderr, "'%s': %s\n", argv[3], expectedSizes ? "no images" : error.c_str());
			return 1;
		}
		for (uint32_t expected : *expectedSizes)
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
// anything, so the module file stays as the test left it, and prints which step it ran ("-- Fake CMake: -S" or
// "-- Fake CMake: --build"), which tests read in the build's log. Other modes fail a step (below).
static std::optional<int> RunAsFakeCMake(int argc, char** argv)
{
	const std::optional<std::string> mode = Strata::Platform::GetEnvVar("STRATA_TEST_FAKE_CMAKE");
	if (!mode || argc < 2 || (std::string_view(argv[1]) != "-S" && std::string_view(argv[1]) != "--build"))
		return std::nullopt;
	if (*mode == "succeed")
	{
		std::printf("-- Fake CMake: %s\n", argv[1]);
		std::fflush(stdout);
		return 0;
	}
	// "fail": configuring succeeds, building reports a compiler error and a warning in the project's Scripts/Broken.cpp
	// (relative to the project, as the Problems panel resolves it) and fails. The warning names a token pasted with "##",
	// which UI labels would cut.
	if (*mode == "fail")
	{
		if (std::string_view(argv[1]) == "-S")
		{
			std::printf("-- Fake CMake: configured\n");
			std::fflush(stdout);
			return 0;
		}
		std::printf("Scripts/Broken.cpp(7,5): error C2065: 'Undeclared': undeclared identifier\n");
		std::printf("Scripts/Broken.cpp(9,1): warning C4668: 'TOKEN##QUIRK' is not defined as a preprocessor macro, replacing with '0' for "
			"'#if/#elif'\n");
		std::fflush(stdout);
		return 1;
	}
	// "fail-configure": configuring reports a CMake error in the scripts' CMakeLists.txt and fails; building succeeds.
	if (*mode == "fail-configure")
	{
		if (std::string_view(argv[1]) == "-S")
		{
			std::printf("CMake Error at CMakeLists.txt:3 (message):\n  Fake configure error\n\n");
			std::printf("-- Configuring incomplete, errors occurred!\n");
			std::fflush(stdout);
			return 1;
		}
		std::printf("-- Fake CMake: %s\n", argv[1]);
		std::fflush(stdout);
		return 0;
	}
	return std::nullopt;
}

int main(int argc, char** argv)
{
	// A console program, also in its helper modes: what goes wrong is printed, never shown in a dialog (applications the
	// tests make in the process report failures with Application::ReportFatalError, which nobody would dismiss).
	Strata::Platform::ConnectStandardStreams(Strata::ProgramRun::Console);
	// Only warnings and errors are logged, by the tests and by the helper processes, whose output their tests read (the
	// composition root's line about the scene systems' order would run into it); helper modes that log more initialize the
	// log themselves.
	Strata::LogSpecification logSpecification;
	logSpecification.Level = Strata::LogLevel::Warn;
	Strata::Log::Init(logSpecification);

	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	const bool isHelper = argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix;
	const std::string_view helperMode = isHelper ? std::string_view(argv[1]).substr(helperPrefix.size()) : std::string_view();
	if (isHelper)
	{
		if (const std::optional<int> result = Strata::Tests::RunModuleRegistrationHelper(helperMode, argc, argv))
			return *result;
	}

	// Like every program that uses the engine, before anything touches a registry; with the asset pipeline, like the editor.
	Strata::Engine::ModuleRegistrationOptions modules;
	modules.AssetPipeline = Strata::RegisterAssetPipeline;
	Strata::Engine::RegisterBuiltinModules(modules);

	if (isHelper)
		return RunHelperMode(helperMode, argc, argv);
	if (const std::optional<int> fakeCMake = RunAsFakeCMake(argc, argv))
		return *fakeCMake;

	// Launched as an editor by the CLI launch tests (see Network/FakeEditorProcess.h).
	if (Strata::Tests::IsFakeEditorLaunch(argc, argv))
		return Strata::Tests::RunFakeEditor(argc, argv);

	// The perf suites measure the engine as applications run it, and applications opt out of execution-speed throttling
	// (Application): so does the test process, whose console is rarely the window in the foreground.
	Strata::Platform::DisableExecutionSpeedThrottling();

	// The tests (and the helper processes they start, which inherit the environment) keep their runtime files - script
	// module copies, private directories - in a private directory of their own, never in the user's.
	const std::filesystem::path runtimeDirectory = Strata::Tests::CreateTemporaryDirectory("Runtime");
	std::error_code permissionError;
	std::filesystem::permissions(runtimeDirectory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, permissionError);
	Strata::Platform::SetEnvVar("STRATA_RUNTIME_DIR", Strata::FileSystem::ToUTF8(runtimeDirectory));
	// Editors the tests start keep their recent projects in the test's directory, never in the user's list.
	Strata::Platform::SetEnvVar("STRATA_RECENT_PROJECTS", Strata::FileSystem::ToUTF8(runtimeDirectory / "RecentProjects.json"));
	// Script builds of projects too deep for MSBuild go to the test's directory, never to the user's build directory.
	Strata::Platform::SetEnvVar("STRATA_SCRIPT_BUILDS_DIR", Strata::FileSystem::ToUTF8(runtimeDirectory / "ScriptBuilds"));

	doctest::Context context(argc, argv);
	const int result = context.run();

	Strata::Tests::GPUContext::ShutdownShared();
	Strata::Log::Shutdown();
	Strata::Tests::CleanupTemporaryDirectories();
	return result;
}
