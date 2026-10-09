#pragma once

#include <doctest/doctest.h>

#include "Strata/Core/Base.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Reflection/Property.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Strata::Tests
{

	// A test script module built next to the test executable (see StrataTests/CMakeLists.txt).
	std::filesystem::path GetTestScriptModule(std::string_view fileName);

	// A script engine that is the active engine (used by scenes that start playing) for the lifetime of this object.
	class ScopedScriptEngine
	{
	public:
		ScopedScriptEngine();
		// Loads the module (REQUIREs success). With hot reload enabled first, the module runs from a private copy, so tests
		// can overwrite the file ("rebuild" it) while it is loaded.
		explicit ScopedScriptEngine(const std::filesystem::path& module, bool enableHotReload = false);
		~ScopedScriptEngine();

		ScopedScriptEngine(const ScopedScriptEngine&) = delete;
		ScopedScriptEngine& operator=(const ScopedScriptEngine&) = delete;

		ScriptEngine& operator*() const { return *m_Engine; }
		ScriptEngine* operator->() const { return m_Engine.get(); }
		const Ref<ScriptEngine>& Get() const { return m_Engine; }
	private:
		Ref<ScriptEngine> m_Engine;
		Ref<ScriptEngine> m_Previous;
	};

	// Live instances of the API test module's counted scripts (ScriptTests::CountedScript), read through the module's
	// export. Holds a reference to the module's library while it exists.
	class LiveInstanceCounter
	{
	public:
		// The engine's loaded module must be the API test module (REQUIREd).
		explicit LiveInstanceCounter(const ScriptEngine& engine);

		int64_t Get() const;
	private:
		DynamicLibrary m_Library;
		int64_t (*m_GetCount)() = nullptr;
	};

	// The script system of a playing scene (REQUIREs one).
	ScriptSystem& GetScriptSystem(Scene& scene);

	// Adds a script entry to an entity's Script component (creating the component if needed).
	ScriptEntry& AddScriptEntry(Entity entity, const std::string& className);
	void AddFieldOverride(ScriptEntry& entry, const std::string& name, PropertyType type, PropertyValue value);

	// The tests' event recorder: an entity named "Log" whose Text scripts append "<entity>.<script>.<event>;" to.
	Entity CreateLogEntity(Scene& scene);
	std::string GetLog(Scene& scene);
	void ClearLog(Scene& scene);

	// The value of a field of a live script instance (REQUIREs that it exists and has type T).
	template<typename T>
	T GetField(const ScriptSystem& system, Entity entity, std::string_view className, std::string_view field)
	{
		const std::optional<PropertyValue> value = system.GetFieldValue(entity, className, field);
		INFO("Field ", std::string(className), ".", std::string(field));
		REQUIRE(value.has_value());
		REQUIRE(std::holds_alternative<T>(*value));
		return std::get<T>(*value);
	}

	// Checks of CheckingScript-based test scripts: every check passed and at least `minimumChecks` ran.
	void CheckScriptChecks(const ScriptSystem& system, Entity entity, std::string_view className, int32_t minimumChecks);

	// Advances a playing scene by `frames` updates of `deltaTime` seconds.
	void RunFrames(Scene& scene, int frames, float deltaTime = 1.0f / 60.0f);

}
