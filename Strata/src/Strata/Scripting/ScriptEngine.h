#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Scripting/ScriptTypes.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class FileWatcher;
	class ScriptModule;
	class ScriptSystem;
	class ScriptWatchdog;

	// Owns the game's script module: a shared library of C++ script classes built against StrataScriptCore (see
	// AGENTS.md, "Scripting"). Scenes that play while an engine is active (SetActive) run their Script components through
	// it (ScriptSystem). Main thread only.
	//
	// Crash containment: every call into the module is guarded. When script code crashes (access violation, division by
	// zero, stack overflow, an exception escaping the SDK, and on Windows abort() - also from a failed assert() or
	// std::terminate), the module is marked faulted: it is not called again and every script instance becomes inert,
	// while the engine keeps running. Poll IsFaulted()/GetFault() to react (the editor stops play mode). Reloading the module clears the
	// fault. Exceptions thrown by scripts are caught by the SDK and only disable the instance that threw.
	//
	// Limitations: native code cannot be preempted, so an infinite loop in a script blocks the main thread (an optional
	// watchdog reports it); a crash inside the module's static initializers or destructors (run while the library loads
	// or unloads) fails the load or abandons the library (the Windows loader contains such crashes itself; elsewhere the
	// crash guard reports them), but what the library left behind may make the process crash when it exits, and outside
	// Windows the platform's loader may be left in an undefined state (an error recommends a restart; until then that file
	// is only loaded from copies, so the loader never hands out the broken library again); std::terminate (an exception
	// leaving a noexcept function or a destructor) is contained on Windows through abort(), but the C++ runtime keeps the
	// abandoned exception; a crash inside the C library's allocator (a corrupted heap) can leave it locked, so the main
	// thread blocks at its next allocation; stray writes into engine memory are not detected; memory of instances
	// abandoned after a crash is leaked. Not contained at all (the process ends): on Linux and macOS abort() (also a
	// failed assert() or std::terminate), because the C library also aborts on heap corruption while it holds allocator
	// locks - it is reported on stderr first; Windows fail-fast terminations (__fastfail: /GS buffer overrun checks,
	// invalid-parameter failures of the C runtime, heap corruption the system detects); abort() in Windows modules that
	// link the C runtime dynamically (/MD) or do not use the SDK's entry points; and calls that end the process (exit,
	// TerminateProcess).
	class ScriptEngine
	{
	public:
		ScriptEngine();
		~ScriptEngine();

		ScriptEngine(const ScriptEngine&) = delete;
		ScriptEngine& operator=(const ScriptEngine&) = delete;

		// Loads the module at `path`: from the file itself, or from a private copy while hot reload is enabled (see
		// SetHotReloadEnabled). A file that is already loaded (by this or another engine) is always loaded from a copy. If
		// a module is already loaded this is a reload: running scenes keep their script state (see Reload). On failure
		// the current module (if any) keeps running and the error is logged.
		bool LoadModule(const std::filesystem::path& path, std::string* outError = nullptr);
		// Loads the module file again (hot reload). Running scenes snapshot every script's fields, destroy the instances
		// without OnDestroy, recreate them from the new code with the fields that still exist (same name and type) and
		// call OnReload. Classes that no longer exist lose their instances.
		bool Reload(std::string* outError = nullptr);
		// Destroys every script instance of running scenes (calling OnDestroy) and unloads the module.
		void UnloadModule();

		bool IsModuleLoaded() const;
		// Absolute path of the module file (empty when none is loaded).
		const std::filesystem::path& GetModulePath() const { return m_ModulePath; }
		// Name the module reports (its file name without extension by default); empty when none is loaded.
		std::string GetModuleName() const;
		// Number of successful loads and reloads.
		uint64_t GetLoadCount() const { return m_LoadCount; }

		// Watches the module file and reloads it in Update() when it changes (once the file is completely written). While
		// enabled, modules load from a private copy in a directory only the user can modify, so the build can replace the
		// file; enable it before LoadModule (a module that already runs from its file is moved to a copy at the next
		// Update(), like a reload). Without hot reload (shipped games) modules load in place.
		void SetHotReloadEnabled(bool enabled);
		bool IsHotReloadEnabled() const { return m_HotReloadEnabled; }
		// Once per frame, outside scene updates: performs pending hot reloads.
		void Update();
		bool IsReloadPending() const { return m_ReloadPending; }

		// Classes of the loaded module (empty without one). Valid until the module is unloaded or reloaded.
		const std::vector<ScriptClassInfo>& GetClasses() const;
		const ScriptClassInfo* FindClass(std::string_view name) const;

		bool IsFaulted() const;
		// The crash that faulted the current module, if any.
		std::optional<ScriptFault> GetFault() const;
		// Number of faults since the engine was created (also counts faults of modules replaced since).
		uint64_t GetFaultCount() const;

		// Reports script calls running longer than `timeout` (likely infinite loops); zero disables the watchdog.
		void SetWatchdogTimeout(std::chrono::milliseconds timeout);
		std::chrono::milliseconds GetWatchdogTimeout() const;
		uint64_t GetWatchdogReportCount() const;

		// Engine-internal access to the loaded module (null when none is loaded).
		ScriptModule* GetModule() const { return m_Module.get(); }

		// The engine used by scenes that start playing (null: scenes run without scripts).
		static void SetActive(const Ref<ScriptEngine>& engine);
		static const Ref<ScriptEngine>& GetActive();

		// File name of a module built by strata_add_script_module(): "<name>.dll", "<name>.so" or "<name>.dylib".
		static std::string GetModuleFileName(std::string_view moduleName);
	private:
		friend class ScriptSystem;
		void AttachSystem(ScriptSystem& system);
		void DetachSystem(ScriptSystem& system);

		bool LoadInternal(const std::filesystem::path& path, std::string* outError, bool* outRetryable);
		void StartWatching();
		void StopWatching();
	private:
		// Declared before the module so that it outlives it (the module reports calls to it).
		Scope<ScriptWatchdog> m_Watchdog;
		Scope<ScriptModule> m_Module;
		std::filesystem::path m_ModulePath;
		uint64_t m_LoadCount = 0;
		uint64_t m_ReplacedFaultCount = 0; // Faults of modules that were unloaded or replaced
		std::vector<ScriptSystem*> m_Systems;

		Scope<FileWatcher> m_Watcher;
		bool m_HotReloadEnabled = false;
		bool m_ReloadPending = false;
		uint32_t m_ReloadAttempts = 0;
		std::chrono::steady_clock::time_point m_NextReloadAttempt;
	};

}
