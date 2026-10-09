#include "Scripting/ScriptTestUtils.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"

namespace Strata::Tests
{

	std::filesystem::path GetTestScriptModule(std::string_view fileName)
	{
		return Platform::GetExecutableDirectory() / FileSystem::FromUTF8(fileName);
	}

	ScopedScriptEngine::ScopedScriptEngine()
		: m_Engine(CreateRef<ScriptEngine>()), m_Previous(ScriptEngine::GetActive())
	{
		ScriptEngine::SetActive(m_Engine);
	}

	ScopedScriptEngine::ScopedScriptEngine(const std::filesystem::path& module, bool enableHotReload)
		: ScopedScriptEngine()
	{
		m_Engine->SetHotReloadEnabled(enableHotReload);
		std::string error;
		REQUIRE_MESSAGE(m_Engine->LoadModule(module, &error), error);
	}

	ScopedScriptEngine::~ScopedScriptEngine()
	{
		ScriptEngine::SetActive(m_Previous);
	}

	ScriptSystem& GetScriptSystem(Scene& scene)
	{
		ScriptSystem* system = scene.GetSystem<ScriptSystem>();
		REQUIRE(system != nullptr);
		return *system;
	}

	ScriptEntry& AddScriptEntry(Entity entity, const std::string& className)
	{
		ScriptComponent& component = entity.HasComponent<ScriptComponent>() ? entity.GetComponent<ScriptComponent>() : entity.AddComponent<ScriptComponent>();
		ScriptEntry& entry = component.Scripts.emplace_back();
		entry.ClassName = className;
		return entry;
	}

	void AddFieldOverride(ScriptEntry& entry, const std::string& name, PropertyType type, PropertyValue value)
	{
		entry.Fields.push_back(ScriptFieldValue { name, type, std::move(value) });
	}

	Entity CreateLogEntity(Scene& scene)
	{
		Entity log = scene.CreateEntity("Log");
		log.AddComponent<TextComponent>().Text.clear();
		return log;
	}

	std::string GetLog(Scene& scene)
	{
		const Entity log = scene.FindEntityByName("Log");
		REQUIRE(log.IsValid());
		return log.GetComponent<TextComponent>().Text;
	}

	void ClearLog(Scene& scene)
	{
		Entity log = scene.FindEntityByName("Log");
		REQUIRE(log.IsValid());
		log.GetComponent<TextComponent>().Text.clear();
	}

	void CheckScriptChecks(const ScriptSystem& system, Entity entity, std::string_view className, int32_t minimumChecks)
	{
		const std::string failure = GetField<std::string>(system, entity, className, "Failure");
		const int32_t checks = GetField<int32_t>(system, entity, className, "Checks");
		INFO("Script ", std::string(className), " failed check: ", failure);
		CHECK(failure.empty());
		CHECK(checks >= minimumChecks);
	}

	void RunFrames(Scene& scene, int frames, float deltaTime)
	{
		for (int frame = 0; frame < frames; frame++)
			scene.OnUpdateRuntime(deltaTime);
	}

}
