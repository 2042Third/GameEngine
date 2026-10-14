#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Application.h"
#include "Strata/Core/EngineModule.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/Log.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Project/Project.h"
#include "Strata/Project/ProjectModule.h"
#include "TestHelpers.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	using Journal = std::vector<std::string>;

	// What a journaling module does besides writing every hook it gets into the journal.
	struct ModuleBehavior
	{
		std::vector<EngineModuleDependency> Dependencies;
		bool FailInit = false;
		std::string StartFailureProblem;
		// OnBeginFrame returns Skip at these calls of it (1 is the first) and Close at CloseAt.
		std::vector<uint32_t> SkipAt;
		std::optional<uint32_t> CloseAt;
		// Waits this long in OnBeginFrame and reports the wait (EngineFrame::WaitSeconds).
		std::chrono::milliseconds Wait { 0 };
		bool HandleEvents = false;
		// Pushes a layer in OnInit, as ImGui pushes its overlay, which journals "Detach <name> layer".
		bool PushLayer = false;
	};

	// The layer a module pushes (ModuleBehavior::PushLayer).
	class ModuleLayer : public Layer
	{
	public:
		ModuleLayer(std::string_view moduleName, Journal& journal)
			: Layer("Module"), m_ModuleName(moduleName), m_Journal(journal)
		{
		}

		void OnDetach() override { m_Journal.push_back(fmt::format("Detach {} layer", m_ModuleName)); }
	private:
		std::string m_ModuleName;
		Journal& m_Journal;
	};

	// Writes "<hook> <name>" into the journal for every hook: "Init A", "Begin A", "Update A", "UI A", "End A", "Event A",
	// "Stop A", "Release A", "Shutdown A"; Release and Shutdown also say whether the job system still runs ("Release A
	// jobs", "Shutdown A no jobs").
	class JournalModule : public EngineModule
	{
	public:
		JournalModule(std::string name, Journal& journal, ModuleBehavior behavior = {})
			: m_Name(std::move(name)), m_Journal(journal), m_Behavior(std::move(behavior))
		{
		}

		std::string_view GetName() const override { return m_Name; }
		std::span<const EngineModuleDependency> GetDependencies() const override { return m_Behavior.Dependencies; }

		bool OnInit(EngineContext& context) override
		{
			Add("Init");
			m_InitWithModules = &context.Modules == &context.App.GetModules();
			if (m_Behavior.PushLayer)
				context.App.PushLayer(new ModuleLayer(m_Name, m_Journal));
			return !m_Behavior.FailInit;
		}

		EngineModuleStartFailure GetStartFailure() const override { return { m_Behavior.StartFailureProblem, {} }; }

		EngineFrameAction OnBeginFrame(EngineFrame& frame) override
		{
			Add("Begin");
			m_BeginCalls++;
			if (m_Behavior.Wait.count() > 0)
			{
				const auto start = std::chrono::steady_clock::now();
				std::this_thread::sleep_for(m_Behavior.Wait);
				frame.WaitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			}
			if (m_Behavior.CloseAt && m_BeginCalls == *m_Behavior.CloseAt)
				return EngineFrameAction::Close;
			for (const uint32_t call : m_Behavior.SkipAt)
			{
				if (call == m_BeginCalls)
					return EngineFrameAction::Skip;
			}
			return EngineFrameAction::Continue;
		}

		void OnUpdate(EngineFrame&) override { Add("Update"); }
		void OnRenderUI(EngineFrame&) override { Add("UI"); }
		void OnEndFrame(EngineFrame&) override { Add("End"); }

		void OnEvent(Event& event) override
		{
			Add("Event");
			if (m_Behavior.HandleEvents)
				event.Handled = true;
		}

		void OnStop() override { Add("Stop"); }
		void OnRelease() override { m_Journal.push_back(fmt::format("Release {} {}", m_Name, JobSystem::IsInitialized() ? "jobs" : "no jobs")); }
		void OnShutdown() override { m_Journal.push_back(fmt::format("Shutdown {} {}", m_Name, JobSystem::IsInitialized() ? "jobs" : "no jobs")); }

		bool InitWithModules() const { return m_InitWithModules; }
	private:
		void Add(std::string_view hook) { m_Journal.push_back(fmt::format("{} {}", hook, m_Name)); }
	private:
		std::string m_Name;
		Journal& m_Journal;
		ModuleBehavior m_Behavior;
		uint32_t m_BeginCalls = 0;
		bool m_InitWithModules = false;
	};

	// Module types that EngineModuleRegistry::Find<T> finds by their c_Name.
	class AlphaModule : public JournalModule
	{
	public:
		static constexpr std::string_view c_Name = "Alpha";
		explicit AlphaModule(Journal& journal)
			: JournalModule(std::string(c_Name), journal)
		{
		}
	};

	class BetaModule : public JournalModule
	{
	public:
		static constexpr std::string_view c_Name = "Beta";
		explicit BetaModule(Journal& journal)
			: JournalModule(std::string(c_Name), journal)
		{
		}
	};

	class GammaModule : public JournalModule
	{
	public:
		static constexpr std::string_view c_Name = "Gamma";
		explicit GammaModule(Journal& journal)
			: JournalModule(std::string(c_Name), journal)
		{
		}
	};

	// What a module released after the Project module (listed before it) finds when it is released.
	struct ReleaseObservation
	{
		bool Released = false;
		bool AssetManagerActive = true;
		bool ProjectActive = true;
		bool AssetManagerDestroyed = false;
		bool JobsRunning = false;
	};

	class ReleaseProbeModule : public EngineModule
	{
	public:
		// `manager` is read when the module is released: the test sets it once the application started.
		ReleaseProbeModule(ReleaseObservation& observation, const WeakRef<AssetManagerBase>& manager)
			: m_Observation(observation), m_Manager(manager)
		{
		}

		std::string_view GetName() const override { return "ReleaseProbe"; }
		bool OnInit(EngineContext&) override { return true; }

		void OnRelease() override
		{
			m_Observation.Released = true;
			m_Observation.AssetManagerActive = AssetManager::GetActive() != nullptr;
			m_Observation.ProjectActive = Project::GetActive() != nullptr;
			m_Observation.AssetManagerDestroyed = m_Manager.expired();
			m_Observation.JobsRunning = JobSystem::IsInitialized();
		}
	private:
		ReleaseObservation& m_Observation;
		const WeakRef<AssetManagerBase>& m_Manager;
	};

	EngineModuleFactory MakeModule(std::string name, Journal& journal, ModuleBehavior behavior = {})
	{
		return [name = std::move(name), &journal, behavior = std::move(behavior)]() -> Scope<EngineModule>
		{
			return CreateScope<JournalModule>(name, journal, behavior);
		};
	}

	// Journals its update and detach ("Layer update", "Layer detach") and works for its frame's Work (busy, so the time is
	// exact).
	class JournalLayer : public Layer
	{
	public:
		JournalLayer(Journal& journal, std::chrono::milliseconds work = std::chrono::milliseconds(0), bool handleEvents = false)
			: Layer("Journal"), m_Journal(journal), m_Work(work), m_HandleEvents(handleEvents)
		{
		}

		void OnDetach() override { m_Journal.push_back("Layer detach"); }

		void OnUpdate(Timestep) override
		{
			m_Journal.push_back("Layer update");
			const auto end = std::chrono::steady_clock::now() + m_Work;
			while (std::chrono::steady_clock::now() < end)
			{
			}
		}

		void OnEvent(Event& event) override
		{
			m_Journal.push_back("Layer event");
			if (m_HandleEvents)
				event.Handled = true;
		}
	private:
		Journal& m_Journal;
		std::chrono::milliseconds m_Work;
		bool m_HandleEvents;
	};

	class JournalApplication : public Application
	{
	public:
		JournalApplication(const ApplicationSpecification& specification, Journal& journal, std::chrono::milliseconds layerWork = std::chrono::milliseconds(0))
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new JournalLayer(journal, layerWork));
		}
	};

	ApplicationSpecification MakeSpecification(std::vector<EngineModuleFactory> modules, std::optional<uint64_t> maxFrames = 1)
	{
		ApplicationSpecification specification;
		specification.Name = "EngineModuleTest";
		specification.Headless = true;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		specification.Jobs.BackgroundThreadCount = 1;
		specification.MaxFrames = maxFrames;
		specification.Modules = std::move(modules);
		return specification;
	}

	// The entries of the journal that start with `prefix`.
	Journal Entries(const Journal& journal, std::string_view prefix)
	{
		Journal entries;
		for (const std::string& entry : journal)
		{
			if (std::string_view(entry).substr(0, prefix.size()) == prefix)
				entries.push_back(entry);
		}
		return entries;
	}

	std::vector<std::string> Names(const EngineModuleRegistry& registry)
	{
		std::vector<std::string> names;
		for (const EngineModule* module : registry.GetAll())
			names.emplace_back(module->GetName());
		return names;
	}

}

TEST_SUITE("Core.EngineModules")
{
	TEST_CASE("Modules start in dependency order, listing order where free, and stop, release and shut down in reverse")
	{
		Journal journal;
		{
			// C needs A; D orders itself after a module the program does not run, which is ignored.
			JournalApplication application(MakeSpecification({
				MakeModule("C", journal, { .Dependencies = { { "A" } } }),
				MakeModule("A", journal),
				MakeModule("B", journal, { .Dependencies = { { "C", false } } }),
				MakeModule("D", journal, { .Dependencies = { { "Missing", false } } }) }), journal);
			REQUIRE(application.IsRunning());
			CHECK(Names(application.GetModules()) == std::vector<std::string> { "A", "C", "B", "D" });
			CHECK(Entries(journal, "Init") == Journal { "Init A", "Init C", "Init B", "Init D" });
			const auto* module = static_cast<const JournalModule*>(application.GetModules().Find("C"));
			REQUIRE(module);
			CHECK(module->InitWithModules());
			application.Run();
			CHECK(application.GetExitCode() == 0);
			CHECK(Entries(journal, "Stop") == Journal { "Stop D", "Stop B", "Stop C", "Stop A" });
			CHECK(Entries(journal, "Release").empty());
		}
		// Released after the layers, while the job system runs; shut down once it stopped.
		const Journal ending(std::find(journal.begin(), journal.end(), "Layer detach"), journal.end());
		CHECK(ending == Journal { "Layer detach", "Release D jobs", "Release B jobs", "Release C jobs", "Release A jobs",
			"Shutdown D no jobs", "Shutdown B no jobs", "Shutdown C no jobs", "Shutdown A no jobs" });
	}

	TEST_CASE("Each frame begins, updates and draws the UI of the modules in order and ends them in reverse")
	{
		Journal journal;
		{
			JournalApplication application(MakeSpecification({ MakeModule("B", journal, { .Dependencies = { { "A" } } }), MakeModule("A", journal) }, 2), journal);
			application.Run();
			CHECK(application.GetFrameCount() == 2);
		}
		const Journal frame { "Begin A", "Begin B", "Layer update", "Update A", "Update B", "UI A", "UI B", "End B", "End A" };
		Journal frames;
		for (const std::string& entry : journal)
		{
			if (entry.starts_with("Begin") || entry.starts_with("Update") || entry.starts_with("UI") || entry.starts_with("End") || entry == "Layer update")
				frames.push_back(entry);
		}
		Journal expected = frame;
		expected.insert(expected.end(), frame.begin(), frame.end());
		CHECK(frames == expected);
	}

	TEST_CASE("A module that skips a frame leaves it uncounted, and one that closes ends the loop")
	{
		Journal journal;
		{
			JournalApplication application(MakeSpecification({
				MakeModule("A", journal, { .SkipAt = { 2 }, .CloseAt = 4 }),
				MakeModule("B", journal) }, 10), journal);
			application.Run();
			// Frames 1 and 3 ran; 2 was skipped and 4 closed the program.
			CHECK(application.GetFrameCount() == 2);
			CHECK_FALSE(application.IsRunning());
			CHECK(application.GetExitCode() == 0);
		}
		CHECK(Entries(journal, "Begin A").size() == 4);
		// The modules after the one that ended the frame begin nothing, and no frame that did not run ends.
		CHECK(Entries(journal, "Begin B").size() == 2);
		CHECK(Entries(journal, "Layer update").size() == 2);
		CHECK(Entries(journal, "Update A").size() == 2);
		CHECK(Entries(journal, "End A").size() == 2);
		CHECK(Entries(journal, "Stop") == Journal { "Stop B", "Stop A" });
	}

	TEST_CASE("A module that cannot start ends the program, and only the modules that started end")
	{
		Journal journal;
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		const Journal ended { "Init A", "Init B", "Detach A layer", "Release A jobs", "Shutdown A no jobs" };
		{
			// A pushes a layer, as ImGui pushes its overlay: it goes before A ends, as layers go before modules at exit.
			JournalApplication application(MakeSpecification({
				MakeModule("A", journal, { .PushLayer = true }),
				MakeModule("B", journal, { .FailInit = true, .StartFailureProblem = "B has no device to work with." }),
				MakeModule("C", journal) }), journal);
			CHECK_FALSE(application.IsRunning());
			CHECK(application.GetExitCode() == 1);
			// The modules that started ended at once, before the program reported why it cannot run.
			CHECK(journal == ended);
			application.Run();
			CHECK(application.GetFrameCount() == 0);
			CHECK(application.GetExitCode() == 1);
		}
		CHECK(journal == ended);
		CHECK(Tests::CountLogMessages(logStart, "Engine module 'B' could not start; the application cannot run") == 1);
		CHECK(Tests::CountLogMessages(logStart, "B has no device to work with.") == 1);
	}

	TEST_CASE("The Project module lets go of the active project and asset manager after the layers, while jobs run")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectModuleRelease");
		// Whatever the module does, the next test starts without an active project or asset manager.
		struct ClearActive
		{
			~ClearActive()
			{
				AssetManager::SetActive(nullptr);
				Project::SetActive(nullptr);
			}
		} clearActive;
		Journal journal;
		ReleaseObservation observation;
		WeakRef<AssetManagerBase> weakManager;
		{
			// The probe is listed first, so it is released after the Project module (OnRelease runs in reverse).
			std::vector<EngineModuleFactory> modules;
			modules.push_back([&observation, &weakManager]() -> Scope<EngineModule>
			{
				return CreateScope<ReleaseProbeModule>(observation, weakManager);
			});
			modules.push_back(ProjectModule::Factory());
			JournalApplication application(MakeSpecification(std::move(modules)), journal);
			REQUIRE(application.IsRunning());

			// What the program's layers open: the active project and asset manager, held only as the active ones.
			Ref<Project> project = Project::Create(directory, "ProjectModuleRelease");
			REQUIRE(project);
			Project::SetActive(project);
			project.reset();
			Ref<AssetManagerBase> manager = CreateRef<Tests::FakeAssetManager>();
			weakManager = manager;
			AssetManager::SetActive(manager);
			manager.reset();

			application.Run();
			CHECK_FALSE(observation.Released);
			CHECK(AssetManager::GetActive() != nullptr);
		}
		CHECK(observation.Released);
		CHECK_FALSE(observation.AssetManagerActive);
		CHECK_FALSE(observation.ProjectActive);
		// Let go while the job system still runs (the manager waits for its load jobs when it is destroyed).
		CHECK(observation.AssetManagerDestroyed);
		CHECK(observation.JobsRunning);
	}

	TEST_CASE("A program whose modules cannot be ordered does not start, and the message names them")
	{
		Journal journal;
		SUBCASE("A missing required dependency")
		{
			EngineModuleRegistry registry;
			std::string error;
			CHECK_FALSE(registry.Create(std::vector<EngineModuleFactory> { MakeModule("ImGui", journal, { .Dependencies = { { "Graphics" } } }) }, &error));
			CHECK(error == "engine module 'ImGui' requires 'Graphics', which the program does not run");
			CHECK(registry.GetCount() == 0);
		}
		SUBCASE("A cycle")
		{
			EngineModuleRegistry registry;
			std::string error;
			// D only waits for the cycle and is not part of it.
			CHECK_FALSE(registry.Create(std::vector<EngineModuleFactory> {
				MakeModule("A", journal, { .Dependencies = { { "C" } } }),
				MakeModule("B", journal, { .Dependencies = { { "A" } } }),
				MakeModule("C", journal, { .Dependencies = { { "B", false } } }),
				MakeModule("D", journal, { .Dependencies = { { "C" } } }),
				MakeModule("E", journal) }, &error));
			CHECK(error == "engine modules 'A', 'B' and 'C' depend on each other in a cycle");
			CHECK(registry.GetCount() == 0);
		}
		SUBCASE("A module that depends on itself")
		{
			EngineModuleRegistry registry;
			std::string error;
			CHECK_FALSE(registry.Create(std::vector<EngineModuleFactory> { MakeModule("A", journal, { .Dependencies = { { "A" } } }) }, &error));
			CHECK(error == "engine modules 'A' depend on each other in a cycle");
		}
		SUBCASE("Two modules with one name, and a factory that makes none")
		{
			EngineModuleRegistry registry;
			std::string error;
			CHECK_FALSE(registry.Create(std::vector<EngineModuleFactory> {
				MakeModule("Audio", journal), EngineModuleFactory(), MakeModule("Audio", journal), MakeModule("Audio", journal) }, &error));
			CHECK(error == "the engine module factory at position 2 of the program's list makes no module; two engine modules are named 'Audio'");
		}
		SUBCASE("The application refuses to start")
		{
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			{
				JournalApplication application(MakeSpecification({ MakeModule("ImGui", journal, { .Dependencies = { { "Graphics" } } }) }), journal);
				CHECK_FALSE(application.IsRunning());
				CHECK(application.GetExitCode() == 1);
				CHECK(application.GetModules().GetCount() == 0);
			}
			CHECK(journal.empty()); // Not a module started, and no layer was pushed
			CHECK(Tests::CountLogMessages(logStart, "EngineModuleTest cannot start: engine module 'ImGui' requires 'Graphics', which the program does not run.") == 1);
		}
		CHECK(Entries(journal, "Init").empty());
	}

	TEST_CASE("Events reach the modules in order before the layers, and a handled event goes no further")
	{
		Journal journal;
		SUBCASE("Not handled")
		{
			JournalApplication application(MakeSpecification({ MakeModule("B", journal, { .Dependencies = { { "A" } } }), MakeModule("A", journal) }), journal);
			journal.clear();
			KeyPressedEvent event(Key::A);
			application.OnEvent(event);
			CHECK(journal == Journal { "Event A", "Event B", "Layer event" });
		}
		SUBCASE("Handled by a module")
		{
			JournalApplication application(MakeSpecification({ MakeModule("A", journal, { .HandleEvents = true }), MakeModule("B", journal) }), journal);
			journal.clear();
			KeyPressedEvent event(Key::A);
			application.OnEvent(event);
			CHECK(event.Handled);
			CHECK(journal == Journal { "Event A" });
		}
	}

	TEST_CASE("What a module waits for is left out of the frame's work time")
	{
		Journal journal;
		constexpr std::chrono::milliseconds layerWork(5);
		constexpr std::chrono::milliseconds wait(60);
		JournalApplication application(MakeSpecification({ MakeModule("A", journal, { .Wait = wait }) }), journal, layerWork);
		application.Run();
		REQUIRE(application.GetFrameCount() == 1);
		const double workTime = application.GetLastFrameWorkTime();
		CHECK(workTime >= std::chrono::duration<double>(layerWork).count());
		// The 60 ms wait would make it at least that long.
		CHECK(workTime < std::chrono::duration<double>(wait).count() * 0.75);
	}

	TEST_CASE("Find<T> finds a module by its type's name")
	{
		Journal journal;
		EngineModuleRegistry registry;
		REQUIRE(registry.Create(std::vector<EngineModuleFactory> {
			[&]() -> Scope<EngineModule> { return CreateScope<BetaModule>(journal); },
			[&]() -> Scope<EngineModule> { return CreateScope<AlphaModule>(journal); } }));
		REQUIRE(registry.GetCount() == 2);
		CHECK(registry.Find<AlphaModule>() == registry.GetAll()[1]);
		CHECK(registry.Find<BetaModule>() == registry.GetAll()[0]);
		CHECK(registry.Find<GammaModule>() == nullptr);
		CHECK(registry.Find("Beta") == registry.GetAll()[0]);
		CHECK(registry.Find("beta") == nullptr);
		registry.Clear();
		CHECK(registry.GetCount() == 0);
		CHECK(registry.Find<AlphaModule>() == nullptr);
	}
}
