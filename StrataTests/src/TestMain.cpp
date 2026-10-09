#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptModule.h"
#include "TestHelpers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#if defined(ST_PLATFORM_POSIX)
	#include <csetjmp>
	#include <csignal>
	#include <pthread.h>
	#include <sys/types.h>
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

}
#endif

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
	if (mode == "exit-code")
		return argc > 2 ? std::atoi(argv[2]) : 0;
	if (mode == "sleep")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(argc > 2 ? std::atoi(argv[2]) : 10000));
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
	return 99;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);

	Strata::LogSpecification logSpecification;
	logSpecification.Level = Strata::LogLevel::Warn;
	Strata::Log::Init(logSpecification);

	// The tests (and the helper processes they start, which inherit the environment) keep their runtime files - script
	// module copies, private directories - in a private directory of their own, never in the user's.
	const std::filesystem::path runtimeDirectory = Strata::Tests::CreateTemporaryDirectory("Runtime");
	std::error_code permissionError;
	std::filesystem::permissions(runtimeDirectory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, permissionError);
	Strata::Platform::SetEnvVar("STRATA_RUNTIME_DIR", Strata::FileSystem::ToUTF8(runtimeDirectory));

	doctest::Context context(argc, argv);
	const int result = context.run();

	Strata::Tests::GPUContext::ShutdownShared();
	Strata::Log::Shutdown();
	Strata::Tests::CleanupTemporaryDirectories();
	return result;
}
