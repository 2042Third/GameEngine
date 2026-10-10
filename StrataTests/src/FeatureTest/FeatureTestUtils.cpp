#include "FeatureTest/FeatureTestUtils.h"

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Input/Input.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "Strata/Scripting/ScriptTypes.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <variant>

namespace Strata::Tests
{

	namespace
	{

		struct ExpectedLogMessage
		{
			LogLevel Level = LogLevel::Info;
			std::string_view Text;
			bool Exact = true; // Otherwise the message only has to contain Text
		};

		// Messages the feature scripts log on purpose (LogFeatures and ExceptionProbe in StrataTests/FeatureTest/Scripts).
		constexpr ExpectedLogMessage c_ExpectedLogMessages[] = {
			{ LogLevel::Trace, "FeatureTest trace: 1" },
			{ LogLevel::Info, "FeatureTest info: text 2.5 false" },
			{ LogLevel::Warn, "FeatureTest expected warning: true, 7, 0.5, (1, 2), (1, 2, 3), (1, 2, 3, 4), quat(0, 0, 0, 1), Entity(42), Asset(43)" },
			{ LogLevel::Error, "FeatureTest expected error: scripts report errors" },
			{ LogLevel::Error, "threw an exception in OnUpdate: FeatureTest: deliberate exception. The script is disabled.", false },
		};

		bool Matches(const LogEntry& entry, const ExpectedLogMessage& expected)
		{
			if (entry.Logger != "Script" || entry.Level != expected.Level)
				return false;
			return expected.Exact ? entry.Message == expected.Text : entry.Message.find(expected.Text) != std::string::npos;
		}

		Entity RequireEntity(Scene& scene, std::string_view name)
		{
			const Entity entity = scene.FindEntityByName(name);
			INFO("Entity '", std::string(name), "' of the feature scene");
			REQUIRE(entity.IsValid());
			return entity;
		}

		// The input InputFeatures expects around its press frame (see InputFeatures.cpp).
		void SimulateInput(int32_t frame, int32_t pressFrame)
		{
			if (frame == pressFrame - 1)
			{
				Input::ProcessMouseMove({ 100.0f, 60.0f });
			}
			else if (frame == pressFrame)
			{
				Input::ProcessKey(Key::Space, true);
				Input::ProcessMouseButton(Mouse::ButtonLeft, true);
				Input::ProcessMouseMove({ 120.0f, 50.0f });
				Input::ProcessScroll({ 0.0f, 2.0f });
			}
			else if (frame == pressFrame + 2)
			{
				Input::ProcessKey(Key::Space, false);
				Input::ProcessMouseButton(Mouse::ButtonLeft, false);
			}
		}

		// Leaves the global input state clean for other tests, also when a check fails.
		struct ScopedInput
		{
			ScopedInput()
			{
				Input::Reset();
				Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });
			}

			~ScopedInput()
			{
				Input::Reset();
			}

			ScopedInput(const ScopedInput&) = delete;
			ScopedInput& operator=(const ScopedInput&) = delete;
		};

		// Every live instance of a feature script (a class with the fields Checks, Failure and Completed) completed without
		// a failed check. Returns the number of instances checked.
		int32_t CheckFeatureScripts(Scene& scene, const ScriptSystem& system, const ScriptEngine& engine)
		{
			int32_t checked = 0;
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			{
				const ScriptComponent* scripts = entity.TryGetComponent<ScriptComponent>();
				if (!scripts)
					continue;

				for (const ScriptEntry& entry : scripts->Scripts)
				{
					const ScriptClassInfo* info = engine.FindClass(entry.ClassName);
					INFO("Script ", entry.ClassName, " on entity '", entity.GetName(), "'");
					REQUIRE_MESSAGE(info, "the feature script module has no such class");
					if (!info->FindField("Checks") || !info->FindField("Failure") || !info->FindField("Completed"))
						continue;

					const std::optional<PropertyValue> failure = system.GetFieldValue(entity, entry.ClassName, "Failure");
					const std::optional<PropertyValue> completed = system.GetFieldValue(entity, entry.ClassName, "Completed");
					const std::optional<PropertyValue> checks = system.GetFieldValue(entity, entry.ClassName, "Checks");
					REQUIRE_MESSAGE((failure && completed && checks), "the script has no live instance");
					const std::string& failedCheck = std::get<std::string>(*failure);
					CHECK_MESSAGE(failedCheck.empty(), "failed check: ", failedCheck);
					CHECK_MESSAGE(std::get<bool>(*completed), "the script did not complete its scenario (", std::get<int32_t>(*checks), " checks ran)");
					CHECK(std::get<int32_t>(*checks) > 0);
					checked++;
				}
			}
			return checked;
		}

		// Every ScriptCallback value (derived from their names, so new callbacks are included).
		std::vector<ScriptCallback> GetScriptCallbacks()
		{
			std::vector<ScriptCallback> callbacks;
			for (uint32_t value = 0; value < 256; value++)
			{
				const ScriptCallback callback = static_cast<ScriptCallback>(value);
				if (std::string_view(ScriptCallbackToString(callback)) != "Unknown")
					callbacks.push_back(callback);
			}
			return callbacks;
		}

		std::vector<std::string> GetJournal(Scene& scene)
		{
			const Entity journal = RequireEntity(scene, "Journal");
			REQUIRE(journal.HasComponent<TextComponent>());
			const std::string& text = journal.GetComponent<TextComponent>().Text;

			std::vector<std::string> entries;
			size_t start = 0;
			while (start < text.size())
			{
				size_t end = text.find(';', start);
				if (end == std::string::npos)
					end = text.size();
				if (end > start)
					entries.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			return entries;
		}

	}

	std::filesystem::path GetFeatureProjectSourceDirectory()
	{
		return FileSystem::FromUTF8(STRATA_FEATURE_TEST_DIR);
	}

	std::filesystem::path GetFeatureScriptModule()
	{
		return GetTestScriptModule(STRATA_TEST_SCRIPTS_FEATURETEST);
	}

	std::filesystem::path CopyFeatureProject(const std::filesystem::path& directory)
	{
		const std::filesystem::path source = GetFeatureProjectSourceDirectory();
		const std::filesystem::path projectFile = directory / "FeatureTest.stproj";
		REQUIRE(FileSystem::Copy(source / "FeatureTest.stproj", projectFile));
		REQUIRE(FileSystem::CopyDirectory(source / "Assets", directory / "Assets"));
		return projectFile;
	}

	void LoadAllAssets(AssetManagerBase& manager)
	{
		for (const AssetMetadata& metadata : manager.GetAllMetadata())
		{
			if (metadata.IsBuiltin())
				continue;
			INFO("Asset ", metadata.Path, " ", metadata.SubAssetKey);
			const bool loaded = manager.LoadAssetSync(metadata.Handle) != nullptr;
			CHECK_MESSAGE(loaded, manager.GetAssetError(metadata.Handle));
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// FeatureProject
	////////////////////////////////////////////////////////////////////////////////

	FeatureProject::FeatureProject()
		: m_Directory(CreateTemporaryDirectory("FeatureProject")), m_PreviousAssetManager(AssetManager::GetActive()), m_PreviousProject(Project::GetActive())
	{
		std::string error;
		m_Project = Project::Load(CopyFeatureProject(m_Directory), &error);
		REQUIRE_MESSAGE(m_Project, error);

		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = m_Project->GetAssetDirectory();
		specification.CacheDirectory = m_Project->GetCacheDirectory();
		specification.WatchFiles = false;
		m_AssetManager = CreateRef<EditorAssetManager>(specification);
		m_AssetManager->Scan();

		AssetManager::SetActive(m_AssetManager);
		Project::SetActive(m_Project);
	}

	FeatureProject::~FeatureProject()
	{
		AssetManager::SetActive(m_PreviousAssetManager);
		Project::SetActive(m_PreviousProject);
	}

	Ref<Scene> FeatureProject::LoadStartScene() const
	{
		const AssetHandle handle = m_Project->GetConfig().StartScene;
		REQUIRE(m_AssetManager->GetAssetType(handle) == AssetType::Scene);
		const Ref<SceneAsset> asset = AssetManager::LoadAssetSync<SceneAsset>(handle);
		REQUIRE_MESSAGE(asset, m_AssetManager->GetAssetError(handle));

		// Deserialized here rather than through SceneAsset::CreateScene, which does not report warnings.
		Ref<Scene> scene = CreateRef<Scene>();
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(*scene, asset->GetDocument(), &error, &warnings), error);
		for (const std::string& warning : warnings)
			FAIL_CHECK("Loading the feature scene warned: ", warning);
		return scene;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Logs
	////////////////////////////////////////////////////////////////////////////////

	LogCapture::LogCapture()
		: m_Start(Log::GetBuffer().GetLatestSequence())
	{
	}

	std::vector<LogEntry> LogCapture::GetEntries() const
	{
		std::vector<LogEntry> entries = Log::GetBuffer().GetEntries(m_Start);
		REQUIRE_MESSAGE((entries.empty() || entries.front().Sequence == m_Start + 1), "The log buffer dropped entries; raise its capacity");
		return entries;
	}

	ScopedScriptLogLevel::ScopedScriptLogLevel()
		: m_Previous(Log::GetScriptLogger()->level())
	{
		Log::GetScriptLogger()->set_level(spdlog::level::trace);
	}

	ScopedScriptLogLevel::~ScopedScriptLogLevel()
	{
		Log::GetScriptLogger()->set_level(m_Previous);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Feature runs
	////////////////////////////////////////////////////////////////////////////////

	void PlayFeatureScene(Scene& scene, ScriptEngine& engine, const std::function<void()>& advanceFrame)
	{
		ScopedInput input;
		REQUIRE(scene.IsRunning());
		const int32_t pressFrame = GetField<int32_t>(GetScriptSystem(scene), RequireEntity(scene, "Input Features"), "InputFeatures", "PressFrame");
		REQUIRE(pressFrame > 1);
		REQUIRE(pressFrame + 2 < c_FeatureReloadFrame);

		for (int32_t frame = 0; frame < c_FeatureFrames; frame++)
		{
			INFO("Frame ", frame);
			Input::BeginFrame();
			SimulateInput(frame, pressFrame);
			advanceFrame();
			REQUIRE_FALSE(engine.IsFaulted());
			if (frame == c_FeatureReloadFrame)
			{
				std::string error;
				REQUIRE_MESSAGE(engine.Reload(&error), error);
			}
		}
	}

	void CheckFeatureResults(Scene& scene, const ScriptEngine& engine)
	{
		const ScriptSystem& system = GetScriptSystem(scene);
		// Every FeatureScript instance alive at the end: the authored ones, the crates and the physics probes.
		CHECK(CheckFeatureScripts(scene, system, engine) >= 20);

		// One update per frame, across the hot reload (fields survive it).
		CHECK(GetField<int32_t>(system, RequireEntity(scene, "Field Features"), "FieldFeatures", "Count") == -7 + c_FeatureFrames);
		CHECK(GetField<int32_t>(system, RequireEntity(scene, "Lifecycle Features"), "LifecycleFeatures", "Reloads") == 1);

		// The bodies rest where the physics probes saw them.
		int32_t bodies = 0;
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			if (!system.HasInstance(entity, "PhysicsFeatures"))
				continue;
			INFO("Body ", entity.GetName());
			const float restHeight = GetField<float>(system, entity, "PhysicsFeatures", "RestHeight");
			const float tolerance = GetField<float>(system, entity, "PhysicsFeatures", "Tolerance");
			const float height = scene.GetWorldTransform(entity)[3].y;
			CHECK(std::abs(height - restHeight) <= tolerance);
			bodies++;
		}
		CHECK(bodies == 3);

		// The fragile probe destroyed itself when it landed (inside its OnCollisionEnter).
		CHECK_FALSE(scene.FindEntityByName("Fragile Probe").IsValid());
	}

	int32_t PlayFeatureQuitFrame(Scene& scene, ScriptEngine& engine, const std::function<void()>& advanceFrame)
	{
		ScriptSystem& system = GetScriptSystem(scene);
		const Entity gameFeatures = RequireEntity(scene, "Game Features");
		REQUIRE(GetField<int32_t>(system, gameFeatures, "GameFeatures", "QuitFrame") == -1); // Nothing quit during the scenario
		const int32_t quitCode = GetField<int32_t>(system, gameFeatures, "GameFeatures", "QuitCode");
		REQUIRE(scene.GetFrameIndex() == static_cast<uint64_t>(c_FeatureFrames));
		REQUIRE(system.SetFieldValue(gameFeatures, "GameFeatures", "QuitFrame", PropertyValue(static_cast<int32_t>(c_FeatureFrames))));
		REQUIRE_FALSE(scene.GetQuitRequest().has_value());

		ScopedInput input;
		advanceFrame();
		REQUIRE_FALSE(engine.IsFaulted());
		return quitCode;
	}
	void CheckFeatureJournal(Scene& scene, const ScriptEngine& engine)
	{
		const std::vector<std::string> journal = GetJournal(scene);
		REQUIRE_FALSE(engine.GetClasses().empty());
		for (const ScriptClassInfo& info : engine.GetClasses())
		{
			const std::string prefix = info.Name + ".OnCreate@";
			bool created = false;
			for (const std::string& entry : journal)
				created |= entry.rfind(prefix, 0) == 0;
			CHECK_MESSAGE(created, "Script class ", info.Name, " never ran in the feature scene: attach it to an entity of "
				"StrataTests/FeatureTest/Assets/Scenes/Feature.stscene (or spawn it) and journal its OnCreate");
		}

		// Every callback the engine offers ran (the scripts journal their callbacks, see FeatureScript.h).
		for (const ScriptCallback callback : GetScriptCallbacks())
		{
			const std::string name = ScriptCallbackToString(callback);
			const std::string event = "." + name + "@";
			const bool ran = std::any_of(journal.begin(), journal.end(), [&](const std::string& entry) { return entry.find(event) != std::string::npos; });
			CHECK_MESSAGE(ran, "Script callback ", name, " never ran: implement it in a feature script that runs and journal its call (Journal(*this, "
				"\"<Class>\", \"", name, "\"))");
		}

		for (const char* expected : { "DoomedProbe.OnDestroy@Doomed", "Helper.OnDestroy@Script Features", "LifecycleFeatures.OnReload@Lifecycle Features",
				 "ExceptionProbe.OnReload@Exception Probe", "LifecycleFeatures.OnDestroy@Lifecycle Features", "FragileProbe.OnCollisionEnter@Fragile Probe",
				 "FragileProbe.OnDestroy@Fragile Probe", "GameFeatures.Quit@Game Features", "SpawnFeatures.ReleaseAssets@Spawn Features" })
		{
			INFO("Journal entry ", expected);
			CHECK(std::find(journal.begin(), journal.end(), expected) != journal.end());
		}
	}

	void CheckFeatureLog(const std::vector<LogEntry>& entries, int32_t runs, std::span<const std::string_view> tolerated)
	{
		std::vector<int32_t> counts(std::size(c_ExpectedLogMessages), 0);
		for (const LogEntry& entry : entries)
		{
			bool known = false;
			for (size_t index = 0; index < std::size(c_ExpectedLogMessages); index++)
			{
				if (Matches(entry, c_ExpectedLogMessages[index]))
				{
					counts[index]++;
					known = true;
				}
			}
			for (const std::string_view text : tolerated)
				known |= entry.Message.find(text) != std::string::npos;
			if (!known && entry.Level >= LogLevel::Warn)
				FAIL_CHECK("Unexpected ", std::string(LogLevelToString(entry.Level)), " from ", entry.Logger, ": ", entry.Message);
		}
		for (size_t index = 0; index < std::size(c_ExpectedLogMessages); index++)
		{
			INFO("Expected log message: ", std::string(c_ExpectedLogMessages[index].Text));
			CHECK(counts[index] == runs);
		}
	}

}
