#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Physics.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strata
{

	namespace Detail
	{
		struct ClassRecord;
		struct ScriptAccess;
	}

	// Base class of game scripts. A script class is attached to entities through their Script component; the engine
	// creates one instance per entity and calls:
	//   OnCreate        once, before the first update (all scripts that exist at that moment are already constructed)
	//   OnUpdate        every frame
	//   OnFixedUpdate   zero or more times per frame, at the fixed timestep (gameplay physics)
	//   OnLateUpdate    every frame, after the fixed updates
	//   OnDestroy       when the entity is destroyed, the script is removed or the scene stops playing
	//   OnReload        after a hot reload, instead of OnCreate (see below)
	//   OnCollisionEnter / OnCollisionExit, OnTriggerEnter / OnTriggerExit
	//                   when the entity's physics body starts or stops touching another body (see below)
	// Instances update in entity hierarchy order (parents first), then in the order of the scripts on the entity.
	// Inactive entities receive no updates. Overrides may be public, protected or private.
	//
	// Hot reload: when the module is rebuilt while the game runs, each instance is deleted (its destructor runs, but
	// not OnDestroy) and a new instance of the new code is constructed. Field values (see ST_SCRIPT_FIELD) carry over
	// when the new class still has a field with the same name and type; other state starts over. OnReload then runs
	// instead of OnCreate, so re-acquire anything that is not a field (cached entities, script pointers) there.
	//
	// Contacts: scripts on an entity that owns a physics body (its RigidBody entity; colliders on descendants belong to it)
	// learn when the body starts touching another body and when they part (or one leaves the simulation), after the fixed
	// step that found it. The trigger callbacks report contacts where either body is a trigger (no collision response), the
	// collision callbacks all others. Both entities' scripts are called. Every Enter a script got is followed by one Exit,
	// also when the entity was deactivated meanwhile (pooled objects end their contacts that way); a script added during a
	// contact gets neither. Destroying entities in these callbacks is safe: destruction happens at the end of the frame.
	//
	// Script classes must be default constructible; the module constructs one instance when it loads to read the field
	// defaults. Do gameplay initialization in OnCreate, not in the constructor.
	class Script
	{
	public:
		Script()
			: m_Entity(Detail::s_ConstructingEntity), m_Context(Detail::s_Context)
		{
		}

		virtual ~Script() = default;

		Script(const Script&) = delete;
		Script& operator=(const Script&) = delete;

		virtual void OnCreate() {}
		virtual void OnUpdate([[maybe_unused]] float deltaTime) {}
		virtual void OnFixedUpdate([[maybe_unused]] float fixedDeltaTime) {}
		virtual void OnLateUpdate([[maybe_unused]] float deltaTime) {}
		virtual void OnDestroy() {}
		virtual void OnReload() {}
		virtual void OnCollisionEnter([[maybe_unused]] const Collision& collision) {}
		virtual void OnCollisionExit([[maybe_unused]] const Collision& collision) {}
		virtual void OnTriggerEnter([[maybe_unused]] const Collision& collision) {}
		virtual void OnTriggerExit([[maybe_unused]] const Collision& collision) {}

		// The entity this script is attached to.
		Entity GetEntity() const { return m_Entity; }
		TransformComponent GetTransform() const { return m_Entity.GetTransform(); }
	private:
		Entity m_Entity;
		StrataScriptContext* m_Context = nullptr;
		const Detail::ClassRecord* m_Class = nullptr;

		friend struct Detail::ScriptAccess;
	};

	template<typename T>
	class ScriptClassBuilder;

	namespace Detail
	{

		struct ScriptAccess
		{
			static StrataScriptContext* GetInstanceContext(const Script& script) { return script.m_Context; }
			static const ClassRecord* GetClass(const Script& script) { return script.m_Class; }
			static void SetClass(Script& script, const ClassRecord* record) { script.m_Class = record; }
		};

		template<typename T>
		struct MemberPointerTraits;

		template<typename Class, typename Member>
		struct MemberPointerTraits<Member Class::*>
		{
			using ClassType = Class;
			using MemberType = Member;
		};

		// Types scripts can declare as fields.
		template<typename T>
		inline constexpr bool c_IsFieldType = std::is_same_v<T, bool> || std::is_same_v<T, int32_t> || std::is_same_v<T, float>
			|| std::is_same_v<T, glm::vec2> || std::is_same_v<T, glm::vec3> || std::is_same_v<T, glm::vec4> || std::is_same_v<T, glm::quat>
			|| std::is_same_v<T, std::string> || std::is_same_v<T, Entity> || std::is_same_v<T, AssetHandle>;

		struct FieldBinding
		{
			std::string Name;
			uint32_t Type = StrataScriptValueType_Empty;
			// String values point into the script.
			void (*Get)(const Script& script, StrataScriptValue& outValue) = nullptr;
			// False if the value has the wrong type (the field is unchanged).
			bool (*Set)(Script& script, const StrataScriptValue& value) = nullptr;

			std::string DefaultString; // Storage for a string default value
			StrataScriptFieldDesc Desc = {};
		};

		struct ClassRecord
		{
			std::string Name;
			std::vector<FieldBinding> Fields;
			std::vector<const StrataScriptFieldDesc*> FieldDescs;
			StrataScriptClassDesc Desc = {};
		};

		template<typename T, auto Member>
		void GetFieldValue(const Script& script, StrataScriptValue& outValue)
		{
			using MemberType = typename MemberPointerTraits<decltype(Member)>::MemberType;
			outValue = ValueTraits<MemberType>::ToValue(static_cast<const T&>(script).*Member);
		}

		template<typename T, auto Member>
		bool SetFieldValue(Script& script, const StrataScriptValue& value)
		{
			using MemberType = typename MemberPointerTraits<decltype(Member)>::MemberType;
			MemberType converted {};
			if (!ValueTraits<MemberType>::FromValue(value, converted))
				return false;
			static_cast<T&>(script).*Member = std::move(converted);
			return true;
		}

		inline void ReportException(const char* message)
		{
			if (s_Host)
				s_Host->ReportException(ToABIString(message ? std::string_view(message) : std::string_view("Unknown exception")));
		}

		// Runs script code with `context` current, turning C++ exceptions into StrataScriptResult_Exception.
		template<typename Function>
		uint32_t InvokeScript(StrataScriptContext* context, Function&& function)
		{
			ContextScope scope(context);
			try
			{
				function();
				return StrataScriptResult_Ok;
			}
			catch (const std::exception& exception)
			{
				ReportException(exception.what());
			}
			catch (...)
			{
				ReportException("Unknown exception (not derived from std::exception)");
			}
			return StrataScriptResult_Exception;
		}

		inline Script* ToScript(StrataScriptInstance instance)
		{
			return static_cast<Script*>(instance);
		}

		inline uint32_t DestroyThunk(StrataScriptInstance instance)
		{
			Script* script = ToScript(instance);
			if (!script)
				return StrataScriptResult_InvalidArgument;
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { delete script; });
		}

		inline uint32_t GetFieldThunk(StrataScriptInstance instance, uint32_t fieldIndex, StrataScriptValue* outValue)
		{
			Script* script = ToScript(instance);
			const ClassRecord* record = script ? ScriptAccess::GetClass(*script) : nullptr;
			if (!record || !outValue || fieldIndex >= record->Fields.size())
				return StrataScriptResult_InvalidArgument;
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { record->Fields[fieldIndex].Get(*script, *outValue); });
		}

		inline uint32_t SetFieldThunk(StrataScriptInstance instance, uint32_t fieldIndex, const StrataScriptValue* value)
		{
			Script* script = ToScript(instance);
			const ClassRecord* record = script ? ScriptAccess::GetClass(*script) : nullptr;
			if (!record || !value || fieldIndex >= record->Fields.size())
				return StrataScriptResult_InvalidArgument;
			bool accepted = false;
			const uint32_t result = InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { accepted = record->Fields[fieldIndex].Set(*script, *value); });
			if (result != StrataScriptResult_Ok)
				return result;
			return accepted ? StrataScriptResult_Ok : StrataScriptResult_InvalidArgument;
		}

		inline uint32_t OnCreateThunk(StrataScriptInstance instance)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnCreate(); });
		}

		inline uint32_t OnUpdateThunk(StrataScriptInstance instance, float deltaTime)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnUpdate(deltaTime); });
		}

		inline uint32_t OnFixedUpdateThunk(StrataScriptInstance instance, float fixedDeltaTime)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnFixedUpdate(fixedDeltaTime); });
		}

		inline uint32_t OnLateUpdateThunk(StrataScriptInstance instance, float deltaTime)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnLateUpdate(deltaTime); });
		}

		inline uint32_t OnDestroyThunk(StrataScriptInstance instance)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnDestroy(); });
		}

		inline uint32_t OnReloadThunk(StrataScriptInstance instance)
		{
			Script* script = ToScript(instance);
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { script->OnReload(); });
		}

		// The engine's contact, if it is complete (it is engine memory: members appended later are read only when its
		// StructSize covers them).
		inline bool ReadCollision(const StrataScriptCollision* collision, Collision& out)
		{
			if (!collision || collision->StructSize < offsetof(StrataScriptCollision, Normal) + sizeof(collision->Normal))
				return false;
			out.Other = Entity(collision->Other);
			out.Point = FromABIVector3(collision->Point);
			out.Normal = FromABIVector3(collision->Normal);
			return true;
		}

		template<void (Script::*Callback)(const Collision&)>
		uint32_t ContactThunk(StrataScriptInstance instance, const StrataScriptCollision* collision)
		{
			Script* script = ToScript(instance);
			Collision contact;
			if (!script || !ReadCollision(collision, contact))
				return StrataScriptResult_InvalidArgument;
			return InvokeScript(ScriptAccess::GetInstanceContext(*script), [&]() { (script->*Callback)(contact); });
		}

		// Per script class: its registered name and, while the module is loaded, its record.
		template<typename T>
		struct ClassSlot
		{
			static inline const char* Name = nullptr;
			static inline const ClassRecord* Record = nullptr;
			static inline void (*Describe)(ScriptClassBuilder<T>&) = nullptr;
		};

		// Which callbacks a class overrides; the engine skips the others entirely.
		enum CallbackFlag : uint32_t
		{
			CallbackFlag_OnCreate = 1u << 0,
			CallbackFlag_OnUpdate = 1u << 1,
			CallbackFlag_OnFixedUpdate = 1u << 2,
			CallbackFlag_OnLateUpdate = 1u << 3,
			CallbackFlag_OnDestroy = 1u << 4,
			CallbackFlag_OnReload = 1u << 5,
			CallbackFlag_OnCollisionEnter = 1u << 6,
			CallbackFlag_OnCollisionExit = 1u << 7,
			CallbackFlag_OnTriggerEnter = 1u << 8,
			CallbackFlag_OnTriggerExit = 1u << 9
		};

		// Whether T overrides a callback. `&T::OnUpdate` names Script::OnUpdate (type `void (Script::*)(float)`) unless T,
		// or a base between T and Script, declares OnUpdate. When that declaration is not accessible here (a private or
		// protected override), the expression is invalid: the class then declares the callback itself.
		template<typename T>
		constexpr bool OverridesOnCreate()
		{
			if constexpr (requires { &T::OnCreate; })
				return !std::is_same_v<decltype(&T::OnCreate), void (Script::*)()>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnUpdate()
		{
			if constexpr (requires { &T::OnUpdate; })
				return !std::is_same_v<decltype(&T::OnUpdate), void (Script::*)(float)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnFixedUpdate()
		{
			if constexpr (requires { &T::OnFixedUpdate; })
				return !std::is_same_v<decltype(&T::OnFixedUpdate), void (Script::*)(float)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnLateUpdate()
		{
			if constexpr (requires { &T::OnLateUpdate; })
				return !std::is_same_v<decltype(&T::OnLateUpdate), void (Script::*)(float)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnDestroy()
		{
			if constexpr (requires { &T::OnDestroy; })
				return !std::is_same_v<decltype(&T::OnDestroy), void (Script::*)()>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnReload()
		{
			if constexpr (requires { &T::OnReload; })
				return !std::is_same_v<decltype(&T::OnReload), void (Script::*)()>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnCollisionEnter()
		{
			if constexpr (requires { &T::OnCollisionEnter; })
				return !std::is_same_v<decltype(&T::OnCollisionEnter), void (Script::*)(const Collision&)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnCollisionExit()
		{
			if constexpr (requires { &T::OnCollisionExit; })
				return !std::is_same_v<decltype(&T::OnCollisionExit), void (Script::*)(const Collision&)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnTriggerEnter()
		{
			if constexpr (requires { &T::OnTriggerEnter; })
				return !std::is_same_v<decltype(&T::OnTriggerEnter), void (Script::*)(const Collision&)>;
			else
				return true;
		}

		template<typename T>
		constexpr bool OverridesOnTriggerExit()
		{
			if constexpr (requires { &T::OnTriggerExit; })
				return !std::is_same_v<decltype(&T::OnTriggerExit), void (Script::*)(const Collision&)>;
			else
				return true;
		}

		template<typename T>
		constexpr uint32_t GetCallbackFlags()
		{
			uint32_t flags = 0;
			if constexpr (OverridesOnCreate<T>())
				flags |= CallbackFlag_OnCreate;
			if constexpr (OverridesOnUpdate<T>())
				flags |= CallbackFlag_OnUpdate;
			if constexpr (OverridesOnFixedUpdate<T>())
				flags |= CallbackFlag_OnFixedUpdate;
			if constexpr (OverridesOnLateUpdate<T>())
				flags |= CallbackFlag_OnLateUpdate;
			if constexpr (OverridesOnDestroy<T>())
				flags |= CallbackFlag_OnDestroy;
			if constexpr (OverridesOnReload<T>())
				flags |= CallbackFlag_OnReload;
			if constexpr (OverridesOnCollisionEnter<T>())
				flags |= CallbackFlag_OnCollisionEnter;
			if constexpr (OverridesOnCollisionExit<T>())
				flags |= CallbackFlag_OnCollisionExit;
			if constexpr (OverridesOnTriggerEnter<T>())
				flags |= CallbackFlag_OnTriggerEnter;
			if constexpr (OverridesOnTriggerExit<T>())
				flags |= CallbackFlag_OnTriggerExit;
			return flags;
		}

		// A registered class. Static-lifetime and trivially destructible: registration happens during static
		// initialization of the module and must not depend on anything else.
		struct ClassRegistration
		{
			const char* Name = nullptr;
			uint32_t CallbackFlags = 0;
			void (*Describe)(ClassRecord& record) = nullptr;
			Script* (*Construct)() = nullptr;
			uint32_t (*Create)(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptInstance* outInstance) = nullptr;
			const ClassRecord** RecordSlot = nullptr;
			ClassRegistration* Next = nullptr;
		};

		inline ClassRegistration* s_FirstRegistration = nullptr;
		inline ClassRegistration* s_LastRegistration = nullptr;

		template<typename T>
		uint32_t CreateThunk(StrataScriptContext* context, StrataScriptEntityID entity, StrataScriptInstance* outInstance)
		{
			if (!outInstance)
				return StrataScriptResult_InvalidArgument;
			*outInstance = nullptr;

			const StrataScriptEntityID previousEntity = s_ConstructingEntity;
			s_ConstructingEntity = entity;
			T* created = nullptr;
			const uint32_t result = InvokeScript(context, [&]() { created = new T(); });
			s_ConstructingEntity = previousEntity;
			if (result != StrataScriptResult_Ok)
				return result;

			ScriptAccess::SetClass(*created, ClassSlot<T>::Record);
			*outInstance = static_cast<Script*>(created);
			return StrataScriptResult_Ok;
		}

		template<typename T>
		Script* ConstructDefault()
		{
			return new T();
		}

		template<typename T>
		void DescribeClass(ClassRecord& record);

		template<typename T>
		class ClassRegistrar
		{
		public:
			ClassRegistrar(const char* name, void (*describe)(ScriptClassBuilder<T>&))
			{
				static_assert(std::is_base_of_v<Script, T>, "Script classes must derive from Strata::Script");
				static_assert(std::is_default_constructible_v<T>, "Script classes must be default constructible");

				ClassSlot<T>::Name = name;
				ClassSlot<T>::Describe = describe;
				m_Registration.Name = name;
				m_Registration.CallbackFlags = GetCallbackFlags<T>();
				m_Registration.Describe = &DescribeClass<T>;
				m_Registration.Construct = &ConstructDefault<T>;
				m_Registration.Create = &CreateThunk<T>;
				m_Registration.RecordSlot = &ClassSlot<T>::Record;

				if (s_LastRegistration)
					s_LastRegistration->Next = &m_Registration;
				else
					s_FirstRegistration = &m_Registration;
				s_LastRegistration = &m_Registration;
			}

			ClassRegistrar(const ClassRegistrar&) = delete;
			ClassRegistrar& operator=(const ClassRegistrar&) = delete;
		private:
			ClassRegistration m_Registration;
		};

		struct ModuleState
		{
			std::string Name;
			std::vector<std::unique_ptr<ClassRecord>> Classes;
			std::vector<const StrataScriptClassDesc*> ClassDescs;
		};

		inline ModuleState* s_Module = nullptr;

		inline uint32_t UnloadModule()
		{
			for (ClassRegistration* registration = s_FirstRegistration; registration; registration = registration->Next)
				*registration->RecordSlot = nullptr;
			delete s_Module;
			s_Module = nullptr;
			s_Host = nullptr;
			return StrataScriptResult_Ok;
		}

		// Builds the record of one registered class: its fields with default values read from a default-constructed
		// instance, and the ABI descriptor.
		inline std::unique_ptr<ClassRecord> BuildClassRecord(const ClassRegistration& registration)
		{
			auto record = std::make_unique<ClassRecord>();
			record->Name = registration.Name;
			registration.Describe(*record);

			const std::unique_ptr<Script> defaults(registration.Construct());
			for (FieldBinding& field : record->Fields)
			{
				StrataScriptValue value = {};
				field.Get(*defaults, value);
				if (value.Type == StrataScriptValueType_String)
				{
					field.DefaultString = std::string(FromABIString(value.As.String));
					value.As.String = ToABIString(field.DefaultString);
				}
				field.Desc.StructSize = sizeof(StrataScriptFieldDesc);
				field.Desc.Type = field.Type;
				field.Desc.Name = ToABIString(field.Name);
				field.Desc.DefaultValue = value;
			}
			for (const FieldBinding& field : record->Fields)
				record->FieldDescs.push_back(&field.Desc);

			const uint32_t flags = registration.CallbackFlags;
			StrataScriptClassDesc& desc = record->Desc;
			desc.StructSize = sizeof(StrataScriptClassDesc);
			desc.FieldCount = static_cast<uint32_t>(record->Fields.size());
			desc.Name = ToABIString(record->Name);
			desc.Fields = record->FieldDescs.data();
			desc.Create = registration.Create;
			desc.Destroy = &DestroyThunk;
			desc.GetField = &GetFieldThunk;
			desc.SetField = &SetFieldThunk;
			desc.OnCreate = (flags & CallbackFlag_OnCreate) ? &OnCreateThunk : nullptr;
			desc.OnUpdate = (flags & CallbackFlag_OnUpdate) ? &OnUpdateThunk : nullptr;
			desc.OnFixedUpdate = (flags & CallbackFlag_OnFixedUpdate) ? &OnFixedUpdateThunk : nullptr;
			desc.OnLateUpdate = (flags & CallbackFlag_OnLateUpdate) ? &OnLateUpdateThunk : nullptr;
			desc.OnDestroy = (flags & CallbackFlag_OnDestroy) ? &OnDestroyThunk : nullptr;
			desc.OnReload = (flags & CallbackFlag_OnReload) ? &OnReloadThunk : nullptr;
			desc.OnCollisionEnter = (flags & CallbackFlag_OnCollisionEnter) ? &ContactThunk<&Script::OnCollisionEnter> : nullptr;
			desc.OnCollisionExit = (flags & CallbackFlag_OnCollisionExit) ? &ContactThunk<&Script::OnCollisionExit> : nullptr;
			desc.OnTriggerEnter = (flags & CallbackFlag_OnTriggerEnter) ? &ContactThunk<&Script::OnTriggerEnter> : nullptr;
			desc.OnTriggerExit = (flags & CallbackFlag_OnTriggerExit) ? &ContactThunk<&Script::OnTriggerExit> : nullptr;
			return record;
		}

		// Whether the engine's StrataScriptModuleAPI (its size is in StructSize) holds every member of this ABI version.
		inline bool CanReceiveModuleAPI(const StrataScriptModuleAPI* outModule)
		{
			return outModule->StructSize >= offsetof(StrataScriptModuleAPI, Unload) + sizeof(outModule->Unload);
		}

		// Hands a module description (a StrataScriptModuleAPI, possibly with members of a newer SDK appended) to the engine.
		// The engine's struct may be smaller (an engine built against an older SDK of the same ABI version): at most its
		// size (StructSize on entry) is written, and StructSize then holds `descriptionSize`, the members the module knows.
		// Writes nothing and returns false when the engine's struct cannot hold the members of this ABI version.
		inline bool WriteModuleAPI(StrataScriptModuleAPI* outModule, const void* description, uint32_t descriptionSize)
		{
			if (!CanReceiveModuleAPI(outModule) || descriptionSize < sizeof(StrataScriptModuleAPI))
				return false;
			const uint32_t capacity = outModule->StructSize;
			std::memcpy(outModule, description, capacity < descriptionSize ? capacity : descriptionSize);
			outModule->StructSize = descriptionSize;
			return true;
		}

		// Implementation of StrataScript_Load (see ScriptModuleEntry.cpp).
		inline uint32_t LoadModule(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule, const char* moduleName)
		{
			if (!host || !outModule)
				return StrataScriptResult_InvalidArgument;
			// Every function of this ABI version must be present (later additions are checked where they are used), and the
			// engine's module description must have room for every member of this ABI version.
			if (hostABIVersion != ST_SCRIPT_ABI_VERSION || host->ABIVersion != ST_SCRIPT_ABI_VERSION
				|| !ST_SCRIPT_HAS_MEMBER(StrataScriptHostAPI, host, GetScrollDelta) || !CanReceiveModuleAPI(outModule))
				return StrataScriptResult_ABIMismatch;

			if (s_Module)
				UnloadModule();
			s_Host = host;

			try
			{
				auto state = std::make_unique<ModuleState>();
				state->Name = moduleName ? moduleName : "";
				for (ClassRegistration* registration = s_FirstRegistration; registration; registration = registration->Next)
				{
					std::unique_ptr<ClassRecord> record = BuildClassRecord(*registration);
					*registration->RecordSlot = record.get();
					state->ClassDescs.push_back(&record->Desc);
					state->Classes.push_back(std::move(record));
				}

				StrataScriptModuleAPI description = {};
				description.StructSize = sizeof(StrataScriptModuleAPI);
				description.ABIVersion = ST_SCRIPT_ABI_VERSION;
				description.Name = ToABIString(state->Name);
				description.ClassCount = static_cast<uint32_t>(state->ClassDescs.size());
				description.Classes = state->ClassDescs.data();
				description.Unload = &UnloadModule;
				s_Module = state.release();
				WriteModuleAPI(outModule, &description, sizeof(description)); // The capacity was checked above
				return StrataScriptResult_Ok;
			}
			catch (const std::exception& exception)
			{
				ReportException(exception.what());
			}
			catch (...)
			{
				ReportException("Unknown exception while loading the script module");
			}
			for (ClassRegistration* registration = s_FirstRegistration; registration; registration = registration->Next)
				*registration->RecordSlot = nullptr;
			s_Host = nullptr;
			return StrataScriptResult_Exception;
		}

	}

	// Declares the fields of a script class (see ST_SCRIPT_CLASS and ST_SCRIPT_FIELD).
	template<typename T>
	class ScriptClassBuilder
	{
	public:
		using ClassType = T;

		explicit ScriptClassBuilder(Detail::ClassRecord& record)
			: m_Record(record)
		{
		}

		// Registers a public data member as a field. Its value in a default-constructed instance is the default.
		template<auto Member>
		ScriptClassBuilder& Field(std::string_view name)
		{
			using Traits = Detail::MemberPointerTraits<decltype(Member)>;
			using MemberType = typename Traits::MemberType;
			static_assert(std::is_base_of_v<typename Traits::ClassType, T>, "The field must be a member of the script class or its bases");
			static_assert(Detail::c_IsFieldType<MemberType>,
				"Unsupported script field type: use bool, int32_t, float, glm::vec2/3/4, glm::quat, std::string, Strata::Entity or Strata::AssetHandle");

			Detail::FieldBinding& field = m_Record.Fields.emplace_back();
			field.Name = std::string(name);
			field.Type = Detail::ValueTraits<MemberType>::Type;
			field.Get = &Detail::GetFieldValue<T, Member>;
			field.Set = &Detail::SetFieldValue<T, Member>;
			return *this;
		}
	private:
		Detail::ClassRecord& m_Record;
	};

	namespace Detail
	{

		template<typename T>
		void DescribeClass(ClassRecord& record)
		{
			ScriptClassBuilder<T> builder(record);
			if (ClassSlot<T>::Describe)
				ClassSlot<T>::Describe(builder);
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Entity script access
	////////////////////////////////////////////////////////////////////////////////

	template<typename T>
	T* Entity::GetScript() const
	{
		static_assert(std::is_base_of_v<Script, T>, "GetScript requires a script class");
		const StrataScriptHostAPI* host = Detail::GetHost();
		const char* className = Detail::ClassSlot<T>::Name;
		if (!host || m_ID == 0 || !className)
			return nullptr;

		// The engine only returns instances of the class with this name, which is T (class names are unique).
		StrataScriptInstance instance = host->GetScriptInstance(Detail::GetContext(), m_ID, Detail::ToABIString(className));
		return instance ? static_cast<T*>(static_cast<Script*>(instance)) : nullptr;
	}

	template<typename T>
	T* Entity::AddScript()
	{
		static_assert(std::is_base_of_v<Script, T>, "AddScript requires a script class");
		const char* className = Detail::ClassSlot<T>::Name;
		if (!className || !AddScript(std::string_view(className)))
			return nullptr;
		return GetScript<T>();
	}

}

// Registers a script class and declares its fields. Use once per class, in a .cpp file:
//
//   ST_SCRIPT_CLASS(Player)
//   {
//       ST_SCRIPT_FIELD(Speed);
//       ST_SCRIPT_FIELD(Target);
//   }
//
// The class name used by Script components is the spelled type name ("Player", or "Game::Player" for
// ST_SCRIPT_CLASS(Game::Player)). A class without fields is registered with an empty body: ST_SCRIPT_CLASS(Spinner) {}
#define ST_SCRIPT_CLASS(Type) ST_SCRIPT_DETAIL_CLASS(Type, __COUNTER__)

// Declares a public data member of the class as a field: shown in the editor, serialized with the scene and kept
// across hot reloads. Supported types: bool, int32_t, float, glm::vec2/3/4, glm::quat, std::string, Strata::Entity,
// Strata::AssetHandle.
#define ST_SCRIPT_FIELD(Member) \
	stScriptBuilder.template Field<&std::remove_reference_t<decltype(stScriptBuilder)>::ClassType::Member>(#Member)

// The registrar is not const: registering the next class links it into this one (ClassRegistration::Next).
#define ST_SCRIPT_DETAIL_CLASS(Type, Counter) ST_SCRIPT_DETAIL_CLASS_IMPL(Type, Counter)
#define ST_SCRIPT_DETAIL_CLASS_IMPL(Type, Counter) \
	static void StrataScriptDescribe##Counter(::Strata::ScriptClassBuilder<Type>& stScriptBuilder); \
	static ::Strata::Detail::ClassRegistrar<Type> s_StrataScriptRegistrar##Counter(#Type, &StrataScriptDescribe##Counter); \
	static void StrataScriptDescribe##Counter([[maybe_unused]] ::Strata::ScriptClassBuilder<Type>& stScriptBuilder)
