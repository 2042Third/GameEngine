#include "stpch.h"
#include "Strata/Scripting/ScriptEngine.h"

#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FileWatcher.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Scripting/ScriptModule.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "Strata/Scripting/ScriptWatchdog.h"

namespace Strata
{

	namespace
	{

		// A file being rewritten by the build can be busy or missing for a moment; retry for a few seconds.
		constexpr uint32_t c_MaxReloadAttempts = 20;
		constexpr std::chrono::milliseconds c_ReloadRetryInterval(250);

		Ref<ScriptEngine> s_ActiveEngine;

		bool IsSameFileName(const std::filesystem::path& a, const std::filesystem::path& b)
		{
#if defined(ST_PLATFORM_WINDOWS)
			return StringUtils::EqualsIgnoreCase(FileSystem::ToUTF8(a.filename()), FileSystem::ToUTF8(b.filename()));
#else
			return a.filename() == b.filename();
#endif
		}

	}

	ScriptEngine::ScriptEngine() = default;

	ScriptEngine::~ScriptEngine()
	{
		ST_CORE_ASSERT(m_Systems.empty(), "Script systems hold a reference to their engine");
		StopWatching();
		m_Module.reset();
		m_Watchdog.reset();
	}

	bool ScriptEngine::LoadModule(const std::filesystem::path& path, std::string* outError)
	{
		std::error_code error;
		std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
		if (error)
			absolutePath = path;
		return LoadInternal(absolutePath.lexically_normal(), outError, nullptr);
	}

	bool ScriptEngine::Reload(std::string* outError)
	{
		if (m_ModulePath.empty())
		{
			if (outError)
				*outError = "No script module is loaded";
			return false;
		}
		return LoadInternal(m_ModulePath, outError, nullptr);
	}

	bool ScriptEngine::LoadInternal(const std::filesystem::path& path, std::string* outError, bool* outRetryable)
	{
		ST_PROFILE_FUNCTION();

		if (ScriptModule::GetCurrentCall())
		{
			// Script code is on the stack (e.g. a host call): replacing the module now would unload running code.
			const std::string message = "Script modules cannot be loaded while script code is running";
			ST_CORE_ERROR("{}", message);
			if (outError)
				*outError = message;
			return false;
		}

		// The new module loads next to the old one, so a broken build never disturbs the running game. With hot reload it
		// runs from a private copy, so the build can replace the file while it is loaded.
		ScriptModuleLoadError loadError;
		const ScriptModuleLoadMode mode = m_HotReloadEnabled ? ScriptModuleLoadMode::Copy : ScriptModuleLoadMode::InPlace;
		Scope<ScriptModule> module = ScriptModule::Load(path, mode, m_Watchdog.get(), &loadError);
		if (!module)
		{
			ST_CORE_ERROR("{}{}", loadError.Message, m_Module ? "; the previously loaded module keeps running" : "");
			if (outError)
				*outError = loadError.Message;
			if (outRetryable)
				*outRetryable = loadError.Retryable;
			return false;
		}

		// Running scenes move their script state over to the new module. Iterate over a copy: systems never attach or
		// detach from these callbacks, but the list must not be observed while it could change.
		const std::vector<ScriptSystem*> systems = m_Systems;
		const bool replacing = m_Module != nullptr;
		if (replacing)
		{
			for (ScriptSystem* system : systems)
				system->BeginModuleReload();
			if (m_Module->IsFaulted())
				m_ReplacedFaultCount++;
		}

		const std::filesystem::path previousDirectory = m_ModulePath.parent_path();
		m_Module = std::move(module); // Unloads the previous module
		m_ModulePath = path;
		m_LoadCount++;

		for (ScriptSystem* system : systems)
		{
			if (replacing)
				system->EndModuleReload();
			else
				system->OnModuleLoaded();
		}

		ST_CORE_INFO("{} script module '{}' ({} classes)", replacing ? "Reloaded" : "Loaded", m_Module->GetName(), m_Module->GetClasses().size());
		if (m_HotReloadEnabled && previousDirectory != m_ModulePath.parent_path())
			StartWatching();
		return true;
	}

	void ScriptEngine::UnloadModule()
	{
		if (!m_Module)
			return;
		ST_CORE_ASSERT(!ScriptModule::GetCurrentCall(), "Script modules cannot be unloaded while script code is running");

		const std::vector<ScriptSystem*> systems = m_Systems;
		for (ScriptSystem* system : systems)
			system->OnModuleUnloading();

		if (m_Module->IsFaulted())
			m_ReplacedFaultCount++;
		m_Module.reset();
		m_ModulePath.clear();
		m_ReloadPending = false;
		StopWatching();
	}

	bool ScriptEngine::IsModuleLoaded() const
	{
		return m_Module != nullptr;
	}

	std::string ScriptEngine::GetModuleName() const
	{
		return m_Module ? m_Module->GetName() : std::string();
	}

	void ScriptEngine::SetHotReloadEnabled(bool enabled)
	{
		m_HotReloadEnabled = enabled;
		if (enabled)
			StartWatching();
		else
			StopWatching();

		// A module running from its file keeps the build from replacing it (on Windows the file is locked): move it to a
		// private copy at the next Update().
		if (enabled && m_Module && !m_Module->IsLoadedFromCopy())
		{
			m_ReloadPending = true;
			m_ReloadAttempts = 0;
			m_NextReloadAttempt = std::chrono::steady_clock::now();
		}
	}

	void ScriptEngine::StartWatching()
	{
		StopWatching();
		if (m_ModulePath.empty())
			return;

		FileWatcherSettings settings;
		settings.Recursive = false;
		settings.IgnoredDirectories.clear();
		// Linkers write the module in several steps; only a file that stayed unchanged for a while is complete.
		settings.Debounce = std::chrono::milliseconds(300);
		m_Watcher = CreateScope<FileWatcher>();
		if (!m_Watcher->Start(m_ModulePath.parent_path(), settings))
		{
			ST_CORE_WARN("Cannot watch '{}' for script module changes; hot reload is unavailable", FileSystem::ToUTF8(m_ModulePath.parent_path()));
			m_Watcher.reset();
		}
	}

	void ScriptEngine::StopWatching()
	{
		m_Watcher.reset();
	}

	void ScriptEngine::Update()
	{
		ST_PROFILE_FUNCTION();

		if (m_Watcher && !m_ModulePath.empty())
		{
			for (const FileChange& change : m_Watcher->PollChanges())
			{
				if (change.Type == FileChangeType::Removed || !IsSameFileName(change.Path, m_ModulePath))
					continue;
				m_ReloadPending = true;
				m_ReloadAttempts = 0;
				m_NextReloadAttempt = std::chrono::steady_clock::now();
			}
		}

		if (!m_ReloadPending || std::chrono::steady_clock::now() < m_NextReloadAttempt || ScriptModule::GetCurrentCall())
			return;

		std::string error;
		bool retryable = false;
		if (LoadInternal(m_ModulePath, &error, &retryable))
		{
			m_ReloadPending = false;
			return;
		}

		if (retryable && ++m_ReloadAttempts < c_MaxReloadAttempts)
		{
			m_NextReloadAttempt = std::chrono::steady_clock::now() + c_ReloadRetryInterval;
			return;
		}
		m_ReloadPending = false;
	}

	const std::vector<ScriptClassInfo>& ScriptEngine::GetClasses() const
	{
		static const std::vector<ScriptClassInfo> s_NoClasses;
		return m_Module ? m_Module->GetClasses() : s_NoClasses;
	}

	const ScriptClassInfo* ScriptEngine::FindClass(std::string_view name) const
	{
		return m_Module ? m_Module->FindClass(name) : nullptr;
	}

	bool ScriptEngine::IsFaulted() const
	{
		return m_Module && m_Module->IsFaulted();
	}

	std::optional<ScriptFault> ScriptEngine::GetFault() const
	{
		return m_Module ? m_Module->GetFault() : std::nullopt;
	}

	uint64_t ScriptEngine::GetFaultCount() const
	{
		return m_ReplacedFaultCount + (IsFaulted() ? 1 : 0);
	}

	void ScriptEngine::SetWatchdogTimeout(std::chrono::milliseconds timeout)
	{
		ST_CORE_ASSERT(!ScriptModule::GetCurrentCall(), "The script watchdog cannot change while script code is running");
		if (m_Module)
			m_Module->SetWatchdog(nullptr);
		m_Watchdog.reset();
		if (timeout.count() > 0)
			m_Watchdog = CreateScope<ScriptWatchdog>(timeout);
		if (m_Module)
			m_Module->SetWatchdog(m_Watchdog.get());
	}

	std::chrono::milliseconds ScriptEngine::GetWatchdogTimeout() const
	{
		return m_Watchdog ? m_Watchdog->GetTimeout() : std::chrono::milliseconds(0);
	}

	uint64_t ScriptEngine::GetWatchdogReportCount() const
	{
		return m_Watchdog ? m_Watchdog->GetReportCount() : 0;
	}

	void ScriptEngine::AttachSystem(ScriptSystem& system)
	{
		m_Systems.push_back(&system);
	}

	void ScriptEngine::DetachSystem(ScriptSystem& system)
	{
		m_Systems.erase(std::remove(m_Systems.begin(), m_Systems.end(), &system), m_Systems.end());
	}

	void ScriptEngine::SetActive(const Ref<ScriptEngine>& engine)
	{
		s_ActiveEngine = engine;
	}

	const Ref<ScriptEngine>& ScriptEngine::GetActive()
	{
		return s_ActiveEngine;
	}

	std::string ScriptEngine::GetModuleFileName(std::string_view moduleName)
	{
		return fmt::format("{}{}", moduleName, DynamicLibrary::GetFileExtension());
	}

}
