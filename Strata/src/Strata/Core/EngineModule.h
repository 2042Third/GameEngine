#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Application;
	class Event;
	class EngineModuleRegistry;
	class LayerStack;
	class Window;

	// A module the module named here must start after. Required false: an ordering constraint only, ignored when the
	// program does not run that module.
	struct EngineModuleDependency
	{
		std::string_view Name;
		bool Required = true;
	};

	// What a module's OnInit receives: the application (its specification, PushOverlay, SubmitToMainThread), its window
	// (null when it runs headless) and the program's modules (to find the ones it depends on, which started before it).
	struct EngineContext
	{
		Application& App;
		Window* MainWindow;
		EngineModuleRegistry& Modules;
	};

	// What a module's OnBeginFrame decides about the frame.
	enum class EngineFrameAction : uint8_t
	{
		Continue,
		Skip, // Nothing can be rendered now (e.g. a swapchain mid-resize): the frame ends uncounted, the loop waits 16 ms
		Close // The program cannot go on (a lost device): the loop ends after this frame
	};

	// The frame the hooks run in. WaitSeconds is time modules spent waiting for something outside the program (the GPU, the
	// display) during the frame: Application::GetLastFrameWorkTime leaves it out.
	struct EngineFrame
	{
		Timestep Step;
		double WaitSeconds = 0.0;
		LayerStack& Layers;
	};

	// What a person is told when a module could not start: Application reports it (Application::ReportFatalError, with
	// the application's name as the title) once the started modules shut down and the window closed. An empty Problem:
	// only the log says which module failed.
	struct EngineModuleStartFailure
	{
		std::string Problem;
		std::string Advice;
	};

	// A service the application runs for the whole program (the graphics device, audio, Dear ImGui, the project) with its own
	// startup, frame and shutdown hooks. Programs list the modules they run (ApplicationSpecification::Modules); Application
	// never names one, and calls the hooks in the order of the table in Docs/Architecture.md ("Application and frame loop"):
	// startup, the frame's hooks other than OnEndFrame and events in dependency order, OnEndFrame, stopping, releasing and
	// shutting down in reverse. Modules that never started (OnInit failed or never ran) get no other hook. Main thread.
	//
	// A module type that EngineModuleRegistry::Find<T> finds declares `static constexpr std::string_view c_Name`, the name
	// GetName returns.
	class EngineModule
	{
	public:
		virtual ~EngineModule() = default;

		virtual std::string_view GetName() const = 0;
		// Modules this one starts after. A missing required dependency refuses the program's start.
		virtual std::span<const EngineModuleDependency> GetDependencies() const { return {}; }

		// In the Application constructor, after the job system, the window and Input. False: the program cannot run (the
		// module cleans up what it started itself, logs why and may describe it for a person: GetStartFailure).
		[[nodiscard]] virtual bool OnInit(EngineContext& context) = 0;
		virtual EngineModuleStartFailure GetStartFailure() const { return {}; }

		// After the frame's Input stage, before the layers update. A module that returns Skip or Close ends the frame there:
		// the modules after it get no OnBeginFrame, and no module gets the frame's other hooks.
		virtual EngineFrameAction OnBeginFrame(EngineFrame&) { return EngineFrameAction::Continue; }
		// After the layers' OnUpdate.
		virtual void OnUpdate(EngineFrame&) {}
		// After every module's OnUpdate: user interface over the frame (Dear ImGui calls the layers' OnImGuiRender here).
		// The frame's work time ends after this hook.
		virtual void OnRenderUI(EngineFrame&) {}
		// After the work time was measured, in reverse order, only for frames that ran (presenting, captures).
		virtual void OnEndFrame(EngineFrame&) {}
		// Every event, after the application's own bookkeeping and before the layers; one a module handles goes no further.
		virtual void OnEvent(Event&) {}

		// Once, when Application::Run's loop ended; the layers still exist.
		virtual void OnStop() {}
		// In the Application destructor, once the layers are gone; the job system still runs.
		virtual void OnRelease() {}
		// Once the job system has stopped: the last hook.
		virtual void OnShutdown() {}
	};

	using EngineModuleFactory = std::function<Scope<EngineModule>()>;

	// Owns a program's modules and orders them: a stable topological sort, so that modules start after their dependencies
	// and otherwise in the order the program listed them.
	class EngineModuleRegistry
	{
	public:
		EngineModuleRegistry() = default;
		~EngineModuleRegistry();

		EngineModuleRegistry(const EngineModuleRegistry&) = delete;
		EngineModuleRegistry& operator=(const EngineModuleRegistry&) = delete;

		// Makes the modules of the factories and orders them. Refuses (nothing is kept, outError names the modules) a factory
		// that makes no module, two modules with one name, a required dependency the list lacks and dependencies in a cycle.
		bool Create(std::span<const EngineModuleFactory> factories, std::string* outError = nullptr);
		// Destroys the modules, last first.
		void Clear();

		// The modules in dependency order.
		std::span<EngineModule* const> GetAll() const { return m_Modules; }
		size_t GetCount() const { return m_Modules.size(); }
		EngineModule* Find(std::string_view name) const;
		template<typename T>
		T* Find() const
		{
			return static_cast<T*>(Find(T::c_Name));
		}
	private:
		std::vector<Scope<EngineModule>> m_Owned; // In dependency order, like m_Modules
		std::vector<EngineModule*> m_Modules;
	};

}
