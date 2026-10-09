#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/Property.h"
#include "Strata/Scripting/ScriptTypes.h"

#include "StrataScript/ScriptABI.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Scene;
	class ScriptModuleCopyDirectory;
	class ScriptWatchdog;

	// How ScriptModule::Load loads the module's library.
	enum class ScriptModuleLoadMode : uint8_t
	{
		InPlace, // From the file itself (shipped games). A file that is already loaded is loaded from a copy instead.
		Copy     // From a private copy, so the build can replace the file while the module runs (hot reload)
	};

	enum class ScriptCallResult : uint8_t
	{
		Ok,
		Exception,  // The script threw (message in GetLastExceptionMessage); the module itself is fine
		Rejected,   // The module rejected the call, e.g. a field value of the wrong type
		Faulted,    // Script code crashed during the call; the module is now faulted
		Unavailable // Nothing was called: the module is faulted or the class does not implement the callback
	};

	// Identifies a call into script code for diagnostics (fault reports, warnings, the watchdog).
	struct ScriptCallSite
	{
		const ScriptClassInfo* Class = nullptr; // Null for module-level calls
		const char* Method = "";
		UUID Entity = UUID::Null();
		const Scene* EntityScene = nullptr;     // Resolves the entity's name in reports
	};

	struct ScriptModuleLoadError
	{
		std::string Message;
		bool Retryable = false; // The file was missing or busy (e.g. still being written); trying again later may work
	};

	// One loaded script module (engine-internal; see ScriptEngine). It runs from its file, or from a private copy of it
	// (ScriptModuleLoadMode) so the build can overwrite the original while the copy is in use. Copies live in a directory
	// only the user can modify (Platform::GetUserRuntimeDirectory), one per process, and are removed on unload.
	//
	// Every call into module code - including loading and unloading the library - runs under CrashGuard. A crash marks
	// the module faulted: it is never called again, its instances are abandoned (their memory is leaked) and only the
	// library itself is unloaded, also guarded. Main thread only.
	class ScriptModule
	{
	public:
		static Scope<ScriptModule> Load(const std::filesystem::path& path, ScriptModuleLoadMode mode, ScriptWatchdog* watchdog,
			ScriptModuleLoadError* outError = nullptr);
		~ScriptModule();

		ScriptModule(const ScriptModule&) = delete;
		ScriptModule& operator=(const ScriptModule&) = delete;

		const std::filesystem::path& GetSourcePath() const { return m_SourcePath; }
		// The file the library was loaded from: the source file, or the private copy.
		const std::filesystem::path& GetLoadedPath() const { return m_LoadedPath; }
		bool IsLoadedFromCopy() const { return m_CopyDirectory != nullptr; }
		const std::string& GetName() const { return m_Name; }

		const std::vector<ScriptClassInfo>& GetClasses() const { return m_Classes; }
		const ScriptClassInfo* FindClass(std::string_view name) const;

		bool IsFaulted() const { return m_Fault.has_value(); }
		const std::optional<ScriptFault>& GetFault() const { return m_Fault; }
		// Message of the most recent call that returned ScriptCallResult::Exception.
		const std::string& GetLastExceptionMessage() const { return m_LastException; }

		void SetWatchdog(ScriptWatchdog* watchdog) { m_Watchdog = watchdog; }

		// Calls into script code. site.Class selects the class; site.Method is filled in by these functions when empty.
		ScriptCallResult CreateInstance(const ScriptCallSite& site, StrataScriptContext* context, StrataScriptInstance* outInstance);
		ScriptCallResult DestroyInstance(const ScriptCallSite& site, StrataScriptInstance instance);
		ScriptCallResult GetField(const ScriptCallSite& site, StrataScriptInstance instance, uint32_t fieldIndex, PropertyValue& outValue);
		ScriptCallResult SetField(const ScriptCallSite& site, StrataScriptInstance instance, uint32_t fieldIndex, const PropertyValue& value);
		// `argument` is the delta time of the update callbacks (ignored by the others).
		ScriptCallResult InvokeCallback(const ScriptCallSite& site, StrataScriptInstance instance, ScriptCallback callback, float argument);

		// Script host API support: the exception message a module reports for its current call, and the innermost call
		// into script code running on this thread (null outside script code).
		static void ReportException(std::string_view message);
		static const ScriptCallSite* GetCurrentCall();
	private:
		ScriptModule() = default;

		// Loads the library under the crash guard (loading runs the module's static initializers). On failure `outError`
		// is the complete message.
		bool LoadLibraryGuarded(const std::filesystem::path& path, const std::string& displayPath, std::string& outError);
		template<typename Function>
		ScriptCallResult Call(const ScriptCallSite& site, Function&& function);
		void RecordFault(const ScriptCallSite& site, const CrashInfo& crash);
		const StrataScriptClassDesc* GetDescriptor(const ScriptCallSite& site) const;
		// Validates the module description and builds the class metadata. Reads module memory: call guarded.
		bool ReadModuleDescription(std::string& outName, std::vector<ScriptClassInfo>& outClasses, std::vector<const StrataScriptClassDesc*>& outDescriptors,
			std::string& outError) const;
	private:
		std::filesystem::path m_SourcePath;
		std::filesystem::path m_LoadedPath;
		Ref<ScriptModuleCopyDirectory> m_CopyDirectory; // Holds the copy the module runs from (null when it runs in place)
		std::string m_Name;
		DynamicLibrary m_Library;
		StrataScriptModuleAPI m_API = {};
		bool m_Initialized = false; // StrataScript_Load succeeded; Unload is due
		bool m_Ready = false;       // Validated and in use (crashes before that are reported as load failures)
		std::vector<ScriptClassInfo> m_Classes;
		std::vector<const StrataScriptClassDesc*> m_Descriptors; // Per class index
		std::optional<ScriptFault> m_Fault;
		std::string m_LastException;
		ScriptWatchdog* m_Watchdog = nullptr;
	};

}
