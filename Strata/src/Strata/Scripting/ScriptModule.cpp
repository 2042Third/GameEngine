#include "stpch.h"
#include "Strata/Scripting/ScriptModule.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptHostAPI.h"
#include "Strata/Scripting/ScriptValue.h"
#include "Strata/Scripting/ScriptWatchdog.h"

#include <charconv>
#include <cstddef>
#include <unordered_set>

namespace Strata
{

	// The private directory module copies are loaded from (in Platform::GetUserRuntimeDirectory): one per process,
	// created on first use and removed with the last copy. Its name carries the process ID, so directories left behind by
	// processes that ended without unloading (crashes, debugger stops) are removed by later sessions.
	class ScriptModuleCopyDirectory
	{
	public:
		static Ref<ScriptModuleCopyDirectory> Acquire(std::string& outError);
		~ScriptModuleCopyDirectory();

		ScriptModuleCopyDirectory(const ScriptModuleCopyDirectory&) = delete;
		ScriptModuleCopyDirectory& operator=(const ScriptModuleCopyDirectory&) = delete;

		const std::filesystem::path& GetPath() const { return m_Path; }
	private:
		explicit ScriptModuleCopyDirectory(std::filesystem::path path)
			: m_Path(std::move(path))
		{
		}
	private:
		std::filesystem::path m_Path;
	};

	namespace
	{

		// Limits that reject corrupt class tables before they are trusted.
		constexpr uint32_t c_MaxScriptClasses = 1u << 16;
		constexpr uint32_t c_MaxScriptFields = 1u << 12;
		constexpr size_t c_MaxScriptNameSize = 1024;

		thread_local const ScriptCallSite* t_CurrentCall = nullptr;
		thread_local std::string t_PendingException;

		// Members the engine needs from each struct; a module built against this ABI version always has them.
		constexpr size_t c_RequiredModuleAPISize = offsetof(StrataScriptModuleAPI, Unload) + sizeof(StrataScriptModuleAPI::Unload);
		constexpr size_t c_RequiredClassDescSize = offsetof(StrataScriptClassDesc, OnReload) + sizeof(StrataScriptClassDesc::OnReload);
		constexpr size_t c_RequiredFieldDescSize = offsetof(StrataScriptFieldDesc, DefaultValue) + sizeof(StrataScriptFieldDesc::DefaultValue);

		constexpr std::string_view c_CopyDirectoryPrefix = "ScriptModules-";

		// Libraries script modules run from (native handles). Loading a file that is already loaded yields the same
		// library, whose module state belongs to the module that loaded it first. Libraries abandoned after their unload
		// code crashed stay registered: they remain loaded until the process ends.
		std::mutex s_LibrariesMutex;
		std::unordered_set<void*> s_Libraries;

		bool RegisterLibrary(void* handle)
		{
			std::scoped_lock<std::mutex> lock(s_LibrariesMutex);
			return s_Libraries.insert(handle).second;
		}

		void UnregisterLibrary(void* handle)
		{
			std::scoped_lock<std::mutex> lock(s_LibrariesMutex);
			s_Libraries.erase(handle);
		}

		// Copy directories ("ScriptModules-<process id>-<random>") of processes that are no longer running.
		void RemoveStaleCopyDirectories(const std::filesystem::path& parent)
		{
			std::error_code error;
			for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error))
			{
				const std::string name = FileSystem::ToUTF8(it->path().filename());
				if (!name.starts_with(c_CopyDirectoryPrefix))
					continue;

				const char* first = name.data() + c_CopyDirectoryPrefix.size();
				const char* last = name.data() + name.size();
				uint32_t processID = 0;
				const auto [next, parseError] = std::from_chars(first, last, processID);
				if (parseError != std::errc() || next == first || next == last || *next != '-')
					continue;
				if (processID == Platform::GetProcessID() || Platform::IsProcessRunning(processID))
					continue;
				if (!FileSystem::Remove(it->path()))
					ST_CORE_WARN("Cannot remove the stale script module directory '{}'", FileSystem::ToUTF8(it->path()));
			}
		}

		bool ReadName(const StrataScriptString& text, std::string& out)
		{
			return ReadScriptString(text, out) && !out.empty() && out.size() <= c_MaxScriptNameSize;
		}

	}

	Ref<ScriptModuleCopyDirectory> ScriptModuleCopyDirectory::Acquire(std::string& outError)
	{
		static std::mutex s_Mutex;
		static std::weak_ptr<ScriptModuleCopyDirectory> s_Current;

		std::scoped_lock<std::mutex> lock(s_Mutex);
		if (Ref<ScriptModuleCopyDirectory> current = s_Current.lock())
			return current;

		const std::filesystem::path parent = Platform::GetUserRuntimeDirectory("Strata");
		if (parent.empty())
		{
			outError = "there is no per-user runtime directory that only this user can modify";
			return nullptr;
		}
		RemoveStaleCopyDirectories(parent);

		std::filesystem::path path = Platform::CreatePrivateDirectory(parent, fmt::format("{}{}-", c_CopyDirectoryPrefix, Platform::GetProcessID()));
		if (path.empty())
		{
			outError = fmt::format("cannot create a private directory in '{}'", FileSystem::ToUTF8(parent));
			return nullptr;
		}
		Ref<ScriptModuleCopyDirectory> directory(new ScriptModuleCopyDirectory(std::move(path)));
		s_Current = directory;
		return directory;
	}

	ScriptModuleCopyDirectory::~ScriptModuleCopyDirectory()
	{
		// Fails only while a copy is still in use (a library abandoned after a crash); a later session removes it then.
		if (!FileSystem::Remove(m_Path))
			ST_CORE_WARN("Cannot remove the script module directory '{}'; it is removed by a later session", FileSystem::ToUTF8(m_Path));
	}

	template<typename Function>
	ScriptCallResult ScriptModule::Call(const ScriptCallSite& site, Function&& function)
	{
		if (m_Fault)
			return ScriptCallResult::Unavailable;

		struct Invocation
		{
			std::remove_reference_t<Function>* Body;
			uint32_t Result;
		};
		Invocation invocation { &function, StrataScriptResult_Ok };

		const ScriptCallSite* previousCall = t_CurrentCall;
		t_CurrentCall = &site;
		t_PendingException.clear();
		if (m_Watchdog)
			m_Watchdog->BeginCall(site.Class ? site.Class->Name.c_str() : m_Name.c_str(), site.Method, site.Entity);

		CrashInfo crash;
		const bool completed = CrashGuard::Invoke([](void* data)
		{
			Invocation* call = static_cast<Invocation*>(data);
			call->Result = (*call->Body)();
		}, &invocation, &crash);

		if (m_Watchdog)
			m_Watchdog->EndCall();
		t_CurrentCall = previousCall;

		if (!completed)
		{
			RecordFault(site, crash);
			return ScriptCallResult::Faulted;
		}
		// A nested call (e.g. a script spawning a prefab whose script crashed in its constructor) faulted the module.
		if (m_Fault)
			return ScriptCallResult::Faulted;

		switch (invocation.Result)
		{
			case StrataScriptResult_Ok:
				return ScriptCallResult::Ok;
			case StrataScriptResult_Exception:
				m_LastException = t_PendingException.empty() ? std::string("Unknown exception") : std::move(t_PendingException);
				t_PendingException.clear();
				return ScriptCallResult::Exception;
			default:
				return ScriptCallResult::Rejected;
		}
	}

	bool ScriptModule::LoadLibraryGuarded(const std::filesystem::path& path, const std::filesystem::path& dependencyDirectory, const std::string& displayPath,
		std::string& outError)
	{
		struct LibraryLoad
		{
			DynamicLibrary* Library;
			const std::filesystem::path* Path;
			const std::filesystem::path* DependencyDirectory;
			bool Loaded;
		};
		LibraryLoad libraryLoad { &m_Library, &path, &dependencyDirectory, false };
		CrashInfo crash;
		if (!CrashGuard::Invoke([](void* data)
		{
			LibraryLoad* load = static_cast<LibraryLoad*>(data);
			load->Loaded = load->Library->Load(*load->Path, *load->DependencyDirectory);
		}, &libraryLoad, &crash))
		{
			RecordFault(ScriptCallSite { nullptr, "static initialization" }, crash);
			outError = fmt::format("Script module '{}' crashed while loading: {}", displayPath, crash.Description);
			return false;
		}
		if (!libraryLoad.Loaded)
		{
			outError = fmt::format("Cannot load script module '{}': {}", displayPath, m_Library.GetLastError());
			return false;
		}
		return true;
	}

	Scope<ScriptModule> ScriptModule::Load(const std::filesystem::path& path, ScriptModuleLoadMode mode, ScriptWatchdog* watchdog, ScriptModuleLoadError* outError)
	{
		ST_PROFILE_FUNCTION();

		auto fail = [&](std::string message, bool retryable = false) -> Scope<ScriptModule>
		{
			if (outError)
			{
				outError->Message = std::move(message);
				outError->Retryable = retryable;
			}
			return nullptr;
		};

		const std::string displayPath = FileSystem::ToUTF8(path);
		if (!FileSystem::IsRegularFile(path))
			return fail(fmt::format("Script module '{}' does not exist", displayPath), true);

		Scope<ScriptModule> module(new ScriptModule());
		module->m_SourcePath = path;
		module->m_Name = FileSystem::ToUTF8(path.stem());
		module->m_Watchdog = watchdog;

		std::string error;
		bool loaded = false;
		if (mode == ScriptModuleLoadMode::InPlace)
		{
			if (!module->LoadLibraryGuarded(path, {}, displayPath, error))
				return fail(std::move(error));
			if (RegisterLibrary(module->m_Library.GetNativeHandle()))
			{
				module->m_LoadedPath = path;
				loaded = true;
			}
			else
			{
				// The file is loaded already (a reload, another engine): this load only added a reference to that library,
				// whose module state is in use. Dropping the reference runs no module code; the module loads from a copy.
				module->m_Library.Unload();
			}
		}

		if (!loaded)
		{
			// A private copy, so the build can replace the original file while the module is in use.
			module->m_CopyDirectory = ScriptModuleCopyDirectory::Acquire(error);
			if (!module->m_CopyDirectory)
				return fail(fmt::format("Cannot load script module '{}' from a private copy: {}", displayPath, error));

			const std::filesystem::path copyPath = module->m_CopyDirectory->GetPath()
				/ FileSystem::FromUTF8(fmt::format("{}-{}{}", FileSystem::ToUTF8(path.stem()), UUID().ToString(), FileSystem::ToUTF8(path.extension())));
			if (!FileSystem::Copy(path, copyPath, false))
				return fail(fmt::format("Cannot copy script module '{}' (it may still be being written)", displayPath), true);
			module->m_LoadedPath = copyPath; // From now on the destructor removes the copy

			// The libraries the module depends on stay next to the original.
			if (!module->LoadLibraryGuarded(copyPath, path.parent_path(), displayPath, error))
				return fail(std::move(error));
			if (!RegisterLibrary(module->m_Library.GetNativeHandle()))
			{
				module->m_Library.Unload();
				return fail(fmt::format("Cannot load script module '{}': its private copy resolved to a library that is already loaded", displayPath));
			}
		}

		const auto getVersion = module->m_Library.GetFunction<StrataScriptGetABIVersionFunction>(ST_SCRIPT_GET_ABI_VERSION_SYMBOL);
		const auto load = module->m_Library.GetFunction<StrataScriptLoadFunction>(ST_SCRIPT_LOAD_SYMBOL);
		if (!getVersion || !load)
			return fail(fmt::format("'{}' is not a Strata script module: it does not export {} and {}", displayPath, ST_SCRIPT_GET_ABI_VERSION_SYMBOL, ST_SCRIPT_LOAD_SYMBOL));

		uint32_t moduleVersion = 0;
		const ScriptCallResult versionResult = module->Call({ nullptr, ST_SCRIPT_GET_ABI_VERSION_SYMBOL }, [&]()
		{
			moduleVersion = getVersion();
			return static_cast<uint32_t>(StrataScriptResult_Ok);
		});
		if (versionResult != ScriptCallResult::Ok)
			return fail(fmt::format("Script module '{}' crashed while reporting its ABI version: {}", displayPath, module->m_Fault ? module->m_Fault->Description : ""));
		if (moduleVersion != ST_SCRIPT_ABI_VERSION)
		{
			return fail(fmt::format("Script module '{}' was built against script ABI version {}, but this engine uses version {}; rebuild the module "
				"against this engine's StrataScriptCore", displayPath, moduleVersion, ST_SCRIPT_ABI_VERSION));
		}

		StrataScriptModuleAPI api = {};
		const ScriptCallResult loadResult = module->Call({ nullptr, ST_SCRIPT_LOAD_SYMBOL }, [&]()
		{
			return load(&GetScriptHostAPI(), ST_SCRIPT_ABI_VERSION, &api);
		});
		switch (loadResult)
		{
			case ScriptCallResult::Ok:
				break;
			case ScriptCallResult::Exception:
				return fail(fmt::format("Script module '{}' failed to initialize: {}", displayPath, module->m_LastException));
			case ScriptCallResult::Faulted:
			case ScriptCallResult::Unavailable:
				return fail(fmt::format("Script module '{}' crashed while initializing: {}", displayPath, module->m_Fault ? module->m_Fault->Description : ""));
			case ScriptCallResult::Rejected:
				return fail(fmt::format("Script module '{}' rejected the engine (script ABI version {})", displayPath, ST_SCRIPT_ABI_VERSION));
		}
		module->m_API = api;
		module->m_Initialized = true;

		// The description lives in module memory; reading it is guarded like any other module access.
		std::string name;
		std::vector<ScriptClassInfo> classes;
		std::vector<const StrataScriptClassDesc*> descriptors;
		std::string descriptionError;
		bool descriptionValid = false;
		const ScriptCallResult descriptionResult = module->Call({ nullptr, "module description" }, [&]()
		{
			std::string readName;
			std::vector<ScriptClassInfo> readClasses;
			std::vector<const StrataScriptClassDesc*> readDescriptors;
			std::string readError;
			const bool valid = module->ReadModuleDescription(readName, readClasses, readDescriptors, readError);
			// Moving engine objects cannot fault, so the outputs are either untouched or complete.
			name = std::move(readName);
			classes = std::move(readClasses);
			descriptors = std::move(readDescriptors);
			descriptionError = std::move(readError);
			descriptionValid = valid;
			return static_cast<uint32_t>(StrataScriptResult_Ok);
		});
		if (descriptionResult != ScriptCallResult::Ok)
			return fail(fmt::format("Script module '{}' has an unreadable description: {}", displayPath, module->m_Fault ? module->m_Fault->Description : ""));
		if (!descriptionValid)
			return fail(fmt::format("Script module '{}' is invalid: {}", displayPath, descriptionError));

		if (!name.empty())
			module->m_Name = std::move(name);
		module->m_Classes = std::move(classes);
		module->m_Descriptors = std::move(descriptors);
		module->m_Ready = true;
		return module;
	}

	ScriptModule::~ScriptModule()
	{
		if (m_Initialized && !m_Fault && m_API.Unload)
		{
			const auto unload = m_API.Unload;
			Call({ nullptr, "Unload" }, [&]() { return unload(); });
		}

		if (m_Library.IsLoaded())
		{
			// Unloading runs the module's static destructors.
			void* library = m_Library.GetNativeHandle();
			CrashInfo crash;
			if (CrashGuard::Invoke([](void* data) { static_cast<DynamicLibrary*>(data)->Unload(); }, &m_Library, &crash))
			{
				UnregisterLibrary(library);
			}
			else
			{
				// The library stays registered: it remains loaded, so loading its file again would return it.
				ST_CORE_ERROR("Script module '{}' crashed while unloading ({}); it stays loaded", m_Name, crash.Description);
				m_Library.Release();
			}
		}

		// The copy goes with the module (its directory with the last copy).
		if (m_CopyDirectory && !m_LoadedPath.empty() && FileSystem::Exists(m_LoadedPath) && !FileSystem::Remove(m_LoadedPath))
			ST_CORE_WARN("Cannot remove the script module copy '{}'; it is removed by a later session", FileSystem::ToUTF8(m_LoadedPath));
	}

	const ScriptClassInfo* ScriptModule::FindClass(std::string_view name) const
	{
		for (const ScriptClassInfo& info : m_Classes)
		{
			if (info.Name == name)
				return &info;
		}
		return nullptr;
	}

	void ScriptModule::RecordFault(const ScriptCallSite& site, const CrashInfo& crash)
	{
		ScriptFault fault;
		fault.ModuleName = m_Name;
		fault.ClassName = site.Class ? site.Class->Name : std::string();
		fault.Method = site.Method ? site.Method : "";
		fault.Entity = site.Entity;
		if (site.EntityScene && site.Entity.IsValid())
		{
			if (const Entity entity = site.EntityScene->GetEntityByUUID(site.Entity))
				fault.EntityName = entity.GetName();
		}
		fault.Description = crash.Description;

		// A module that crashes while it loads is reported by Load, which refuses it.
		if (m_Ready)
		{
			std::string location = fault.ClassName.empty() ? fault.Method : fmt::format("{}.{}", fault.ClassName, fault.Method);
			if (fault.Entity.IsValid())
				location += fmt::format(" on entity '{}' ({})", fault.EntityName, fault.Entity.ToString());
			ST_CORE_ERROR("Script module '{}' crashed in {}: {}. The module is disabled until it is reloaded.", m_Name, location, fault.Description);
		}

		if (!m_Fault)
			m_Fault = std::move(fault);
	}

	const StrataScriptClassDesc* ScriptModule::GetDescriptor(const ScriptCallSite& site) const
	{
		if (!site.Class || site.Class->Index >= m_Descriptors.size() || &m_Classes[site.Class->Index] != site.Class)
		{
			ST_CORE_ASSERT(false, "Script call site refers to a class of another module");
			return nullptr;
		}
		return m_Descriptors[site.Class->Index];
	}

	ScriptCallResult ScriptModule::CreateInstance(const ScriptCallSite& site, StrataScriptContext* context, StrataScriptInstance* outInstance)
	{
		*outInstance = nullptr;
		const StrataScriptClassDesc* descriptor = GetDescriptor(site);
		if (!descriptor)
			return ScriptCallResult::Rejected;

		StrataScriptInstance instance = nullptr;
		const auto create = descriptor->Create;
		const uint64_t entity = static_cast<uint64_t>(site.Entity);
		const ScriptCallResult result = Call(site, [&]() { return create(context, entity, &instance); });
		if (result == ScriptCallResult::Ok && !instance)
			return ScriptCallResult::Rejected;
		if (result == ScriptCallResult::Ok)
			*outInstance = instance;
		return result;
	}

	ScriptCallResult ScriptModule::DestroyInstance(const ScriptCallSite& site, StrataScriptInstance instance)
	{
		const StrataScriptClassDesc* descriptor = GetDescriptor(site);
		if (!descriptor || !instance)
			return ScriptCallResult::Rejected;
		const auto destroy = descriptor->Destroy;
		return Call(site, [&]() { return destroy(instance); });
	}

	ScriptCallResult ScriptModule::GetField(const ScriptCallSite& site, StrataScriptInstance instance, uint32_t fieldIndex, PropertyValue& outValue)
	{
		const StrataScriptClassDesc* descriptor = GetDescriptor(site);
		if (!descriptor || !instance || fieldIndex >= site.Class->Fields.size())
			return ScriptCallResult::Rejected;

		const auto getField = descriptor->GetField;
		const PropertyType type = site.Class->Fields[fieldIndex].Type;
		std::optional<PropertyValue> result;
		const ScriptCallResult callResult = Call(site, [&]()
		{
			StrataScriptValue value = {};
			const uint32_t status = getField(instance, fieldIndex, &value);
			if (status != StrataScriptResult_Ok)
				return status;
			// Copying a string out of module memory may fault; only a complete copy reaches `result`.
			std::optional<PropertyValue> converted = ScriptValueToFieldValue(value, type);
			result = std::move(converted);
			return status;
		});
		if (callResult != ScriptCallResult::Ok)
			return callResult;
		if (!result)
			return ScriptCallResult::Rejected;
		outValue = std::move(*result);
		return ScriptCallResult::Ok;
	}

	ScriptCallResult ScriptModule::SetField(const ScriptCallSite& site, StrataScriptInstance instance, uint32_t fieldIndex, const PropertyValue& value)
	{
		const StrataScriptClassDesc* descriptor = GetDescriptor(site);
		if (!descriptor || !instance || fieldIndex >= site.Class->Fields.size())
			return ScriptCallResult::Rejected;

		const PropertyType type = site.Class->Fields[fieldIndex].Type;
		if (value.index() != GetPropertyValueIndex(type))
			return ScriptCallResult::Rejected;

		const StrataScriptValue scriptValue = FieldValueToScriptValue(value, type);
		const auto setField = descriptor->SetField;
		return Call(site, [&]() { return setField(instance, fieldIndex, &scriptValue); });
	}

	ScriptCallResult ScriptModule::InvokeCallback(const ScriptCallSite& site, StrataScriptInstance instance, ScriptCallback callback, float argument)
	{
		const StrataScriptClassDesc* descriptor = GetDescriptor(site);
		if (!descriptor || !instance)
			return ScriptCallResult::Rejected;

		switch (callback)
		{
			case ScriptCallback::OnCreate:
			{
				const auto function = descriptor->OnCreate;
				return function ? Call(site, [&]() { return function(instance); }) : ScriptCallResult::Unavailable;
			}
			case ScriptCallback::OnUpdate:
			{
				const auto function = descriptor->OnUpdate;
				return function ? Call(site, [&]() { return function(instance, argument); }) : ScriptCallResult::Unavailable;
			}
			case ScriptCallback::OnFixedUpdate:
			{
				const auto function = descriptor->OnFixedUpdate;
				return function ? Call(site, [&]() { return function(instance, argument); }) : ScriptCallResult::Unavailable;
			}
			case ScriptCallback::OnLateUpdate:
			{
				const auto function = descriptor->OnLateUpdate;
				return function ? Call(site, [&]() { return function(instance, argument); }) : ScriptCallResult::Unavailable;
			}
			case ScriptCallback::OnDestroy:
			{
				const auto function = descriptor->OnDestroy;
				return function ? Call(site, [&]() { return function(instance); }) : ScriptCallResult::Unavailable;
			}
			case ScriptCallback::OnReload:
			{
				const auto function = descriptor->OnReload;
				return function ? Call(site, [&]() { return function(instance); }) : ScriptCallResult::Unavailable;
			}
		}
		return ScriptCallResult::Rejected;
	}

	bool ScriptModule::ReadModuleDescription(std::string& outName, std::vector<ScriptClassInfo>& outClasses, std::vector<const StrataScriptClassDesc*>& outDescriptors,
		std::string& outError) const
	{
		const StrataScriptModuleAPI& api = m_API;
		if (api.StructSize < c_RequiredModuleAPISize || api.ABIVersion != ST_SCRIPT_ABI_VERSION)
		{
			outError = "the module description does not match this ABI version";
			return false;
		}
		// The name is optional (the file name stands in for it).
		if (!ReadScriptString(api.Name, outName) || outName.size() > c_MaxScriptNameSize)
		{
			outError = "the module has an invalid name";
			return false;
		}
		if (api.ClassCount > c_MaxScriptClasses || (api.ClassCount > 0 && !api.Classes))
		{
			outError = fmt::format("invalid class list ({} classes)", api.ClassCount);
			return false;
		}

		std::unordered_set<std::string> classNames;
		for (uint32_t classIndex = 0; classIndex < api.ClassCount; classIndex++)
		{
			const StrataScriptClassDesc* descriptor = api.Classes[classIndex];
			if (!descriptor || descriptor->StructSize < c_RequiredClassDescSize)
			{
				outError = fmt::format("class {} has an invalid descriptor", classIndex);
				return false;
			}

			ScriptClassInfo& info = outClasses.emplace_back();
			info.Index = classIndex;
			if (!ReadName(descriptor->Name, info.Name))
			{
				outError = fmt::format("class {} has an invalid name", classIndex);
				return false;
			}
			if (!classNames.insert(info.Name).second)
			{
				outError = fmt::format("class '{}' is registered twice", info.Name);
				return false;
			}
			if (!descriptor->Create || !descriptor->Destroy || !descriptor->GetField || !descriptor->SetField)
			{
				outError = fmt::format("class '{}' lacks its lifetime or field functions", info.Name);
				return false;
			}

			const std::pair<ScriptCallback, bool> callbacks[] = {
				{ ScriptCallback::OnCreate, descriptor->OnCreate != nullptr },
				{ ScriptCallback::OnUpdate, descriptor->OnUpdate != nullptr },
				{ ScriptCallback::OnFixedUpdate, descriptor->OnFixedUpdate != nullptr },
				{ ScriptCallback::OnLateUpdate, descriptor->OnLateUpdate != nullptr },
				{ ScriptCallback::OnDestroy, descriptor->OnDestroy != nullptr },
				{ ScriptCallback::OnReload, descriptor->OnReload != nullptr }
			};
			for (const auto& [callback, implemented] : callbacks)
			{
				if (implemented)
					info.Callbacks |= 1u << static_cast<uint32_t>(callback);
			}

			if (descriptor->FieldCount > c_MaxScriptFields || (descriptor->FieldCount > 0 && !descriptor->Fields))
			{
				outError = fmt::format("class '{}' has an invalid field list", info.Name);
				return false;
			}

			std::unordered_set<std::string> fieldNames;
			for (uint32_t fieldIndex = 0; fieldIndex < descriptor->FieldCount; fieldIndex++)
			{
				const StrataScriptFieldDesc* fieldDescriptor = descriptor->Fields[fieldIndex];
				if (!fieldDescriptor || fieldDescriptor->StructSize < c_RequiredFieldDescSize)
				{
					outError = fmt::format("field {} of class '{}' has an invalid descriptor", fieldIndex, info.Name);
					return false;
				}

				ScriptFieldInfo& field = info.Fields.emplace_back();
				if (!ReadName(fieldDescriptor->Name, field.Name))
				{
					outError = fmt::format("field {} of class '{}' has an invalid name", fieldIndex, info.Name);
					return false;
				}
				if (!fieldNames.insert(field.Name).second)
				{
					outError = fmt::format("class '{}' declares field '{}' twice", info.Name, field.Name);
					return false;
				}

				const std::optional<PropertyType> type = ScriptFieldTypeToPropertyType(fieldDescriptor->Type);
				if (!type)
				{
					outError = fmt::format("field '{}.{}' has an unsupported type ({})", info.Name, field.Name, fieldDescriptor->Type);
					return false;
				}
				field.Type = *type;

				std::optional<PropertyValue> defaultValue = ScriptValueToFieldValue(fieldDescriptor->DefaultValue, field.Type);
				if (!defaultValue)
				{
					outError = fmt::format("field '{}.{}' has a default value of the wrong type", info.Name, field.Name);
					return false;
				}
				field.DefaultValue = std::move(*defaultValue);
			}

			outDescriptors.push_back(descriptor);
		}
		return true;
	}

	void ScriptModule::ReportException(std::string_view message)
	{
		t_PendingException.assign(message.data(), message.size());
	}

	const ScriptCallSite* ScriptModule::GetCurrentCall()
	{
		return t_CurrentCall;
	}

}
