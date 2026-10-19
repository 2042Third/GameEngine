#include "stpch.h"
#include "Strata/Scripting/ScriptHostAPI.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Input/Input.h"
#include "Strata/Math/Math.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptModule.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "Strata/Scripting/ScriptValue.h"
#include "Strata/Scripting/ScriptingComponents.h"

#include <glm/gtc/type_ptr.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

namespace Strata
{

	namespace
	{

		// Contexts are ScriptSystem pointers. Only the owner thread registers them, and host functions check the thread
		// before looking at the set, so the set itself is only ever touched by that thread.
		std::unordered_set<const void*> s_Contexts;
		std::atomic<std::thread::id> s_OwnerThread;

		// Problems without a scene to report them to (an invalid context) are logged once per message.
		void ReportGlobalProblem(const std::string& message)
		{
			static std::mutex s_Mutex;
			static std::unordered_set<std::string> s_Reported;
			std::scoped_lock<std::mutex> lock(s_Mutex);
			if (s_Reported.size() < 256 && s_Reported.insert(message).second)
				Log::GetScriptLogger()->error("{}", message);
		}

		// Resolves a context to its scene's script system, or null (logging why) if the call is not allowed: invalid or
		// stale context, wrong thread, no script call in progress, or a crashed module.
		ScriptSystem* ResolveContext(StrataScriptContext* context, const char* function)
		{
			if (!context)
			{
				ReportGlobalProblem(fmt::format("{}: scripts can only use the engine from within their callbacks (no scene context)", function));
				return nullptr;
			}
			if (std::this_thread::get_id() != s_OwnerThread.load())
			{
				ReportGlobalProblem(fmt::format("{}: scripts can only use the engine from the main thread", function));
				return nullptr;
			}
			if (!s_Contexts.count(context))
			{
				ReportGlobalProblem(fmt::format("{}: invalid or expired scene context", function));
				return nullptr;
			}
			if (!ScriptModule::GetCurrentCall())
			{
				ReportGlobalProblem(fmt::format("{}: scripts can only use the engine while the engine is calling them", function));
				return nullptr;
			}

			ScriptSystem* system = reinterpret_cast<ScriptSystem*>(context);
			ScriptEngine* engine = system->GetEngine();
			// A module that crashed may still be running code further up the stack; it gets nothing from the engine.
			if (!engine || !engine->GetModule() || engine->GetModule()->IsFaulted())
				return nullptr;
			return system;
		}

		// Runs a host function body. Exceptions (only possible from allocation failures or third-party code) must not
		// unwind into the module, so they are logged and the function returns its failure value.
		template<typename Result, typename Body>
		Result HostCall(const char* function, Result failure, Body&& body) noexcept
		{
			try
			{
				return body();
			}
			catch (const std::exception& exception)
			{
				ReportGlobalProblem(fmt::format("{}: internal error: {}", function, exception.what()));
			}
			catch (...)
			{
				ReportGlobalProblem(fmt::format("{}: internal error", function));
			}
			return failure;
		}

		template<typename Body>
		void HostCallVoid(const char* function, Body&& body) noexcept
		{
			HostCall<bool>(function, false, [&]()
			{
				body();
				return true;
			});
		}

		Entity FindEntity(ScriptSystem& system, StrataScriptEntityID id)
		{
			return id != 0 ? system.GetScene().GetEntityByUUID(UUID(id)) : Entity();
		}

		// The entity a script names, reporting when it does not exist.
		Entity RequireEntity(ScriptSystem& system, StrataScriptEntityID id, const char* function)
		{
			Entity entity = FindEntity(system, id);
			if (!entity)
				system.ReportProblem(function, id == 0 ? std::string("the entity is null") : fmt::format("entity {} does not exist", UUID(id).ToString()));
			return entity;
		}

		uint64_t CopyText(std::string_view text, char* buffer, uint64_t capacity)
		{
			if (buffer && capacity > 0)
				std::memcpy(buffer, text.data(), static_cast<size_t>(std::min<uint64_t>(capacity, text.size())));
			return text.size();
		}

		uint32_t CopyEntityIDs(const std::vector<Entity>& entities, StrataScriptEntityID* buffer, uint32_t capacity)
		{
			if (buffer)
			{
				const size_t count = std::min<size_t>(capacity, entities.size());
				for (size_t index = 0; index < count; index++)
					buffer[index] = static_cast<uint64_t>(entities[index].GetUUID());
			}
			return static_cast<uint32_t>(entities.size());
		}

		uint32_t CopyEntityIDs(const std::vector<UUID>& ids, StrataScriptEntityID* buffer, uint32_t capacity)
		{
			if (buffer)
			{
				const size_t count = std::min<size_t>(capacity, ids.size());
				for (size_t index = 0; index < count; index++)
					buffer[index] = static_cast<uint64_t>(ids[index]);
			}
			return static_cast<uint32_t>(ids.size());
		}

		const ComponentInfo* RequireComponentInfo(ScriptSystem& system, const StrataScriptString& component, const char* function)
		{
			const std::string_view name = ScriptStringView(component);
			const ComponentInfo* info = ComponentRegistry::Find(name);
			if (!info)
				system.ReportProblem(function, fmt::format("there is no component named '{}'", name));
			return info;
		}

		bool IsFinite(const float* values, size_t count)
		{
			for (size_t index = 0; index < count; index++)
			{
				if (!std::isfinite(values[index]))
					return false;
			}
			return true;
		}

		void WriteVector3(const glm::vec3& value, float out[3])
		{
			out[0] = value.x;
			out[1] = value.y;
			out[2] = value.z;
		}

		// Reads a vector a script passes, reporting a missing or non-finite one (`what` names it in the message).
		std::optional<glm::vec3> ReadVector3(ScriptSystem& system, const float* values, const char* what, const char* function)
		{
			if (!values)
			{
				system.ReportProblem(function, fmt::format("the {} pointer is null", what));
				return std::nullopt;
			}
			if (!IsFinite(values, 3))
			{
				system.ReportProblem(function, fmt::format("the {} must be finite", what));
				return std::nullopt;
			}
			return glm::vec3(values[0], values[1], values[2]);
		}

		// Reads a rotation (x, y, z, w) a script passes and normalizes it, reporting a missing, non-finite or zero one.
		std::optional<glm::quat> ReadRotation(ScriptSystem& system, const float* values, const char* function)
		{
			if (!values)
			{
				system.ReportProblem(function, "the rotation pointer is null");
				return std::nullopt;
			}
			const glm::quat value(values[3], values[0], values[1], values[2]);
			const float length = glm::length(value);
			if (!IsFinite(values, 4) || !(length > 1e-6f))
			{
				system.ReportProblem(function, "the rotation must be a finite, non-zero quaternion");
				return std::nullopt;
			}
			return value / length;
		}

		void WriteTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale, StrataScriptTransform& out)
		{
			for (int index = 0; index < 3; index++)
			{
				out.Translation[index] = translation[index];
				out.Scale[index] = scale[index];
			}
			out.Rotation[0] = rotation.x;
			out.Rotation[1] = rotation.y;
			out.Rotation[2] = rotation.z;
			out.Rotation[3] = rotation.w;
		}

		// Validates the parts of a script-provided transform that will be written; normalizes the rotation.
		bool ReadTransform(ScriptSystem& system, const StrataScriptTransform& transform, uint32_t parts, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale,
			const char* function)
		{
			if ((parts & StrataScriptTransformPart_Translation) && !IsFinite(transform.Translation, 3))
			{
				system.ReportProblem(function, "the translation must be finite");
				return false;
			}
			if ((parts & StrataScriptTransformPart_Scale) && !IsFinite(transform.Scale, 3))
			{
				system.ReportProblem(function, "the scale must be finite");
				return false;
			}
			if (parts & StrataScriptTransformPart_Rotation)
			{
				const std::optional<glm::quat> value = ReadRotation(system, transform.Rotation, function);
				if (!value)
					return false;
				rotation = *value;
			}
			if (parts & StrataScriptTransformPart_Translation)
				translation = glm::vec3(transform.Translation[0], transform.Translation[1], transform.Translation[2]);
			if (parts & StrataScriptTransformPart_Scale)
				scale = glm::vec3(transform.Scale[0], transform.Scale[1], transform.Scale[2]);
			return true;
		}

		AssetManagerBase* RequireAssetManager(ScriptSystem& system, const char* function)
		{
			AssetManagerBase* manager = AssetManager::GetActive().get();
			if (!manager)
				system.ReportProblem(function, "no asset manager is active");
			return manager;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Diagnostics
		////////////////////////////////////////////////////////////////////////////////

		void HostLog(uint32_t level, StrataScriptString message)
		{
			HostCallVoid("Log", [&]()
			{
				const std::string_view text = ScriptStringView(message);
				const Ref<spdlog::logger>& logger = Log::GetScriptLogger();
				switch (level)
				{
					case StrataScriptLogLevel_Trace: logger->trace("{}", text); break;
					case StrataScriptLogLevel_Info:  logger->info("{}", text); break;
					case StrataScriptLogLevel_Warn:  logger->warn("{}", text); break;
					default:                         logger->error("{}", text); break;
				}
			});
		}

		void HostReportException(StrataScriptString message)
		{
			HostCallVoid("ReportException", [&]() { ScriptModule::ReportException(ScriptStringView(message)); });
		}

		////////////////////////////////////////////////////////////////////////////////
		// Time
		////////////////////////////////////////////////////////////////////////////////

		float HostGetDeltaTime(StrataScriptContext* context)
		{
			return HostCall("GetDeltaTime", 0.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetDeltaTime");
				return system ? system->GetDeltaTime() : 0.0f;
			});
		}

		float HostGetFixedDeltaTime(StrataScriptContext* context)
		{
			return HostCall("GetFixedDeltaTime", 0.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetFixedDeltaTime");
				if (!system)
					return 0.0f;
				const float fixedTimestep = system->GetScene().GetSettings().FixedTimestep;
				return fixedTimestep > 0.0f ? fixedTimestep : 1.0f / 60.0f;
			});
		}

		double HostGetElapsedTime(StrataScriptContext* context)
		{
			return HostCall("GetElapsedTime", 0.0, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetElapsedTime");
				return system ? system->GetScene().GetTime() : 0.0;
			});
		}

		uint64_t HostGetFrameIndex(StrataScriptContext* context)
		{
			return HostCall("GetFrameIndex", uint64_t(0), [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetFrameIndex");
				return system ? system->GetScene().GetFrameIndex() : uint64_t(0);
			});
		}

		float HostGetTimeScale(StrataScriptContext* context)
		{
			return HostCall("GetTimeScale", 1.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetTimeScale");
				return system ? system->GetScene().GetTimeScale() : 1.0f;
			});
		}

		void HostSetTimeScale(StrataScriptContext* context, float timeScale)
		{
			HostCallVoid("SetTimeScale", [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetTimeScale");
				if (!system)
					return;
				if (!std::isfinite(timeScale) || timeScale < 0.0f)
				{
					system->ReportProblem("SetTimeScale", fmt::format("the time scale must be a finite, non-negative number (got {})", timeScale));
					return;
				}
				system->GetScene().SetTimeScale(timeScale);
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Entities
		////////////////////////////////////////////////////////////////////////////////

		StrataScriptEntityID HostCreateEntity(StrataScriptContext* context, StrataScriptString name, StrataScriptEntityID parent)
		{
			return HostCall("CreateEntity", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "CreateEntity");
				if (!system)
					return 0;
				Entity parentEntity;
				if (parent != 0 && !(parentEntity = RequireEntity(*system, parent, "CreateEntity")))
					return 0;
				const Entity entity = system->GetScene().CreateChildEntity(parentEntity, std::string(ScriptStringView(name)));
				if (!entity)
				{
					system->ReportProblem("CreateEntity", fmt::format("the scene already holds the maximum of {} entities", Scene::c_MaxEntities));
					return 0;
				}
				return static_cast<uint64_t>(entity.GetUUID());
			});
		}

		void HostDestroyEntity(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			HostCallVoid("DestroyEntity", [&]()
			{
				ScriptSystem* system = ResolveContext(context, "DestroyEntity");
				if (!system)
					return;
				if (const Entity target = RequireEntity(*system, entity, "DestroyEntity"))
					system->DestroyEntity(target);
			});
		}

		bool HostIsEntityValid(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return HostCall("IsEntityValid", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "IsEntityValid");
				return system && FindEntity(*system, entity).IsValid();
			});
		}

		StrataScriptEntityID HostFindEntityByName(StrataScriptContext* context, StrataScriptString name)
		{
			return HostCall("FindEntityByName", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "FindEntityByName");
				if (!system)
					return 0;
				const Entity entity = system->GetScene().FindEntityByName(ScriptStringView(name));
				return entity ? static_cast<uint64_t>(entity.GetUUID()) : 0;
			});
		}

		uint32_t HostFindEntitiesByTag(StrataScriptContext* context, StrataScriptString tag, StrataScriptEntityID* outEntities, uint32_t capacity)
		{
			return HostCall("FindEntitiesByTag", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "FindEntitiesByTag");
				if (!system)
					return 0;
				return CopyEntityIDs(system->GetScene().FindEntitiesByTag(ScriptStringView(tag)), outEntities, capacity);
			});
		}

		uint64_t HostGetEntityName(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity)
		{
			return HostCall("GetEntityName", uint64_t(0), [&]() -> uint64_t
			{
				ScriptSystem* system = ResolveContext(context, "GetEntityName");
				const Entity target = system ? RequireEntity(*system, entity, "GetEntityName") : Entity();
				return target ? CopyText(target.GetName(), buffer, capacity) : 0;
			});
		}

		bool HostSetEntityName(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString name)
		{
			return HostCall("SetEntityName", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetEntityName");
				Entity target = system ? RequireEntity(*system, entity, "SetEntityName") : Entity();
				if (!target)
					return false;
				target.GetComponent<NameComponent>().Name = std::string(ScriptStringView(name));
				target.MarkModified<NameComponent>();
				return true;
			});
		}

		uint64_t HostGetEntityTag(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity)
		{
			return HostCall("GetEntityTag", uint64_t(0), [&]() -> uint64_t
			{
				ScriptSystem* system = ResolveContext(context, "GetEntityTag");
				const Entity target = system ? RequireEntity(*system, entity, "GetEntityTag") : Entity();
				const TagComponent* tag = target.TryGetComponent<TagComponent>();
				return tag ? CopyText(tag->Tag, buffer, capacity) : 0;
			});
		}

		bool HostSetEntityTag(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString tag)
		{
			return HostCall("SetEntityTag", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetEntityTag");
				Entity target = system ? RequireEntity(*system, entity, "SetEntityTag") : Entity();
				if (!target)
					return false;

				const std::string_view text = ScriptStringView(tag);
				if (text.empty())
				{
					if (target.HasComponent<TagComponent>())
						target.RemoveComponent<TagComponent>();
					return true;
				}
				if (TagComponent* existing = target.TryGetComponent<TagComponent>())
				{
					existing->Tag = std::string(text);
					target.MarkModified<TagComponent>();
				}
				else
				{
					target.AddComponent<TagComponent>(std::string(text));
				}
				return true;
			});
		}

		bool HostIsEntityActive(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return HostCall("IsEntityActive", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "IsEntityActive");
				const Entity target = system ? RequireEntity(*system, entity, "IsEntityActive") : Entity();
				return target && target.IsActive();
			});
		}

		bool HostIsEntityActiveInHierarchy(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return HostCall("IsEntityActiveInHierarchy", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "IsEntityActiveInHierarchy");
				const Entity target = system ? RequireEntity(*system, entity, "IsEntityActiveInHierarchy") : Entity();
				return target && system->GetScene().IsActiveInHierarchy(target);
			});
		}

		bool HostSetEntityActive(StrataScriptContext* context, StrataScriptEntityID entity, bool active)
		{
			return HostCall("SetEntityActive", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetEntityActive");
				Entity target = system ? RequireEntity(*system, entity, "SetEntityActive") : Entity();
				if (!target)
					return false;
				target.SetActive(active);
				return true;
			});
		}

		StrataScriptEntityID HostGetParent(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return HostCall("GetParent", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "GetParent");
				const Entity target = system ? RequireEntity(*system, entity, "GetParent") : Entity();
				const Entity parent = target.GetParent();
				return parent ? static_cast<uint64_t>(parent.GetUUID()) : 0;
			});
		}

		bool HostSetParent(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptEntityID parent, bool keepWorldTransform)
		{
			return HostCall("SetParent", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetParent");
				const Entity target = system ? RequireEntity(*system, entity, "SetParent") : Entity();
				if (!target)
					return false;
				Entity parentEntity;
				if (parent != 0 && !(parentEntity = RequireEntity(*system, parent, "SetParent")))
					return false;
				if (!system->GetScene().SetParent(target, parentEntity, keepWorldTransform))
				{
					system->ReportProblem("SetParent", fmt::format("'{}' cannot become a child of '{}' (that would create a cycle)", target.GetName(), parentEntity.GetName()));
					return false;
				}
				return true;
			});
		}

		uint32_t HostGetChildren(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptEntityID* outChildren, uint32_t capacity)
		{
			return HostCall("GetChildren", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "GetChildren");
				const Entity target = system ? RequireEntity(*system, entity, "GetChildren") : Entity();
				return target ? CopyEntityIDs(target.GetChildren(), outChildren, capacity) : 0;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Components
		////////////////////////////////////////////////////////////////////////////////

		bool HostHasComponent(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component)
		{
			return HostCall("HasComponent", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "HasComponent");
				const Entity target = system ? RequireEntity(*system, entity, "HasComponent") : Entity();
				const ComponentInfo* info = target ? RequireComponentInfo(*system, component, "HasComponent") : nullptr;
				return info && info->Has(system->GetScene().GetRegistry(), target.GetHandle());
			});
		}

		bool HostAddComponent(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component)
		{
			return HostCall("AddComponent", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AddComponent");
				const Entity target = system ? RequireEntity(*system, entity, "AddComponent") : Entity();
				const ComponentInfo* info = target ? RequireComponentInfo(*system, component, "AddComponent") : nullptr;
				if (!info)
					return false;
				std::string error;
				if (!ComponentAccess::AddComponent(target, *info, &error))
				{
					system->ReportProblem("AddComponent", error);
					return false;
				}
				return true;
			});
		}

		bool HostRemoveComponent(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component)
		{
			return HostCall("RemoveComponent", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "RemoveComponent");
				const Entity target = system ? RequireEntity(*system, entity, "RemoveComponent") : Entity();
				const ComponentInfo* info = target ? RequireComponentInfo(*system, component, "RemoveComponent") : nullptr;
				if (!info)
					return false;
				std::string error;
				if (!ComponentAccess::RemoveComponent(target, *info, &error))
				{
					system->ReportProblem("RemoveComponent", error);
					return false;
				}
				return true;
			});
		}

		// The component and property a script names, reporting what is missing.
		struct PropertyTarget
		{
			Entity Target;
			const ComponentInfo* Component = nullptr;
			const PropertyInfo* Property = nullptr;
		};

		std::optional<PropertyTarget> ResolveProperty(ScriptSystem& system, StrataScriptEntityID entity, const StrataScriptString& component,
			const StrataScriptString& property, const char* function)
		{
			const Entity target = RequireEntity(system, entity, function);
			const ComponentInfo* info = target ? RequireComponentInfo(system, component, function) : nullptr;
			if (!info)
				return std::nullopt;

			const PropertyInfo* propertyInfo = info->FindProperty(ScriptStringView(property));
			if (!propertyInfo)
			{
				system.ReportProblem(function, fmt::format("component '{}' has no property '{}'", info->Name, ScriptStringView(property)));
				return std::nullopt;
			}
			if (!info->Has(system.GetScene().GetRegistry(), target.GetHandle()))
			{
				system.ReportProblem(function, fmt::format("entity '{}' has no {} component", target.GetName(), info->Name));
				return std::nullopt;
			}
			return PropertyTarget { target, info, propertyInfo };
		}

		bool HostGetProperty(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component, StrataScriptString property,
			StrataScriptValue* outValue, char* stringBuffer, uint64_t stringCapacity)
		{
			return HostCall("GetProperty", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetProperty");
				if (!system || !outValue)
					return false;
				const std::optional<PropertyTarget> target = ResolveProperty(*system, entity, component, property, "GetProperty");
				if (!target)
					return false;

				const std::optional<PropertyValue> value = ComponentAccess::GetProperty(target->Target, *target->Component, *target->Property);
				if (!value)
					return false;

				StrataScriptValue result = PropertyValueToScriptValue(*value, target->Property->Type);
				if (result.Type == StrataScriptValueType_String)
				{
					// The text goes into the caller's buffer; the size tells it whether the buffer was large enough.
					const std::string& text = std::get<std::string>(*value);
					CopyText(text, stringBuffer, stringCapacity);
					result.As.String = StrataScriptString { stringBuffer, static_cast<uint64_t>(text.size()) };
				}
				*outValue = result;
				return result.Type != StrataScriptValueType_Empty;
			});
		}

		bool HostSetProperty(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString component, StrataScriptString property,
			const StrataScriptValue* value)
		{
			return HostCall("SetProperty", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetProperty");
				if (!system || !value)
					return false;
				const std::optional<PropertyTarget> target = ResolveProperty(*system, entity, component, property, "SetProperty");
				if (!target)
					return false;

				std::string error;
				const std::optional<PropertyValue> converted = ScriptValueToPropertyValue(*value, *target->Property, &error);
				if (!converted || !ComponentAccess::SetProperty(target->Target, *target->Component, *target->Property, *converted, &error))
				{
					system->ReportProblem("SetProperty", fmt::format("{}.{}: {}", target->Component->Name, target->Property->Name, error));
					return false;
				}
				return true;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Transform
		////////////////////////////////////////////////////////////////////////////////

		bool HostGetTransform(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform)
		{
			return HostCall("GetTransform", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetTransform");
				const Entity target = system && outTransform ? RequireEntity(*system, entity, "GetTransform") : Entity();
				if (!target)
					return false;
				const TransformComponent& transform = target.GetComponent<TransformComponent>();
				WriteTransform(transform.Translation, transform.Rotation, transform.Scale, *outTransform);
				return true;
			});
		}

		bool HostSetTransform(StrataScriptContext* context, StrataScriptEntityID entity, const StrataScriptTransform* transform, uint32_t parts)
		{
			return HostCall("SetTransform", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetTransform");
				Entity target = system && transform ? RequireEntity(*system, entity, "SetTransform") : Entity();
				if (!target)
					return false;

				TransformComponent& component = target.GetComponent<TransformComponent>();
				glm::vec3 translation = component.Translation;
				glm::quat rotation = component.Rotation;
				glm::vec3 scale = component.Scale;
				if (!ReadTransform(*system, *transform, parts, translation, rotation, scale, "SetTransform"))
					return false;
				component.Translation = translation;
				component.Rotation = rotation;
				component.Scale = scale;
				target.MarkModified<TransformComponent>();
				return true;
			});
		}

		bool HostGetWorldTransform(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptTransform* outTransform)
		{
			return HostCall("GetWorldTransform", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetWorldTransform");
				const Entity target = system && outTransform ? RequireEntity(*system, entity, "GetWorldTransform") : Entity();
				if (!target)
					return false;

				glm::vec3 translation, scale;
				glm::quat rotation;
				if (!Math::DecomposeTransform(system->GetScene().GetWorldTransform(target), translation, rotation, scale))
				{
					system->ReportProblem("GetWorldTransform", fmt::format("the world transform of '{}' is degenerate", target.GetName()));
					return false;
				}
				WriteTransform(translation, rotation, scale, *outTransform);
				return true;
			});
		}

		bool HostSetWorldTransform(StrataScriptContext* context, StrataScriptEntityID entity, const StrataScriptTransform* transform, uint32_t parts)
		{
			return HostCall("SetWorldTransform", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetWorldTransform");
				Entity target = system && transform ? RequireEntity(*system, entity, "SetWorldTransform") : Entity();
				if (!target)
					return false;

				Scene& scene = system->GetScene();
				glm::vec3 translation, scale;
				glm::quat rotation;
				if (!Math::DecomposeTransform(scene.GetWorldTransform(target), translation, rotation, scale))
				{
					translation = glm::vec3(0.0f);
					rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
					scale = glm::vec3(1.0f);
				}
				if (!ReadTransform(*system, *transform, parts, translation, rotation, scale, "SetWorldTransform"))
					return false;
				if (!scene.SetWorldTransform(target, Math::ComposeTransform(translation, rotation, scale)))
				{
					system->ReportProblem("SetWorldTransform", fmt::format("the world transform of '{}' cannot be represented under its parent", target.GetName()));
					return false;
				}
				target.MarkModified<TransformComponent>();
				return true;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Scene
		////////////////////////////////////////////////////////////////////////////////

		StrataScriptEntityID HostGetPrimaryCamera(StrataScriptContext* context)
		{
			return HostCall("GetPrimaryCamera", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "GetPrimaryCamera");
				if (!system)
					return 0;
				const Entity camera = system->GetScene().GetPrimaryCameraEntity();
				return camera ? static_cast<uint64_t>(camera.GetUUID()) : 0;
			});
		}

		uint32_t HostGetRootEntities(StrataScriptContext* context, StrataScriptEntityID* outEntities, uint32_t capacity)
		{
			return HostCall("GetRootEntities", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "GetRootEntities");
				return system ? CopyEntityIDs(system->GetScene().GetRootEntities(), outEntities, capacity) : 0;
			});
		}

		StrataScriptEntityID HostInstantiate(StrataScriptContext* context, StrataScriptAssetHandle asset, StrataScriptEntityID parent, const StrataScriptTransform* transform)
		{
			return HostCall("Instantiate", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "Instantiate");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "Instantiate") : nullptr;
				if (!manager)
					return 0;

				const AssetHandle handle(asset);
				const AssetType type = manager->GetAssetType(handle);
				if (type != AssetType::Prefab && type != AssetType::Model)
				{
					system->ReportProblem("Instantiate", asset == 0 ? std::string("the asset is null")
						: fmt::format("asset {} is not a prefab or model", handle.ToString()));
					return 0;
				}

				Entity parentEntity;
				if (parent != 0 && !(parentEntity = RequireEntity(*system, parent, "Instantiate")))
					return 0;

				glm::vec3 translation(0.0f), scale(1.0f);
				glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
				if (transform && !ReadTransform(*system, *transform, StrataScriptTransformPart_All, translation, rotation, scale, "Instantiate"))
					return 0;

				// Never waits for loading (script code runs on the main thread): an asset that is not loaded yet is requested and
				// the call fails, so scripts request assets early and instantiate them once they are loaded.
				const Ref<Asset> loaded = manager->GetAsset(handle, AssetPriority::High);
				if (!loaded)
				{
					if (manager->GetAssetState(handle) == AssetState::Failed)
						system->ReportProblem("Instantiate", fmt::format("asset {} failed to load: {}", handle.ToString(), manager->GetAssetError(handle)));
					else
						system->ReportProblem("Instantiate", fmt::format("asset {} is not loaded yet (its load was started); request assets early "
							"(Assets::RequestLoad) and instantiate them once Assets::IsLoaded is true", handle.ToString()));
					return 0;
				}
				if (loaded->GetType() != AssetType::Prefab && loaded->GetType() != AssetType::Model)
				{
					system->ReportProblem("Instantiate", fmt::format("asset {} is not a prefab or model", handle.ToString()));
					return 0;
				}

				std::string error;
				const std::vector<Entity> roots = std::static_pointer_cast<EntityTemplate>(loaded)->Instantiate(system->GetScene(), parentEntity, &error);
				if (roots.empty())
				{
					system->ReportProblem("Instantiate", error.empty() ? fmt::format("asset {} contains no entities", handle.ToString())
						: fmt::format("asset {} could not be instantiated: {}", handle.ToString(), error));
					return 0;
				}

				Entity root = roots.front();
				if (transform)
				{
					TransformComponent& rootTransform = root.GetComponent<TransformComponent>();
					rootTransform.Translation = translation;
					rootTransform.Rotation = rotation;
					rootTransform.Scale = scale;
					root.MarkModified<TransformComponent>();
				}

				// Scripts of the new entities exist when this returns, so the caller can configure them right away.
				system->CreatePendingInstances();
				return static_cast<uint64_t>(root.GetUUID());
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Assets
		////////////////////////////////////////////////////////////////////////////////

		StrataScriptAssetHandle HostFindAsset(StrataScriptContext* context, StrataScriptString path)
		{
			return HostCall("FindAsset", StrataScriptAssetHandle(0), [&]() -> StrataScriptAssetHandle
			{
				ScriptSystem* system = ResolveContext(context, "FindAsset");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "FindAsset") : nullptr;
				if (!manager)
					return 0;
				const AssetHandle handle = manager->FindAssetByPath(ScriptStringView(path));
				if (!handle.IsValid())
					system->ReportProblem("FindAsset", fmt::format("there is no asset at '{}'", ScriptStringView(path)));
				return static_cast<uint64_t>(handle);
			});
		}

		bool HostIsAssetLoaded(StrataScriptContext* context, StrataScriptAssetHandle asset)
		{
			return HostCall("IsAssetLoaded", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "IsAssetLoaded");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "IsAssetLoaded") : nullptr;
				return manager && manager->GetAssetState(AssetHandle(asset)) == AssetState::Ready;
			});
		}

		bool HostRequestAssetLoad(StrataScriptContext* context, StrataScriptAssetHandle asset)
		{
			return HostCall("RequestAssetLoad", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "RequestAssetLoad");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "RequestAssetLoad") : nullptr;
				if (!manager)
					return false;
				const AssetHandle handle(asset);
				// Pinned for the playing scene until ReleaseAsset or the end of play.
				if (!system->RequestAsset(*manager, handle))
				{
					system->ReportProblem("RequestAssetLoad", fmt::format("asset {} does not exist", handle.ToString()));
					return false;
				}
				return true;
			});
		}

		bool HostReleaseAsset(StrataScriptContext* context, StrataScriptAssetHandle asset)
		{
			return HostCall("ReleaseAsset", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "ReleaseAsset");
				// Releasing an asset the scene does not hold is no misuse (e.g. a script releasing twice): just false.
				return system && system->ReleaseAsset(AssetHandle(asset));
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Scripts
		////////////////////////////////////////////////////////////////////////////////

		StrataScriptInstance HostGetScriptInstance(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className)
		{
			return HostCall("GetScriptInstance", StrataScriptInstance(nullptr), [&]() -> StrataScriptInstance
			{
				ScriptSystem* system = ResolveContext(context, "GetScriptInstance");
				const Entity target = system ? FindEntity(*system, entity) : Entity();
				return target ? system->GetInstanceHandle(target, ScriptStringView(className)) : nullptr;
			});
		}

		bool HostHasScript(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className)
		{
			return HostCall("HasScript", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "HasScript");
				const Entity target = system ? FindEntity(*system, entity) : Entity();
				const ScriptComponent* component = target.TryGetComponent<ScriptComponent>();
				return component && component->FindScript(ScriptStringView(className));
			});
		}

		bool HostAddScript(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className)
		{
			return HostCall("AddScript", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AddScript");
				const Entity target = system ? RequireEntity(*system, entity, "AddScript") : Entity();
				if (!target)
					return false;
				std::string error;
				if (!system->AddScript(target, ScriptStringView(className), &error))
				{
					system->ReportProblem("AddScript", error);
					return false;
				}
				return true;
			});
		}

		bool HostRemoveScript(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString className)
		{
			return HostCall("RemoveScript", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "RemoveScript");
				const Entity target = system ? RequireEntity(*system, entity, "RemoveScript") : Entity();
				return target && system->RemoveScript(target, ScriptStringView(className));
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Input
		////////////////////////////////////////////////////////////////////////////////

		template<typename Query>
		bool QueryKey(StrataScriptContext* context, uint32_t key, const char* function, Query&& query)
		{
			return HostCall(function, false, [&]()
			{
				return ResolveContext(context, function) && key < c_MaxKeyCode && query(static_cast<KeyCode>(key));
			});
		}

		template<typename Query>
		bool QueryMouseButton(StrataScriptContext* context, uint32_t button, const char* function, Query&& query)
		{
			return HostCall(function, false, [&]()
			{
				return ResolveContext(context, function) && button < c_MaxMouseButtons && query(static_cast<MouseCode>(button));
			});
		}

		bool HostIsKeyDown(StrataScriptContext* context, uint32_t key)
		{
			return QueryKey(context, key, "IsKeyDown", [](KeyCode code) { return Input::IsKeyDown(code); });
		}

		bool HostIsKeyPressed(StrataScriptContext* context, uint32_t key)
		{
			return QueryKey(context, key, "IsKeyPressed", [](KeyCode code) { return Input::IsKeyPressed(code); });
		}

		bool HostIsKeyReleased(StrataScriptContext* context, uint32_t key)
		{
			return QueryKey(context, key, "IsKeyReleased", [](KeyCode code) { return Input::IsKeyReleased(code); });
		}

		bool HostIsMouseButtonDown(StrataScriptContext* context, uint32_t button)
		{
			return QueryMouseButton(context, button, "IsMouseButtonDown", [](MouseCode code) { return Input::IsMouseButtonDown(code); });
		}

		bool HostIsMouseButtonPressed(StrataScriptContext* context, uint32_t button)
		{
			return QueryMouseButton(context, button, "IsMouseButtonPressed", [](MouseCode code) { return Input::IsMouseButtonPressed(code); });
		}

		bool HostIsMouseButtonReleased(StrataScriptContext* context, uint32_t button)
		{
			return QueryMouseButton(context, button, "IsMouseButtonReleased", [](MouseCode code) { return Input::IsMouseButtonReleased(code); });
		}

		void WriteVector2(const glm::vec2& value, float out[2])
		{
			out[0] = value.x;
			out[1] = value.y;
		}

		template<typename Query>
		void QueryVector2(StrataScriptContext* context, float out[2], const char* function, Query&& query)
		{
			HostCallVoid(function, [&]()
			{
				if (!out)
					return;
				WriteVector2(ResolveContext(context, function) ? query() : glm::vec2(0.0f), out);
			});
		}

		void HostGetMousePosition(StrataScriptContext* context, float outPosition[2])
		{
			QueryVector2(context, outPosition, "GetMousePosition", []() { return Input::GetMousePosition(); });
		}

		void HostGetMouseDelta(StrataScriptContext* context, float outDelta[2])
		{
			QueryVector2(context, outDelta, "GetMouseDelta", []() { return Input::GetMouseDelta(); });
		}

		void HostGetScrollDelta(StrataScriptContext* context, float outDelta[2])
		{
			QueryVector2(context, outDelta, "GetScrollDelta", []() { return Input::GetScrollDelta(); });
		}

		bool HostIsGamepadConnected(StrataScriptContext* context, uint32_t gamepad)
		{
			return HostCall("IsGamepadConnected", false, [&]()
			{
				return ResolveContext(context, "IsGamepadConnected") && gamepad < c_MaxGamepads && Input::IsGamepadConnected(gamepad);
			});
		}

		template<typename Query>
		bool QueryGamepadButton(StrataScriptContext* context, uint32_t gamepad, uint32_t button, const char* function, Query&& query)
		{
			return HostCall(function, false, [&]()
			{
				return ResolveContext(context, function) && gamepad < c_MaxGamepads && button < c_GamepadButtonCount && query(gamepad, static_cast<GamepadButton>(button));
			});
		}

		bool HostIsGamepadButtonDown(StrataScriptContext* context, uint32_t gamepad, uint32_t button)
		{
			return QueryGamepadButton(context, gamepad, button, "IsGamepadButtonDown", [](uint32_t pad, GamepadButton code) { return Input::IsGamepadButtonDown(pad, code); });
		}

		bool HostIsGamepadButtonPressed(StrataScriptContext* context, uint32_t gamepad, uint32_t button)
		{
			return QueryGamepadButton(context, gamepad, button, "IsGamepadButtonPressed",
				[](uint32_t pad, GamepadButton code) { return Input::IsGamepadButtonPressed(pad, code); });
		}

		bool HostIsGamepadButtonReleased(StrataScriptContext* context, uint32_t gamepad, uint32_t button)
		{
			return QueryGamepadButton(context, gamepad, button, "IsGamepadButtonReleased",
				[](uint32_t pad, GamepadButton code) { return Input::IsGamepadButtonReleased(pad, code); });
		}

		float HostGetGamepadAxis(StrataScriptContext* context, uint32_t gamepad, uint32_t axis)
		{
			return HostCall("GetGamepadAxis", 0.0f, [&]()
			{
				if (!ResolveContext(context, "GetGamepadAxis") || gamepad >= c_MaxGamepads || axis >= c_GamepadAxisCount)
					return 0.0f;
				return Input::GetGamepadAxis(gamepad, static_cast<GamepadAxis>(axis));
			});
		}

		static_assert(StrataScriptCursorMode_Normal == static_cast<uint32_t>(CursorMode::Normal) && StrataScriptCursorMode_Hidden == static_cast<uint32_t>(CursorMode::Hidden)
			&& StrataScriptCursorMode_Locked == static_cast<uint32_t>(CursorMode::Locked), "The cursor modes are the ABI's");

		void HostSetCursorMode(StrataScriptContext* context, uint32_t mode)
		{
			HostCallVoid("SetCursorMode", [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetCursorMode");
				if (!system)
					return;
				if (mode > StrataScriptCursorMode_Locked)
				{
					system->ReportProblem("SetCursorMode", fmt::format("{} is no cursor mode (0 normal, 1 hidden, 2 locked)", mode));
					return;
				}
				Input::SetGameCursorMode(static_cast<CursorMode>(mode));
			});
		}

		uint32_t HostGetCursorMode(StrataScriptContext* context)
		{
			return HostCall("GetCursorMode", uint32_t(StrataScriptCursorMode_Normal), [&]() -> uint32_t
			{
				return ResolveContext(context, "GetCursorMode") ? static_cast<uint32_t>(Input::GetGameCursorMode()) : uint32_t(StrataScriptCursorMode_Normal);
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Physics
		////////////////////////////////////////////////////////////////////////////////

		// What scripts are told when a scene plays without a module's system: the engine has no such module (a game built
		// without it, or one that does not register it). Their calls fail with their failure values.
		constexpr const char* c_WithoutPhysics = "the scene plays without physics (the engine has no physics module)";
		constexpr const char* c_WithoutAudio = "the scene plays without audio (the engine has no audio module)";
		constexpr const char* c_WithoutAnimation = "the scene plays without animation (the engine has no animation module)";

		// The scene's physics services (Scene/SceneServices.h); null when the scene plays without physics.
		IBodyMotionQuery* GetBodyMotion(ScriptSystem& system)
		{
			return system.GetScene().GetService<IBodyMotionQuery>();
		}

		IBodyControl* RequireBodyControl(ScriptSystem& system, const char* function)
		{
			IBodyControl* control = system.GetScene().GetService<IBodyControl>();
			if (!control)
				system.ReportProblem(function, c_WithoutPhysics);
			return control;
		}

		// The scene's physics queries, reporting when the scene plays without physics. A scene with physics but no world yet
		// (no physics component so far) has them: its queries find nothing.
		IPhysicsQueries* RequirePhysicsQueries(ScriptSystem& system, const char* function)
		{
			IPhysicsQueries* queries = system.GetScene().GetService<IPhysicsQueries>();
			if (!queries)
				system.ReportProblem(function, c_WithoutPhysics);
			return queries;
		}

		// The entity a body function acts on, reporting why it cannot: the entity does not exist, has no body in the
		// simulation, or (dynamicOnly) its body is not dynamic.
		Entity RequireBody(ScriptSystem& system, StrataScriptEntityID id, bool dynamicOnly, const char* function)
		{
			const Entity entity = RequireEntity(system, id, function);
			if (!entity)
				return {};
			IBodyMotionQuery* motion = GetBodyMotion(system);
			if (!motion)
			{
				system.ReportProblem(function, c_WithoutPhysics);
				return {};
			}
			if (!motion->HasBody(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' has no body in the physics simulation (bodies are active entities with a RigidBody "
					"component and colliders)", entity.GetName()));
				return {};
			}
			// Colliders without a RigidBody component are static bodies of their own.
			if (dynamicOnly && !motion->IsDynamicBody(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' is not a dynamic body (only dynamic bodies have velocities, forces and impulses "
					"applied to them)", entity.GetName()));
				return {};
			}
			return entity;
		}

		template<typename Query>
		bool GetBodyVector(StrataScriptContext* context, StrataScriptEntityID entity, float out[3], const char* function, Query&& query)
		{
			return HostCall(function, false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, function);
				if (!system)
					return false;
				if (!out)
				{
					system->ReportProblem(function, "the output pointer is null");
					return false;
				}
				const Entity body = RequireBody(*system, entity, false, function);
				if (!body)
					return false;
				WriteVector3(query(*GetBodyMotion(*system), body), out);
				return true;
			});
		}

		// A function changing a dynamic body by a vector: a velocity, force, impulse or torque.
		template<typename Change>
		bool ChangeBody(StrataScriptContext* context, StrataScriptEntityID entity, const float* vector, const char* vectorName, const char* function, Change&& change)
		{
			return HostCall(function, false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, function);
				const Entity body = system ? RequireBody(*system, entity, true, function) : Entity();
				const std::optional<glm::vec3> value = body ? ReadVector3(*system, vector, vectorName, function) : std::nullopt;
				IBodyControl* control = value ? RequireBodyControl(*system, function) : nullptr;
				return control && change(*control, body, *value);
			});
		}

		// A function changing a dynamic body by a vector acting at a world position: a force or impulse.
		template<typename Change>
		bool ChangeBodyAt(StrataScriptContext* context, StrataScriptEntityID entity, const float* vector, const char* vectorName, const float* position,
			const char* function, Change&& change)
		{
			return HostCall(function, false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, function);
				const Entity body = system ? RequireBody(*system, entity, true, function) : Entity();
				const std::optional<glm::vec3> value = body ? ReadVector3(*system, vector, vectorName, function) : std::nullopt;
				const std::optional<glm::vec3> point = value ? ReadVector3(*system, position, "position", function) : std::nullopt;
				IBodyControl* control = point ? RequireBodyControl(*system, function) : nullptr;
				return control && change(*control, body, *value, *point);
			});
		}

		bool HostGetLinearVelocity(StrataScriptContext* context, StrataScriptEntityID entity, float outVelocity[3])
		{
			return GetBodyVector(context, entity, outVelocity, "GetLinearVelocity", [](IBodyMotionQuery& motion, Entity body) { return motion.GetLinearVelocity(body); });
		}

		bool HostSetLinearVelocity(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3])
		{
			return ChangeBody(context, entity, velocity, "velocity", "SetLinearVelocity",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.SetLinearVelocity(body, value); });
		}

		bool HostGetAngularVelocity(StrataScriptContext* context, StrataScriptEntityID entity, float outVelocity[3])
		{
			return GetBodyVector(context, entity, outVelocity, "GetAngularVelocity", [](IBodyMotionQuery& motion, Entity body) { return motion.GetAngularVelocity(body); });
		}

		bool HostSetAngularVelocity(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3])
		{
			return ChangeBody(context, entity, velocity, "velocity", "SetAngularVelocity",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.SetAngularVelocity(body, value); });
		}

		bool HostAddForce(StrataScriptContext* context, StrataScriptEntityID entity, const float force[3])
		{
			return ChangeBody(context, entity, force, "force", "AddForce",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.AddForce(body, value); });
		}

		bool HostAddForceAtPosition(StrataScriptContext* context, StrataScriptEntityID entity, const float force[3], const float worldPosition[3])
		{
			return ChangeBodyAt(context, entity, force, "force", worldPosition, "AddForceAtPosition",
				[](IBodyControl& control, Entity body, const glm::vec3& value, const glm::vec3& position) { return control.AddForceAtPosition(body, value, position); });
		}

		bool HostAddImpulse(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3])
		{
			return ChangeBody(context, entity, impulse, "impulse", "AddImpulse",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.AddImpulse(body, value); });
		}

		bool HostAddImpulseAtPosition(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3], const float worldPosition[3])
		{
			return ChangeBodyAt(context, entity, impulse, "impulse", worldPosition, "AddImpulseAtPosition",
				[](IBodyControl& control, Entity body, const glm::vec3& value, const glm::vec3& position) { return control.AddImpulseAtPosition(body, value, position); });
		}

		bool HostAddTorque(StrataScriptContext* context, StrataScriptEntityID entity, const float torque[3])
		{
			return ChangeBody(context, entity, torque, "torque", "AddTorque",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.AddTorque(body, value); });
		}

		bool HostAddAngularImpulse(StrataScriptContext* context, StrataScriptEntityID entity, const float impulse[3])
		{
			return ChangeBody(context, entity, impulse, "impulse", "AddAngularImpulse",
				[](IBodyControl& control, Entity body, const glm::vec3& value) { return control.AddAngularImpulse(body, value); });
		}

		bool HostTeleport(StrataScriptContext* context, StrataScriptEntityID entity, const float position[3], const float rotation[4])
		{
			return HostCall("Teleport", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "Teleport");
				const Entity body = system ? RequireBody(*system, entity, false, "Teleport") : Entity();
				if (!body)
					return false;
				const std::optional<glm::vec3> worldPosition = ReadVector3(*system, position, "position", "Teleport");
				const std::optional<glm::quat> worldRotation = worldPosition ? ReadRotation(*system, rotation, "Teleport") : std::nullopt;
				IBodyControl* control = worldRotation ? RequireBodyControl(*system, "Teleport") : nullptr;
				if (!control)
					return false;
				if (!control->Teleport(body, *worldPosition, *worldRotation))
				{
					system->ReportProblem("Teleport", fmt::format("'{}' cannot be placed there (its parent is scaled to zero)", body.GetName()));
					return false;
				}
				return true;
			});
		}

		void WriteRaycastHit(const RaycastHit& hit, StrataScriptRaycastHit& out)
		{
			out = {};
			out.Entity = static_cast<uint64_t>(hit.EntityID);
			WriteVector3(hit.Point, out.Point);
			WriteVector3(hit.Normal, out.Normal);
			out.Distance = hit.Distance;
		}

		struct RayArguments
		{
			glm::vec3 Origin = glm::vec3(0.0f);
			glm::vec3 Direction = glm::vec3(0.0f);
			Entity Ignored;
		};

		// Validates a ray a script passes (a finite origin, a finite non-zero direction, a positive distance) and resolves the
		// entity to ignore (one that does not exist ignores nothing).
		std::optional<RayArguments> ReadRay(ScriptSystem& system, const float* origin, const float* direction, float maxDistance, StrataScriptEntityID ignoreEntity,
			const char* function)
		{
			RayArguments ray;
			const std::optional<glm::vec3> start = ReadVector3(system, origin, "origin", function);
			const std::optional<glm::vec3> heading = start ? ReadVector3(system, direction, "direction", function) : std::nullopt;
			if (!heading)
				return std::nullopt;
			if (!(glm::length(*heading) > 1e-12f))
			{
				system.ReportProblem(function, "the direction must not be zero");
				return std::nullopt;
			}
			if (!(maxDistance > 0.0f))
			{
				system.ReportProblem(function, "the maximum distance must be positive");
				return std::nullopt;
			}
			ray.Origin = *start;
			ray.Direction = *heading;
			ray.Ignored = FindEntity(system, ignoreEntity);
			return ray;
		}

		bool HostRaycast(StrataScriptContext* context, const float origin[3], const float direction[3], float maxDistance, uint32_t layerMask,
			StrataScriptEntityID ignoreEntity, bool includeTriggers, StrataScriptRaycastHit* outHit)
		{
			return HostCall("Raycast", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "Raycast");
				const std::optional<RayArguments> ray = system ? ReadRay(*system, origin, direction, maxDistance, ignoreEntity, "Raycast") : std::nullopt;
				IPhysicsQueries* physics = ray ? RequirePhysicsQueries(*system, "Raycast") : nullptr;
				if (!physics)
					return false;
				const std::optional<RaycastHit> hit = physics->Raycast(ray->Origin, ray->Direction, maxDistance, layerMask, ray->Ignored, includeTriggers);
				if (!hit)
					return false;
				if (outHit)
					WriteRaycastHit(*hit, *outHit);
				return true;
			});
		}

		uint32_t HostRaycastAll(StrataScriptContext* context, const float origin[3], const float direction[3], float maxDistance, uint32_t layerMask,
			StrataScriptEntityID ignoreEntity, bool includeTriggers, StrataScriptRaycastHit* outHits, uint32_t capacity)
		{
			return HostCall("RaycastAll", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "RaycastAll");
				const std::optional<RayArguments> ray = system ? ReadRay(*system, origin, direction, maxDistance, ignoreEntity, "RaycastAll") : std::nullopt;
				IPhysicsQueries* physics = ray ? RequirePhysicsQueries(*system, "RaycastAll") : nullptr;
				if (!physics)
					return 0;
				const std::vector<RaycastHit> hits = physics->RaycastAll(ray->Origin, ray->Direction, maxDistance, layerMask, ray->Ignored, includeTriggers);
				if (outHits)
				{
					const size_t count = std::min<size_t>(capacity, hits.size());
					for (size_t index = 0; index < count; index++)
						WriteRaycastHit(hits[index], outHits[index]);
				}
				return static_cast<uint32_t>(hits.size());
			});
		}

		uint32_t HostOverlapSphere(StrataScriptContext* context, const float center[3], float radius, uint32_t layerMask, bool includeTriggers,
			StrataScriptEntityID* outEntities, uint32_t capacity)
		{
			return HostCall("OverlapSphere", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "OverlapSphere");
				const std::optional<glm::vec3> position = system ? ReadVector3(*system, center, "center", "OverlapSphere") : std::nullopt;
				if (!position)
					return 0;
				if (!std::isfinite(radius) || !(radius > 0.0f))
				{
					system->ReportProblem("OverlapSphere", "the radius must be positive and finite");
					return 0;
				}
				IPhysicsQueries* physics = RequirePhysicsQueries(*system, "OverlapSphere");
				return physics ? CopyEntityIDs(physics->OverlapSphere(*position, radius, layerMask, includeTriggers), outEntities, capacity) : 0;
			});
		}

		uint32_t HostOverlapBox(StrataScriptContext* context, const float center[3], const float halfExtents[3], const float rotation[4], uint32_t layerMask,
			bool includeTriggers, StrataScriptEntityID* outEntities, uint32_t capacity)
		{
			return HostCall("OverlapBox", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "OverlapBox");
				const std::optional<glm::vec3> position = system ? ReadVector3(*system, center, "center", "OverlapBox") : std::nullopt;
				const std::optional<glm::vec3> size = position ? ReadVector3(*system, halfExtents, "half extents", "OverlapBox") : std::nullopt;
				const std::optional<glm::quat> orientation = size ? ReadRotation(*system, rotation, "OverlapBox") : std::nullopt;
				if (!orientation)
					return 0;
				if (glm::any(glm::lessThanEqual(*size, glm::vec3(0.0f))))
				{
					system->ReportProblem("OverlapBox", "the half extents must be positive");
					return 0;
				}
				IPhysicsQueries* physics = RequirePhysicsQueries(*system, "OverlapBox");
				return physics ? CopyEntityIDs(physics->OverlapBox(*position, *size, *orientation, layerMask, includeTriggers), outEntities, capacity) : 0;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Character controllers (with the physics functions: they move together when physics is left out of a build)
		////////////////////////////////////////////////////////////////////////////////

		// The scene's character controllers (ICharacterControl) and the entity a character function acts on, reporting why
		// there is none: the entity does not exist, the scene plays without physics, or the entity is no controller.
		Entity RequireCharacter(ScriptSystem& system, StrataScriptEntityID id, const char* function, ICharacterControl*& outCharacters)
		{
			const Entity entity = RequireEntity(system, id, function);
			if (!entity)
				return {};
			outCharacters = system.GetScene().GetService<ICharacterControl>();
			if (!outCharacters)
			{
				system.ReportProblem(function, c_WithoutPhysics);
				return {};
			}
			if (!outCharacters->HasCharacterController(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' is no character controller (character controllers are active entities with an enabled "
					"CharacterController component and no RigidBody)", entity.GetName()));
				return {};
			}
			return entity;
		}

		bool HostCharacterMove(StrataScriptContext* context, StrataScriptEntityID entity, const float velocity[3])
		{
			return HostCall("CharacterMove", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "CharacterMove");
				ICharacterControl* characters = nullptr;
				const Entity character = system ? RequireCharacter(*system, entity, "CharacterMove", characters) : Entity();
				const std::optional<glm::vec3> value = character ? ReadVector3(*system, velocity, "velocity", "CharacterMove") : std::nullopt;
				return value && characters->MoveCharacter(character, *value);
			});
		}

		bool HostCharacterJump(StrataScriptContext* context, StrataScriptEntityID entity, float speed)
		{
			return HostCall("CharacterJump", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "CharacterJump");
				ICharacterControl* characters = nullptr;
				const Entity character = system ? RequireCharacter(*system, entity, "CharacterJump", characters) : Entity();
				if (!character)
					return false;
				if (!std::isfinite(speed))
				{
					system->ReportProblem("CharacterJump", "the speed must be finite");
					return false;
				}
				return characters->JumpCharacter(character, speed);
			});
		}

		bool HostCharacterTeleport(StrataScriptContext* context, StrataScriptEntityID entity, const float position[3])
		{
			return HostCall("CharacterTeleport", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "CharacterTeleport");
				ICharacterControl* characters = nullptr;
				const Entity character = system ? RequireCharacter(*system, entity, "CharacterTeleport", characters) : Entity();
				const std::optional<glm::vec3> value = character ? ReadVector3(*system, position, "position", "CharacterTeleport") : std::nullopt;
				if (!value)
					return false;
				if (!characters->TeleportCharacter(character, *value))
				{
					system->ReportProblem("CharacterTeleport", fmt::format("'{}' cannot be placed there (its parent is scaled to zero)", character.GetName()));
					return false;
				}
				return true;
			});
		}

		bool HostCharacterGetState(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptCharacterState* outState)
		{
			return HostCall("CharacterGetState", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "CharacterGetState");
				if (!system)
					return false;
				if (!outState)
				{
					system->ReportProblem("CharacterGetState", "the output pointer is null");
					return false;
				}
				ICharacterControl* characters = nullptr;
				const Entity character = RequireCharacter(*system, entity, "CharacterGetState", characters);
				const std::optional<CharacterState> state = character ? characters->GetCharacterState(character) : std::nullopt;
				if (!state)
					return false;
				*outState = {};
				outState->GroundState = static_cast<uint32_t>(state->Ground);
				outState->GroundEntity = state->GroundEntity ? static_cast<uint64_t>(state->GroundEntity.GetUUID()) : 0;
				WriteVector3(state->GroundNormal, outState->GroundNormal);
				WriteVector3(state->GroundVelocity, outState->GroundVelocity);
				WriteVector3(state->Velocity, outState->Velocity);
				return true;
			});
		}

		static_assert(static_cast<uint32_t>(CharacterGroundState::OnGround) == StrataScriptGroundState_OnGround
			&& static_cast<uint32_t>(CharacterGroundState::OnSteepGround) == StrataScriptGroundState_OnSteepGround
			&& static_cast<uint32_t>(CharacterGroundState::NotSupported) == StrataScriptGroundState_NotSupported
			&& static_cast<uint32_t>(CharacterGroundState::InAir) == StrataScriptGroundState_InAir, "The ground states are the ABI's");

		////////////////////////////////////////////////////////////////////////////////
		// Audio
		////////////////////////////////////////////////////////////////////////////////

		// The scene's audio service (Scene/SceneServices.h), reporting when the scene plays without audio.
		IAudioPlayback* RequireAudio(ScriptSystem& system, const char* function)
		{
			IAudioPlayback* audio = system.GetScene().GetService<IAudioPlayback>();
			if (!audio)
				system.ReportProblem(function, c_WithoutAudio);
			return audio;
		}

		// The engine's audio output (an engine service: it works whether or not the scene plays audio), reporting when the
		// build has no audio module.
		IAudioOutput* RequireAudioOutput(ScriptSystem& system, const char* function)
		{
			IAudioOutput* output = EngineServices::Get<IAudioOutput>();
			if (!output)
				system.ReportProblem(function, "the engine has no audio output");
			return output;
		}

		// The entity whose AudioSource a function acts on, reporting why there is none.
		Entity RequireAudioSource(ScriptSystem& system, IAudioPlayback& audio, StrataScriptEntityID id, const char* function)
		{
			const Entity entity = RequireEntity(system, id, function);
			if (!entity)
				return {};
			if (!audio.HasSource(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' has no AudioSource component", entity.GetName()));
				return {};
			}
			if (!system.GetScene().IsActiveInHierarchy(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' is inactive, so its AudioSource is silent", entity.GetName()));
				return {};
			}
			return entity;
		}

		template<typename Result, typename Action>
		Result ControlSource(StrataScriptContext* context, StrataScriptEntityID entity, const char* function, Result failure, Action&& action)
		{
			return HostCall(function, failure, [&]() -> Result
			{
				ScriptSystem* system = ResolveContext(context, function);
				IAudioPlayback* audio = system ? RequireAudio(*system, function) : nullptr;
				const Entity source = audio ? RequireAudioSource(*system, *audio, entity, function) : Entity();
				return source ? action(*system, *audio, source) : failure;
			});
		}

		bool HostAudioPlay(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlSource(context, entity, "AudioPlay", false, [](ScriptSystem& system, IAudioPlayback& audio, Entity source)
			{
				if (audio.Play(source))
					return true;
				system.ReportProblem("AudioPlay", fmt::format("'{}' cannot play: its clip is missing or unusable, or there is no audio output",
					source.GetName()));
				return false;
			});
		}

		bool HostAudioPause(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlSource(context, entity, "AudioPause", false, [](ScriptSystem&, IAudioPlayback& audio, Entity source) { return audio.Pause(source); });
		}

		bool HostAudioStop(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlSource(context, entity, "AudioStop", false, [](ScriptSystem&, IAudioPlayback& audio, Entity source) { return audio.Stop(source); });
		}

		bool HostAudioIsPlaying(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlSource(context, entity, "AudioIsPlaying", false, [](ScriptSystem&, IAudioPlayback& audio, Entity source) { return audio.IsPlaying(source); });
		}

		bool HostAudioSeek(StrataScriptContext* context, StrataScriptEntityID entity, float seconds)
		{
			return ControlSource(context, entity, "AudioSeek", false, [seconds](ScriptSystem& system, IAudioPlayback& audio, Entity source)
			{
				if (!std::isfinite(seconds))
				{
					system.ReportProblem("AudioSeek", "the position must be finite");
					return false;
				}
				return audio.Seek(source, seconds);
			});
		}

		float HostAudioGetPlaybackPosition(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlSource(context, entity, "AudioGetPlaybackPosition", 0.0f,
				[](ScriptSystem&, IAudioPlayback& audio, Entity source) { return audio.GetPlaybackPosition(source); });
		}

		// Checks a one-shot's clip, volume and pitch, reporting what is wrong.
		bool ValidateOneShot(ScriptSystem& system, StrataScriptAssetHandle clip, float volume, float pitch, const char* function)
		{
			if (clip == 0)
			{
				system.ReportProblem(function, "the clip is null");
				return false;
			}
			AssetManagerBase* manager = RequireAssetManager(system, function);
			if (!manager)
				return false;
			if (manager->GetAssetType(AssetHandle(clip)) != AssetType::AudioClip)
			{
				system.ReportProblem(function, fmt::format("asset {} is not an audio clip", AssetHandle(clip).ToString()));
				return false;
			}
			if (!std::isfinite(volume) || volume < 0.0f)
			{
				system.ReportProblem(function, "the volume must be finite and not negative");
				return false;
			}
			if (!std::isfinite(pitch) || !(pitch > 0.0f))
			{
				system.ReportProblem(function, "the pitch must be finite and positive");
				return false;
			}
			return true;
		}

		bool HostAudioPlayOneShot(StrataScriptContext* context, StrataScriptAssetHandle clip, float volume, float pitch)
		{
			return HostCall("AudioPlayOneShot", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AudioPlayOneShot");
				IAudioPlayback* audio = system ? RequireAudio(*system, "AudioPlayOneShot") : nullptr;
				if (!audio || !ValidateOneShot(*system, clip, volume, pitch, "AudioPlayOneShot"))
					return false;
				if (audio->PlayOneShot(AssetHandle(clip), volume, pitch))
					return true;
				system->ReportProblem("AudioPlayOneShot", fmt::format("clip {} cannot play (it failed to load, or there is no audio output)",
					AssetHandle(clip).ToString()));
				return false;
			});
		}

		bool HostAudioPlayOneShotAt(StrataScriptContext* context, StrataScriptAssetHandle clip, const float position[3], float volume, float pitch)
		{
			return HostCall("AudioPlayOneShotAt", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AudioPlayOneShotAt");
				IAudioPlayback* audio = system ? RequireAudio(*system, "AudioPlayOneShotAt") : nullptr;
				if (!audio || !ValidateOneShot(*system, clip, volume, pitch, "AudioPlayOneShotAt"))
					return false;
				const std::optional<glm::vec3> worldPosition = ReadVector3(*system, position, "position", "AudioPlayOneShotAt");
				if (!worldPosition)
					return false;
				if (audio->PlayOneShotAt(AssetHandle(clip), *worldPosition, volume, pitch))
					return true;
				system->ReportProblem("AudioPlayOneShotAt", fmt::format("clip {} cannot play (it failed to load, or there is no audio output)",
					AssetHandle(clip).ToString()));
				return false;
			});
		}

		void HostAudioSetMasterVolume(StrataScriptContext* context, float volume)
		{
			HostCallVoid("AudioSetMasterVolume", [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AudioSetMasterVolume");
				if (!system)
					return;
				if (!std::isfinite(volume))
				{
					system->ReportProblem("AudioSetMasterVolume", "the volume must be finite");
					return;
				}
				if (IAudioOutput* output = RequireAudioOutput(*system, "AudioSetMasterVolume"))
					output->SetMasterVolume(volume);
			});
		}

		float HostAudioGetMasterVolume(StrataScriptContext* context)
		{
			return HostCall("AudioGetMasterVolume", 0.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "AudioGetMasterVolume");
				IAudioOutput* output = system ? RequireAudioOutput(*system, "AudioGetMasterVolume") : nullptr;
				return output ? output->GetMasterVolume() : 0.0f;
			});
		}
		////////////////////////////////////////////////////////////////////////////////
		// Game flow
		////////////////////////////////////////////////////////////////////////////////

		void HostQuitGame(StrataScriptContext* context, int32_t exitCode)
		{
			HostCallVoid("QuitGame", [&]()
			{
				if (ScriptSystem* system = ResolveContext(context, "QuitGame"))
					system->GetScene().RequestQuit(exitCode);
			});
		}

		bool HostLoadScene(StrataScriptContext* context, StrataScriptAssetHandle scene)
		{
			return HostCall("LoadScene", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "LoadScene");
				if (!system)
					return false;
				// The null handle restarts the running scene, whatever it was loaded from.
				if (scene != 0)
				{
					AssetManagerBase* manager = RequireAssetManager(*system, "LoadScene");
					if (!manager)
						return false;
					if (manager->GetAssetType(AssetHandle(scene)) != AssetType::Scene)
					{
						system->ReportProblem("LoadScene", fmt::format("asset {} is not a scene", AssetHandle(scene).ToString()));
						return false;
					}
				}
				system->GetScene().RequestSceneLoad(UUID(scene));
				return true;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Scene streaming
		////////////////////////////////////////////////////////////////////////////////

		uint64_t HostLoadSceneAdditive(StrataScriptContext* context, StrataScriptAssetHandle scene)
		{
			return HostCall("LoadSceneAdditive", uint64_t(0), [&]() -> uint64_t
			{
				ScriptSystem* system = ResolveContext(context, "LoadSceneAdditive");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "LoadSceneAdditive") : nullptr;
				if (!manager)
					return 0;
				const AssetHandle handle(scene);
				if (manager->GetAssetType(handle) != AssetType::Scene)
				{
					system->ReportProblem("LoadSceneAdditive", scene == 0 ? std::string("the asset is null")
						: fmt::format("asset {} is not a scene", handle.ToString()));
					return 0;
				}
				std::string error;
				const SubSceneID id = system->GetScene().LoadSubScene(handle, {}, &error);
				if (id == 0)
					system->ReportProblem("LoadSceneAdditive", fmt::format("scene {} cannot be loaded: {}", handle.ToString(), error));
				return id;
			});
		}

		bool HostUnloadSubScene(StrataScriptContext* context, uint64_t subScene)
		{
			return HostCall("UnloadSubScene", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "UnloadSubScene");
				// Unloading what is gone (e.g. twice) is no misuse: just false.
				return system && system->GetScene().UnloadSubScene(subScene);
			});
		}

		bool HostIsSubSceneLoaded(StrataScriptContext* context, uint64_t subScene)
		{
			return HostCall("IsSubSceneLoaded", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "IsSubSceneLoaded");
				return system && system->GetScene().IsSubSceneLoaded(subScene);
			});
		}

		float HostGetSceneLoadProgress(StrataScriptContext* context)
		{
			return HostCall("GetSceneLoadProgress", -1.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetSceneLoadProgress");
				const std::optional<float> progress = system ? system->GetScene().GetSceneLoadProgress() : std::nullopt;
				return progress.value_or(-1.0f);
			});
		}

		float HostGetAssetLoadProgress(StrataScriptContext* context, StrataScriptAssetHandle asset)
		{
			return HostCall("GetAssetLoadProgress", -1.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetAssetLoadProgress");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "GetAssetLoadProgress") : nullptr;
				if (!manager)
					return -1.0f;
				const float progress = system->GetAssetLoadProgress(*manager, AssetHandle(asset));
				if (progress < 0.0f)
					system->ReportProblem("GetAssetLoadProgress", asset == 0 ? std::string("the asset is null")
						: fmt::format("asset {} does not exist", AssetHandle(asset).ToString()));
				return progress;
			});
		}

		uint32_t HostInstantiateMany(StrataScriptContext* context, StrataScriptAssetHandle asset, uint32_t count, const float* transforms,
			StrataScriptEntityID parent, StrataScriptEntityID* outEntities)
		{
			return HostCall("InstantiateMany", uint32_t(0), [&]() -> uint32_t
			{
				ScriptSystem* system = ResolveContext(context, "InstantiateMany");
				AssetManagerBase* manager = system ? RequireAssetManager(*system, "InstantiateMany") : nullptr;
				if (!manager || count == 0)
					return 0;
				if (!transforms || !outEntities)
				{
					system->ReportProblem("InstantiateMany", "the transforms or the entity array are null");
					return 0;
				}
				if (count > Scene::c_MaxEntities)
				{
					system->ReportProblem("InstantiateMany", fmt::format("{} instances are more than a scene holds", count));
					return 0;
				}

				const AssetHandle handle(asset);
				const AssetType type = manager->GetAssetType(handle);
				if (type != AssetType::Prefab && type != AssetType::Model)
				{
					system->ReportProblem("InstantiateMany", asset == 0 ? std::string("the asset is null")
						: fmt::format("asset {} is not a prefab or model", handle.ToString()));
					return 0;
				}
				Entity parentEntity;
				if (parent != 0 && !(parentEntity = RequireEntity(*system, parent, "InstantiateMany")))
					return 0;

				// Module memory is read once, into engine memory, before anything is created.
				std::vector<glm::mat4> matrices(count);
				for (uint32_t index = 0; index < count; index++)
				{
					const float* values = transforms + static_cast<size_t>(index) * 16;
					for (uint32_t element = 0; element < 16; element++)
					{
						if (!std::isfinite(values[element]))
						{
							system->ReportProblem("InstantiateMany", fmt::format("transform {} is not finite", index));
							return 0;
						}
					}
					matrices[index] = glm::make_mat4(values);
				}

				// Never waits for loading, as Instantiate.
				const Ref<Asset> loaded = manager->GetAsset(handle, AssetPriority::High);
				if (!loaded)
				{
					if (manager->GetAssetState(handle) == AssetState::Failed)
						system->ReportProblem("InstantiateMany", fmt::format("asset {} failed to load: {}", handle.ToString(), manager->GetAssetError(handle)));
					else
						system->ReportProblem("InstantiateMany", fmt::format("asset {} is not loaded yet (its load was started); request assets early "
							"(Assets::RequestLoad, Assets::GetLoadProgress) and instantiate them once they are loaded", handle.ToString()));
					return 0;
				}
				if (loaded->GetType() != AssetType::Prefab && loaded->GetType() != AssetType::Model)
				{
					system->ReportProblem("InstantiateMany", fmt::format("asset {} is not a prefab or model", handle.ToString()));
					return 0;
				}

				std::string error;
				const std::vector<Entity> roots = std::static_pointer_cast<EntityTemplate>(loaded)->InstantiateMany(system->GetScene(), matrices, parentEntity, &error);
				if (roots.size() != count)
				{
					system->ReportProblem("InstantiateMany", error.empty() ? fmt::format("asset {} contains no entities", handle.ToString())
						: fmt::format("asset {} could not be instantiated: {}", handle.ToString(), error));
					return 0;
				}
				for (uint32_t index = 0; index < count; index++)
					outEntities[index] = static_cast<uint64_t>(roots[index].GetUUID());
				// Scripts of the new entities exist when this returns, as for Instantiate.
				system->CreatePendingInstances();
				return count;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Animation
		////////////////////////////////////////////////////////////////////////////////

		// The scene's animator service (Scene/SceneServices.h), reporting when the scene plays without animation.
		IAnimatorControl* RequireAnimation(ScriptSystem& system, const char* function)
		{
			IAnimatorControl* animation = system.GetScene().GetService<IAnimatorControl>();
			if (!animation)
				system.ReportProblem(function, c_WithoutAnimation);
			return animation;
		}

		// The entity whose Animator a function acts on, reporting why there is none.
		Entity RequireAnimator(ScriptSystem& system, IAnimatorControl& animation, StrataScriptEntityID id, const char* function)
		{
			const Entity entity = RequireEntity(system, id, function);
			if (entity && !animation.HasAnimator(entity))
			{
				system.ReportProblem(function, fmt::format("'{}' has no Animator component", entity.GetName()));
				return {};
			}
			return entity;
		}

		template<typename Result, typename Action>
		Result ControlAnimator(StrataScriptContext* context, StrataScriptEntityID entity, const char* function, Result failure, Action&& action)
		{
			return HostCall(function, failure, [&]() -> Result
			{
				ScriptSystem* system = ResolveContext(context, function);
				IAnimatorControl* animation = system ? RequireAnimation(*system, function) : nullptr;
				const Entity animator = animation ? RequireAnimator(*system, *animation, entity, function) : Entity();
				return animator ? action(*system, *animation, animator) : failure;
			});
		}

		bool HostAnimatorPlay(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString state)
		{
			return ControlAnimator(context, entity, "AnimatorPlay", false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				const std::string_view name = ScriptStringView(state);
				if (animation.Play(animator, name))
					return true;
				system.ReportProblem("AnimatorPlay", fmt::format("the animator of '{}' has no controller with a state '{}'", animator.GetName(), name));
				return false;
			});
		}

		bool HostAnimatorPlayClip(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptAssetHandle clip)
		{
			return ControlAnimator(context, entity, "AnimatorPlayClip", false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				if (animation.PlayClip(animator, AssetHandle(clip)))
					return true;
				system.ReportProblem("AnimatorPlayClip", fmt::format("asset {} is not an animation clip", AssetHandle(clip).ToString()));
				return false;
			});
		}

		bool HostAnimatorCrossFade(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString state, float seconds)
		{
			return ControlAnimator(context, entity, "AnimatorCrossFade", false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				const std::string_view name = ScriptStringView(state);
				if (!std::isfinite(seconds) || seconds < 0.0f)
				{
					system.ReportProblem("AnimatorCrossFade", "the duration must be finite and not negative");
					return false;
				}
				if (animation.CrossFade(animator, name, seconds))
					return true;
				system.ReportProblem("AnimatorCrossFade", fmt::format("the animator of '{}' has no controller with a state '{}'", animator.GetName(), name));
				return false;
			});
		}

		// Sets a parameter through `set`, reporting a parameter the animator's controller does not have with that type.
		template<typename Set>
		bool SetAnimatorParameter(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter, const char* function,
			const char* type, Set&& set)
		{
			return ControlAnimator(context, entity, function, false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				const std::string_view name = ScriptStringView(parameter);
				if (set(animation, animator, name))
					return true;
				system.ReportProblem(function, fmt::format("the animator of '{}' has no {} parameter '{}' (or the value is not finite)", animator.GetName(), type, name));
				return false;
			});
		}

		bool HostAnimatorSetFloat(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter, float value)
		{
			return SetAnimatorParameter(context, entity, parameter, "AnimatorSetFloat", "float",
				[value](IAnimatorControl& animation, Entity animator, std::string_view name) { return animation.SetFloat(animator, name, value); });
		}

		bool HostAnimatorSetBool(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter, bool value)
		{
			return SetAnimatorParameter(context, entity, parameter, "AnimatorSetBool", "bool",
				[value](IAnimatorControl& animation, Entity animator, std::string_view name) { return animation.SetBool(animator, name, value); });
		}

		bool HostAnimatorSetTrigger(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter)
		{
			return SetAnimatorParameter(context, entity, parameter, "AnimatorSetTrigger", "trigger",
				[](IAnimatorControl& animation, Entity animator, std::string_view name) { return animation.SetTrigger(animator, name); });
		}

		bool HostAnimatorResetTrigger(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString parameter)
		{
			return SetAnimatorParameter(context, entity, parameter, "AnimatorResetTrigger", "trigger",
				[](IAnimatorControl& animation, Entity animator, std::string_view name) { return animation.ResetTrigger(animator, name); });
		}

		bool HostAnimatorSetSpeed(StrataScriptContext* context, StrataScriptEntityID entity, float speed)
		{
			return ControlAnimator(context, entity, "AnimatorSetSpeed", false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				if (animation.SetSpeed(animator, speed))
					return true;
				system.ReportProblem("AnimatorSetSpeed", "the speed must be finite and at most 100 in magnitude");
				return false;
			});
		}

		uint64_t HostAnimatorGetCurrentState(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity)
		{
			return ControlAnimator(context, entity, "AnimatorGetCurrentState", uint64_t(0), [&](ScriptSystem&, IAnimatorControl& animation, Entity animator)
			{
				const std::optional<std::string> state = animation.GetCurrentState(animator);
				return state ? CopyText(*state, buffer, capacity) : uint64_t(0);
			});
		}

		float HostAnimatorGetNormalizedTime(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlAnimator(context, entity, "AnimatorGetNormalizedTime", 0.0f,
				[](ScriptSystem&, IAnimatorControl& animation, Entity animator) { return animation.GetNormalizedTime(animator); });
		}

		bool HostAnimatorIsInTransition(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlAnimator(context, entity, "AnimatorIsInTransition", false,
				[](ScriptSystem&, IAnimatorControl& animation, Entity animator) { return animation.IsInTransition(animator); });
		}

		bool HostAnimatorGetJointTransform(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString joint, StrataScriptTransform* outTransform)
		{
			return ControlAnimator(context, entity, "AnimatorGetJointTransform", false, [&](ScriptSystem& system, IAnimatorControl& animation, Entity animator)
			{
				if (!outTransform)
					return false;
				const std::string_view name = ScriptStringView(joint);
				const std::optional<glm::mat4> transform = animation.GetJointTransform(animator, name);
				if (!transform)
				{
					system.ReportProblem("AnimatorGetJointTransform", fmt::format("the animator of '{}' has no pose with a joint '{}'", animator.GetName(), name));
					return false;
				}
				glm::vec3 translation, scale;
				glm::quat rotation;
				if (!Math::DecomposeTransform(*transform, translation, rotation, scale))
				{
					system.ReportProblem("AnimatorGetJointTransform", fmt::format("the transform of joint '{}' of '{}' is degenerate", name, animator.GetName()));
					return false;
				}
				WriteTransform(translation, rotation, scale, *outTransform);
				return true;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// The game's window and save files (engine services of the program running the game)
		////////////////////////////////////////////////////////////////////////////////

		IGameWindow* RequireGameWindow(ScriptSystem& system, const char* function)
		{
			IGameWindow* window = EngineServices::Get<IGameWindow>();
			if (!window)
				system.ReportProblem(function, "the program running the game has no window to change");
			return window;
		}

		static_assert(StrataScriptWindowMode_Windowed == static_cast<uint32_t>(GameWindowMode::Windowed)
			&& StrataScriptWindowMode_Fullscreen == static_cast<uint32_t>(GameWindowMode::Fullscreen)
			&& StrataScriptWindowMode_Borderless == static_cast<uint32_t>(GameWindowMode::Borderless), "The window modes are the ABI's");

		bool HostSetWindowMode(StrataScriptContext* context, uint32_t mode)
		{
			return HostCall("SetWindowMode", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetWindowMode");
				IGameWindow* window = system ? RequireGameWindow(*system, "SetWindowMode") : nullptr;
				if (!window)
					return false;
				if (mode > StrataScriptWindowMode_Borderless)
				{
					system->ReportProblem("SetWindowMode", fmt::format("{} is no window mode (0 windowed, 1 fullscreen, 2 borderless)", mode));
					return false;
				}
				return window->SetWindowMode(static_cast<GameWindowMode>(mode));
			});
		}

		bool HostSetResolution(StrataScriptContext* context, uint32_t width, uint32_t height)
		{
			return HostCall("SetResolution", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetResolution");
				IGameWindow* window = system ? RequireGameWindow(*system, "SetResolution") : nullptr;
				if (!window)
					return false;
				if (width == 0 || height == 0 || width > IGameWindow::c_MaxResolution || height > IGameWindow::c_MaxResolution)
				{
					system->ReportProblem("SetResolution", fmt::format("{}x{} is no resolution (1 to {} pixels per side)", width, height, IGameWindow::c_MaxResolution));
					return false;
				}
				return window->SetResolution(width, height);
			});
		}

		bool HostSetVSync(StrataScriptContext* context, bool enabled)
		{
			return HostCall("SetVSync", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "SetVSync");
				IGameWindow* window = system ? RequireGameWindow(*system, "SetVSync") : nullptr;
				return window && window->SetVSync(enabled);
			});
		}

		bool HostGetWindowSize(StrataScriptContext* context, uint32_t outSize[2])
		{
			return HostCall("GetWindowSize", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetWindowSize");
				if (!system)
					return false;
				if (!outSize)
				{
					system->ReportProblem("GetWindowSize", "the output pointer is null");
					return false;
				}
				IGameWindow* window = RequireGameWindow(*system, "GetWindowSize");
				if (!window)
					return false;
				const GameWindowState state = window->GetState();
				outSize[0] = state.Width;
				outSize[1] = state.Height;
				return true;
			});
		}

		// The game's save directory, reporting when the program running the game keeps no save files.
		std::optional<std::filesystem::path> RequireSaveDirectory(ScriptSystem& system, const char* function)
		{
			IGameSaves* saves = EngineServices::Get<IGameSaves>();
			std::optional<std::filesystem::path> directory = saves ? saves->GetSaveDirectory() : std::nullopt;
			if (!directory)
				system.ReportProblem(function, "the program running the game has no save directory (the editor keeps saves in its open project)");
			return directory;
		}

		bool HostWriteSaveFile(StrataScriptContext* context, StrataScriptString name, const void* data, uint64_t size)
		{
			return HostCall("WriteSaveFile", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "WriteSaveFile");
				if (!system)
					return false;
				if (!data && size > 0)
				{
					system->ReportProblem("WriteSaveFile", "the data pointer is null");
					return false;
				}
				const std::optional<std::filesystem::path> directory = RequireSaveDirectory(*system, "WriteSaveFile");
				if (!directory)
					return false;
				if (size > SaveFiles::c_MaxFileSize)
				{
					system->ReportProblem("WriteSaveFile", fmt::format("{} bytes are more than a save file may hold ({} bytes)", size, SaveFiles::c_MaxFileSize));
					return false;
				}
				std::string error;
				const std::span<const uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<size_t>(size));
				if (!SaveFiles::Write(*directory, ScriptStringView(name), bytes, &error))
				{
					system->ReportProblem("WriteSaveFile", error);
					return false;
				}
				return true;
			});
		}

		bool HostReadSaveFile(StrataScriptContext* context, StrataScriptString name, void* buffer, uint64_t capacity, uint64_t* outSize)
		{
			return HostCall("ReadSaveFile", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "ReadSaveFile");
				if (!system)
					return false;
				if (!outSize)
				{
					system->ReportProblem("ReadSaveFile", "the size pointer is null");
					return false;
				}
				const std::optional<std::filesystem::path> directory = RequireSaveDirectory(*system, "ReadSaveFile");
				if (!directory)
					return false;
				const std::string_view saveName = ScriptStringView(name);
				if (std::optional<std::string> problem = SaveFiles::ValidateName(saveName))
				{
					system->ReportProblem("ReadSaveFile", *problem);
					return false;
				}
				// A save that does not exist (yet) is an answer, not a mistake.
				if (!SaveFiles::Exists(*directory, saveName))
					return false;
				std::string error;
				const std::optional<std::vector<uint8_t>> bytes = SaveFiles::Read(*directory, saveName, &error);
				if (!bytes)
				{
					system->ReportProblem("ReadSaveFile", error);
					return false;
				}
				*outSize = bytes->size();
				if (buffer && capacity > 0)
					std::memcpy(buffer, bytes->data(), static_cast<size_t>(std::min<uint64_t>(capacity, bytes->size())));
				return true;
			});
		}

		bool HostDeleteSaveFile(StrataScriptContext* context, StrataScriptString name)
		{
			return HostCall("DeleteSaveFile", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "DeleteSaveFile");
				const std::optional<std::filesystem::path> directory = system ? RequireSaveDirectory(*system, "DeleteSaveFile") : std::nullopt;
				if (!directory)
					return false;
				const std::string_view saveName = ScriptStringView(name);
				if (std::optional<std::string> problem = SaveFiles::ValidateName(saveName))
				{
					system->ReportProblem("DeleteSaveFile", *problem);
					return false;
				}
				// Deleting what is not there leaves nothing to do; other failures are reported.
				if (!SaveFiles::Exists(*directory, saveName))
					return false;
				std::string error;
				if (!SaveFiles::Delete(*directory, saveName, &error))
				{
					system->ReportProblem("DeleteSaveFile", error);
					return false;
				}
				return true;
			});
		}

		uint64_t HostListSaveFiles(StrataScriptContext* context, char* buffer, uint64_t capacity)
		{
			return HostCall("ListSaveFiles", uint64_t(0), [&]() -> uint64_t
			{
				ScriptSystem* system = ResolveContext(context, "ListSaveFiles");
				const std::optional<std::filesystem::path> directory = system ? RequireSaveDirectory(*system, "ListSaveFiles") : std::nullopt;
				if (!directory)
					return 0;
				std::string text;
				for (const std::string& saveName : SaveFiles::List(*directory))
				{
					text += saveName;
					text += '\n';
				}
				return CopyText(text, buffer, capacity);
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Collision layers
		////////////////////////////////////////////////////////////////////////////////

		// The layer matrix lives in the scene's settings; the physics system applies changes before its next step (also when
		// the scene plays without physics yet: a world created later starts with the matrix).
		bool HostPhysicsSetLayersCollide(StrataScriptContext* context, uint32_t layerA, uint32_t layerB, bool collide)
		{
			return HostCall("PhysicsSetLayersCollide", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "PhysicsSetLayersCollide");
				if (!system)
					return false;
				if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
				{
					system->ReportProblem("PhysicsSetLayersCollide",
						fmt::format("layers {} and {}: layers range from 0 to {}", layerA, layerB, c_PhysicsLayerCount - 1));
					return false;
				}
				system->GetScene().GetSettings().PhysicsLayers.SetCollide(layerA, layerB, collide);
				return true;
			});
		}

		bool HostPhysicsGetLayersCollide(StrataScriptContext* context, uint32_t layerA, uint32_t layerB)
		{
			return HostCall("PhysicsGetLayersCollide", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "PhysicsGetLayersCollide");
				return system && system->GetScene().GetSettings().PhysicsLayers.DoCollide(layerA, layerB);
			});
		}

		int32_t HostPhysicsGetLayerIndex(StrataScriptContext* context, StrataScriptString name)
		{
			return HostCall("PhysicsGetLayerIndex", int32_t(-1), [&]() -> int32_t
			{
				ScriptSystem* system = ResolveContext(context, "PhysicsGetLayerIndex");
				if (!system)
					return -1;
				// An unknown name is an answer (-1), not misuse: games look layers up that a scene may not have.
				const std::optional<uint32_t> layer = system->GetScene().GetSettings().PhysicsLayers.FindLayer(ScriptStringView(name));
				return layer ? static_cast<int32_t>(*layer) : -1;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// Game UI
		////////////////////////////////////////////////////////////////////////////////

		// The scene's game UI (Scene/SceneServices.h), reporting when the scene plays without it.
		IUIControl* RequireUI(ScriptSystem& system, const char* function)
		{
			IUIControl* ui = system.GetScene().GetService<IUIControl>();
			if (!ui)
				system.ReportProblem(function, "the scene plays without the game UI");
			return ui;
		}

		// Runs an element function of the game UI on an entity the script names; the service's reason is reported when it
		// fails.
		template<typename Result, typename Action>
		Result ControlUIElement(StrataScriptContext* context, StrataScriptEntityID entity, const char* function, Result failure, Action&& action)
		{
			return HostCall(function, failure, [&]() -> Result
			{
				ScriptSystem* system = ResolveContext(context, function);
				IUIControl* ui = system ? RequireUI(*system, function) : nullptr;
				const Entity element = ui ? RequireEntity(*system, entity, function) : Entity();
				if (!element)
					return failure;
				std::string error;
				std::optional<Result> result = action(*ui, element, error);
				if (!result)
				{
					system->ReportProblem(function, error);
					return failure;
				}
				return *result;
			});
		}

		uint64_t HostUIGetText(StrataScriptContext* context, StrataScriptEntityID entity, char* buffer, uint64_t capacity)
		{
			return ControlUIElement(context, entity, "UIGetText", uint64_t(0), [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<uint64_t>
			{
				std::string text;
				if (!ui.GetText(element, text, &error))
					return std::nullopt;
				return CopyText(text, buffer, capacity);
			});
		}

		bool HostUISetText(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptString text)
		{
			return ControlUIElement(context, entity, "UISetText", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!ui.SetText(element, ScriptStringView(text), &error))
					return std::nullopt;
				return true;
			});
		}

		StrataScriptAssetHandle HostUIGetImage(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlUIElement(context, entity, "UIGetImage", StrataScriptAssetHandle(0), [&](IUIControl& ui, Entity element, std::string& error)
				-> std::optional<StrataScriptAssetHandle>
			{
				AssetHandle texture;
				if (!ui.GetImage(element, texture, &error))
					return std::nullopt;
				return static_cast<StrataScriptAssetHandle>(static_cast<uint64_t>(texture));
			});
		}

		bool HostUISetImage(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptAssetHandle texture)
		{
			return ControlUIElement(context, entity, "UISetImage", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!ui.SetImage(element, AssetHandle(texture), &error))
					return std::nullopt;
				return true;
			});
		}

		bool HostUIGetColor(StrataScriptContext* context, StrataScriptEntityID entity, float outColor[4])
		{
			return ControlUIElement(context, entity, "UIGetColor", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!outColor)
				{
					error = "the output pointer is null";
					return std::nullopt;
				}
				glm::vec4 color(0.0f);
				if (!ui.GetColor(element, color, &error))
					return std::nullopt;
				for (int index = 0; index < 4; index++)
					outColor[index] = color[index];
				return true;
			});
		}

		bool HostUISetColor(StrataScriptContext* context, StrataScriptEntityID entity, const float color[4])
		{
			return ControlUIElement(context, entity, "UISetColor", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!color)
				{
					error = "the color pointer is null";
					return std::nullopt;
				}
				if (!ui.SetColor(element, glm::vec4(color[0], color[1], color[2], color[3]), &error))
					return std::nullopt;
				return true;
			});
		}

		bool HostUIIsInteractable(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlUIElement(context, entity, "UIIsInteractable", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				bool interactable = false;
				if (!ui.IsInteractable(element, interactable, &error))
					return std::nullopt;
				return interactable;
			});
		}

		bool HostUISetInteractable(StrataScriptContext* context, StrataScriptEntityID entity, bool interactable)
		{
			return ControlUIElement(context, entity, "UISetInteractable", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!ui.SetInteractable(element, interactable, &error))
					return std::nullopt;
				return true;
			});
		}

		float HostUIGetValue(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return ControlUIElement(context, entity, "UIGetValue", 0.0f, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<float>
			{
				float value = 0.0f;
				if (!ui.GetValue(element, value, &error))
					return std::nullopt;
				return value;
			});
		}

		bool HostUISetValue(StrataScriptContext* context, StrataScriptEntityID entity, float value)
		{
			return ControlUIElement(context, entity, "UISetValue", false, [&](IUIControl& ui, Entity element, std::string& error) -> std::optional<bool>
			{
				if (!ui.SetValue(element, value, &error))
					return std::nullopt;
				return true;
			});
		}

		StrataScriptEntityID HostUIGetFocus(StrataScriptContext* context)
		{
			return HostCall("UIGetFocus", StrataScriptEntityID(0), [&]() -> StrataScriptEntityID
			{
				ScriptSystem* system = ResolveContext(context, "UIGetFocus");
				IUIControl* ui = system ? RequireUI(*system, "UIGetFocus") : nullptr;
				const Entity focused = ui ? ui->GetFocus() : Entity();
				return focused ? static_cast<uint64_t>(focused.GetUUID()) : 0;
			});
		}

		bool HostUISetFocus(StrataScriptContext* context, StrataScriptEntityID entity)
		{
			return HostCall("UISetFocus", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "UISetFocus");
				IUIControl* ui = system ? RequireUI(*system, "UISetFocus") : nullptr;
				if (!ui)
					return false;
				// 0 clears the focus; other ids must name an entity.
				const Entity selectable = entity != 0 ? RequireEntity(*system, entity, "UISetFocus") : Entity();
				if (entity != 0 && !selectable)
					return false;
				std::string error;
				if (ui->SetFocus(selectable, &error))
					return true;
				system->ReportProblem("UISetFocus", error);
				return false;
			});
		}

		bool HostUIIsPointerOverUI(StrataScriptContext* context)
		{
			return HostCall("UIIsPointerOverUI", false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "UIIsPointerOverUI");
				// A scene without the game UI has no UI under the pointer: an answer, not misuse.
				IUIControl* ui = system ? system->GetScene().GetService<IUIControl>() : nullptr;
				return ui && ui->IsPointerOverUI();
			});
		}

		template<typename Query>
		bool QueryUIAction(StrataScriptContext* context, uint32_t action, const char* function, Query&& query)
		{
			return HostCall(function, false, [&]()
			{
				ScriptSystem* system = ResolveContext(context, function);
				if (!system)
					return false;
				if (action > StrataScriptUIAction_Right)
				{
					system->ReportProblem(function, fmt::format("{} is not a UI action (0 to {})", action, static_cast<uint32_t>(StrataScriptUIAction_Right)));
					return false;
				}
				IUIControl* ui = system->GetScene().GetService<IUIControl>();
				return ui && query(*ui);
			});
		}

		bool HostUIIsActionPressed(StrataScriptContext* context, uint32_t action)
		{
			return QueryUIAction(context, action, "UIIsActionPressed", [action](IUIControl& ui) { return ui.IsActionPressed(action); });
		}

		bool HostUIIsActionDown(StrataScriptContext* context, uint32_t action)
		{
			return QueryUIAction(context, action, "UIIsActionDown", [action](IUIControl& ui) { return ui.IsActionDown(action); });
		}

		float HostGetUnscaledDeltaTime(StrataScriptContext* context)
		{
			return HostCall("GetUnscaledDeltaTime", 0.0f, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetUnscaledDeltaTime");
				return system ? system->GetScene().GetUnscaledDeltaTime() : 0.0f;
			});
		}

		double HostGetRealTime(StrataScriptContext* context)
		{
			return HostCall("GetRealTime", 0.0, [&]()
			{
				ScriptSystem* system = ResolveContext(context, "GetRealTime");
				return system ? system->GetScene().GetUnscaledTime() : 0.0;
			});
		}

		uint64_t HostGetTypedText(StrataScriptContext* context, char* buffer, uint64_t capacity)
		{
			return HostCall("GetTypedText", uint64_t(0), [&]() -> uint64_t
			{
				ScriptSystem* system = ResolveContext(context, "GetTypedText");
				return system ? CopyText(Input::GetTypedText(), buffer, capacity) : 0;
			});
		}

		////////////////////////////////////////////////////////////////////////////////
		// The table
		////////////////////////////////////////////////////////////////////////////////

		// Every function of StrataScriptHostAPI in declaration order, each implemented by Host<Name> above. CreateHostAPI
		// builds the table from this list, giving every function a call counter (except in Dist builds), and the
		// static_assert below fails the build when the list and the struct disagree: a function appended to the ABI has
		// to be listed here, and the feature test (StrataTests/FeatureTest) then requires its scripts to call it.
#define ST_SCRIPT_HOST_FUNCTIONS(X) \
	X(Log) \
	X(ReportException) \
	X(GetDeltaTime) \
	X(GetFixedDeltaTime) \
	X(GetElapsedTime) \
	X(GetFrameIndex) \
	X(GetTimeScale) \
	X(SetTimeScale) \
	X(CreateEntity) \
	X(DestroyEntity) \
	X(IsEntityValid) \
	X(FindEntityByName) \
	X(FindEntitiesByTag) \
	X(GetEntityName) \
	X(SetEntityName) \
	X(GetEntityTag) \
	X(SetEntityTag) \
	X(IsEntityActive) \
	X(IsEntityActiveInHierarchy) \
	X(SetEntityActive) \
	X(GetParent) \
	X(SetParent) \
	X(GetChildren) \
	X(HasComponent) \
	X(AddComponent) \
	X(RemoveComponent) \
	X(GetProperty) \
	X(SetProperty) \
	X(GetTransform) \
	X(SetTransform) \
	X(GetWorldTransform) \
	X(SetWorldTransform) \
	X(GetPrimaryCamera) \
	X(GetRootEntities) \
	X(Instantiate) \
	X(FindAsset) \
	X(IsAssetLoaded) \
	X(RequestAssetLoad) \
	X(GetScriptInstance) \
	X(HasScript) \
	X(AddScript) \
	X(RemoveScript) \
	X(IsKeyDown) \
	X(IsKeyPressed) \
	X(IsKeyReleased) \
	X(IsMouseButtonDown) \
	X(IsMouseButtonPressed) \
	X(IsMouseButtonReleased) \
	X(GetMousePosition) \
	X(GetMouseDelta) \
	X(GetScrollDelta) \
	X(GetLinearVelocity) \
	X(SetLinearVelocity) \
	X(GetAngularVelocity) \
	X(SetAngularVelocity) \
	X(AddForce) \
	X(AddForceAtPosition) \
	X(AddImpulse) \
	X(AddImpulseAtPosition) \
	X(AddTorque) \
	X(AddAngularImpulse) \
	X(Teleport) \
	X(Raycast) \
	X(RaycastAll) \
	X(OverlapSphere) \
	X(OverlapBox) \
	X(AudioPlay) \
	X(AudioPause) \
	X(AudioStop) \
	X(AudioIsPlaying) \
	X(AudioSeek) \
	X(AudioGetPlaybackPosition) \
	X(AudioPlayOneShot) \
	X(AudioPlayOneShotAt) \
	X(AudioSetMasterVolume) \
	X(AudioGetMasterVolume) \
	X(QuitGame) \
	X(LoadScene) \
	X(ReleaseAsset) \
	X(LoadSceneAdditive) \
	X(UnloadSubScene) \
	X(IsSubSceneLoaded) \
	X(GetSceneLoadProgress) \
	X(GetAssetLoadProgress) \
	X(InstantiateMany) \
	X(AnimatorPlay) \
	X(AnimatorPlayClip) \
	X(AnimatorCrossFade) \
	X(AnimatorSetFloat) \
	X(AnimatorSetBool) \
	X(AnimatorSetTrigger) \
	X(AnimatorResetTrigger) \
	X(AnimatorSetSpeed) \
	X(AnimatorGetCurrentState) \
	X(AnimatorGetNormalizedTime) \
	X(AnimatorIsInTransition) \
	X(AnimatorGetJointTransform) \
	X(CharacterMove) \
	X(CharacterJump) \
	X(CharacterTeleport) \
	X(CharacterGetState) \
	X(IsGamepadConnected) \
	X(IsGamepadButtonDown) \
	X(IsGamepadButtonPressed) \
	X(IsGamepadButtonReleased) \
	X(GetGamepadAxis) \
	X(SetCursorMode) \
	X(GetCursorMode) \
	X(SetWindowMode) \
	X(SetResolution) \
	X(SetVSync) \
	X(GetWindowSize) \
	X(WriteSaveFile) \
	X(ReadSaveFile) \
	X(DeleteSaveFile) \
	X(ListSaveFiles) \
	X(PhysicsSetLayersCollide) \
	X(PhysicsGetLayersCollide) \
	X(PhysicsGetLayerIndex) \
	X(UIGetText) \
	X(UISetText) \
	X(UIGetImage) \
	X(UISetImage) \
	X(UIGetColor) \
	X(UISetColor) \
	X(UIIsInteractable) \
	X(UISetInteractable) \
	X(UIGetValue) \
	X(UISetValue) \
	X(UIGetFocus) \
	X(UISetFocus) \
	X(UIIsPointerOverUI) \
	X(UIIsActionPressed) \
	X(UIIsActionDown) \
	X(GetUnscaledDeltaTime) \
	X(GetRealTime) \
	X(GetTypedText)

		struct HostFunctionEntry
		{
			std::string_view Name;
			size_t Offset = 0;
			size_t Size = 0;
		};

#define ST_SCRIPT_HOST_FUNCTION_ENTRY(Name) HostFunctionEntry { #Name, offsetof(StrataScriptHostAPI, Name), sizeof(StrataScriptHostAPI::Name) },
		constexpr HostFunctionEntry c_HostFunctions[] = { ST_SCRIPT_HOST_FUNCTIONS(ST_SCRIPT_HOST_FUNCTION_ENTRY) };
#undef ST_SCRIPT_HOST_FUNCTION_ENTRY

		// True when c_HostFunctions covers the struct after its header exactly: every member, in order, without gaps.
		constexpr bool ListsEveryHostFunction()
		{
			size_t expectedOffset = offsetof(StrataScriptHostAPI, ABIVersion) + sizeof(StrataScriptHostAPI::ABIVersion);
			for (const HostFunctionEntry& entry : c_HostFunctions)
			{
				if (entry.Offset != expectedOffset)
					return false;
				expectedOffset += entry.Size;
			}
			return expectedOffset == sizeof(StrataScriptHostAPI);
		}

		static_assert(ListsEveryHostFunction(), "ST_SCRIPT_HOST_FUNCTIONS must list every member of StrataScriptHostAPI after ABIVersion, in declaration order");

		// Call counting is a development diagnostic. Dist builds (shipped games) put the implementations into the table
		// directly: no host call pays for an atomic increment there, nor for threads (e.g. logging) contending on counters.
#if !defined(ST_DIST)
		constexpr size_t c_HostFunctionCount = std::size(c_HostFunctions);

		constexpr size_t GetHostFunctionIndex(size_t offset)
		{
			for (size_t index = 0; index < c_HostFunctionCount; index++)
			{
				if (c_HostFunctions[index].Offset == offset)
					return index;
			}
			return c_HostFunctionCount;
		}

		// Relaxed counters: Log may be called from any thread, and readers only need eventually consistent totals.
		std::array<std::atomic<uint64_t>, c_HostFunctionCount> s_HostCallCounts = {};

		// The table entry of a host function: counts the call, then forwards to the implementation.
		template<size_t Index, auto Function>
		struct CountedHostFunction;

		template<size_t Index, typename Result, typename... Arguments, Result (*Function)(Arguments...)>
		struct CountedHostFunction<Index, Function>
		{
			static_assert(Index < c_HostFunctionCount, "Host function missing from ST_SCRIPT_HOST_FUNCTIONS");

			static Result Call(Arguments... arguments)
			{
				s_HostCallCounts[Index].fetch_add(1, std::memory_order_relaxed);
				return Function(arguments...);
			}
		};
#endif

		StrataScriptHostAPI CreateHostAPI()
		{
			StrataScriptHostAPI api = {};
			api.StructSize = sizeof(StrataScriptHostAPI);
			api.ABIVersion = ST_SCRIPT_ABI_VERSION;
#if defined(ST_DIST)
	#define ST_SCRIPT_ASSIGN_HOST_FUNCTION(Name) api.Name = &Host##Name;
#else
	#define ST_SCRIPT_ASSIGN_HOST_FUNCTION(Name) api.Name = &CountedHostFunction<GetHostFunctionIndex(offsetof(StrataScriptHostAPI, Name)), &Host##Name>::Call;
#endif
			ST_SCRIPT_HOST_FUNCTIONS(ST_SCRIPT_ASSIGN_HOST_FUNCTION)
#undef ST_SCRIPT_ASSIGN_HOST_FUNCTION
			return api;
		}

#undef ST_SCRIPT_HOST_FUNCTIONS

	}

	const StrataScriptHostAPI& GetScriptHostAPI()
	{
		static const StrataScriptHostAPI s_HostAPI = CreateHostAPI();
		return s_HostAPI;
	}

	std::vector<ScriptHostFunctionCalls> GetScriptHostCallCounts()
	{
#if defined(ST_DIST)
		return {};
#else
		std::vector<ScriptHostFunctionCalls> calls;
		calls.reserve(c_HostFunctionCount);
		for (size_t index = 0; index < c_HostFunctionCount; index++)
			calls.push_back({ c_HostFunctions[index].Name, s_HostCallCounts[index].load(std::memory_order_relaxed) });
		return calls;
#endif
	}

	void ResetScriptHostCallCounts()
	{
#if !defined(ST_DIST)
		for (std::atomic<uint64_t>& count : s_HostCallCounts)
			count.store(0, std::memory_order_relaxed);
#endif
	}

	StrataScriptContext* RegisterScriptContext(ScriptSystem& system)
	{
		// The owner thread can only change hands while no context is alive.
		ST_CORE_VERIFY(s_Contexts.empty() || s_OwnerThread.load() == std::this_thread::get_id(), "Scenes playing scripts must all run on the same thread");
		s_OwnerThread.store(std::this_thread::get_id());
		s_Contexts.insert(&system);
		return reinterpret_cast<StrataScriptContext*>(&system);
	}

	void UnregisterScriptContext(ScriptSystem& system)
	{
		s_Contexts.erase(&system);
	}

}
