#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/Property.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Callbacks a script class may implement (see StrataScript/Script.h for when each runs).
	enum class ScriptCallback : uint8_t
	{
		OnCreate = 0,
		OnUpdate,
		OnFixedUpdate,
		OnLateUpdate,
		OnDestroy,
		OnReload,
		// Contacts of the entity's physics body (see StrataScriptClassDesc and ScriptSystem).
		OnCollisionEnter,
		OnCollisionExit,
		OnTriggerEnter,
		OnTriggerExit
	};

	const char* ScriptCallbackToString(ScriptCallback callback);

	// A field declared by a script class (ST_SCRIPT_FIELD): shown in the inspector, overridden per entity through
	// ScriptEntry::Fields and kept across hot reloads.
	struct ScriptFieldInfo
	{
		std::string Name;
		PropertyType Type = PropertyType::Float; // Bool, Int, Float, Vec2, Vec3, Vec4, Quat, String, Entity or Asset
		PropertyValue DefaultValue;
	};

	// Metadata of a script class of the loaded module. Owned by the module: valid until it is unloaded or reloaded.
	struct ScriptClassInfo
	{
		std::string Name;
		std::vector<ScriptFieldInfo> Fields; // In declaration order
		uint32_t Index = 0;                  // Position in the module's class list
		uint32_t Callbacks = 0;              // Bit (1 << ScriptCallback) per implemented callback

		bool Implements(ScriptCallback callback) const { return (Callbacks & (1u << static_cast<uint32_t>(callback))) != 0; }
		// Field names are case-sensitive (they are C++ identifiers). Returns null / UINT32_MAX when absent.
		const ScriptFieldInfo* FindField(std::string_view name) const;
		uint32_t FindFieldIndex(std::string_view name) const;
	};

	// Describes a crash inside script code. After a fault the module is not called again until it is reloaded.
	struct ScriptFault
	{
		std::string ModuleName;
		std::string ClassName;   // Empty for module-level calls (loading, unloading)
		std::string Method;      // "OnUpdate", "Create", "StrataScript_Load", ...
		UUID Entity = UUID::Null();
		std::string EntityName;
		std::string Description; // E.g. "Access violation writing address 0x0000000000000000 at 0x..."
	};

}
