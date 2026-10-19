#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/Property.h"

#include <cstdint>
#include <optional>
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
		OnTriggerExit,
		// Events of the entity's animator (see StrataScriptClassDesc and ScriptSystem).
		OnAnimationEvent,
		// Events of the game UI's widgets on the entity or naming it as their event target.
		OnUIClick,
		OnUIValueChanged,
		OnUIFocusChanged,
		OnUISubmit
	};

	// How many callbacks there are: code that lists or counts every callback (script.status) goes up to this, so callbacks
	// appended to the enum are included without edits there. Defined from the last value: keep it pointing at the last one.
	constexpr size_t c_ScriptCallbackCount = static_cast<size_t>(ScriptCallback::OnUISubmit) + 1;

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

	// A place in native code, as far as the module's debug information tells (ScriptSymbolizer): the function and, where
	// the module has a PDB (Windows), the source file and line.
	struct ScriptCodeLocation
	{
		uint64_t Address = 0;      // In this process
		std::string Module;        // The file name of the module holding it (empty when no module does)
		uint64_t ModuleOffset = 0; // From the module's base
		std::string Function;      // Empty when unknown
		std::string File;          // Empty when unknown (no debug information; POSIX reads none)
		uint32_t Line = 0;
		// A call the compiler inlined at Address (optimized builds, Windows): the frames that follow it with the same address
		// are the functions it was inlined into, the last of them the function the code belongs to.
		bool Inlined = false;
	};

	// "C:/Game/Scripts/Player.cpp(42) in Player::OnUpdate", or "GameScripts.dll+0x1A2B in ..." without a file.
	std::string DescribeScriptCodeLocation(const ScriptCodeLocation& location);

	// Describes a crash inside script code. After a fault the module is not called again until it is reloaded.
	struct ScriptFault
	{
		std::string ModuleName;
		std::string ClassName;   // Empty for module-level calls (loading, unloading)
		std::string Method;      // "OnUpdate", "Create", "StrataScript_Load", ...
		UUID Entity = UUID::Null();
		std::string EntityName;
		std::string Description; // E.g. "Access violation writing address 0x0000000000000000 at 0x..."
		// Where in the module's code it happened (the first frame of the stack inside the module), and the stack from the
		// fault outwards (frames of the module and the engine). Filled in by symbolization after the fault (ScriptEngine
		// polls it, see ScriptEngine::Update); Symbolized tells that it is done.
		std::optional<ScriptCodeLocation> Location;
		std::vector<ScriptCodeLocation> Stack;
		bool Symbolized = false;
	};

}
