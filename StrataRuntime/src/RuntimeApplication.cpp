#include <Strata.h>
#include <Strata/Core/EntryPoint.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/SaveFiles.h>
#include <Strata/Renderer/GPUUploadQueue.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/StagingPool.h>
#include <Strata/Runtime/FramePerfLog.h>
#include <Strata/Runtime/GameRenderer.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Runtime/GameWindowService.h>

namespace Strata
{

	// Exit codes: 1 when the game cannot start, 2 when its scripts crashed in a headless run.
	constexpr int c_ScriptCrashExitCode = 2;
	// Frames per second of a headless run (no vsync paces it): game time passes as in a window, and a dedicated server
	// does not keep a CPU core busy.
	constexpr uint32_t c_HeadlessFrameRate = 60;
	// The largest --asset-budget-mb (16 TB): beyond any device, and far from overflowing when converted to bytes.
	constexpr int64_t c_MaxAssetBudgetMB = 16ll << 20;
	// What a person learns when the game cannot start for want of its files.
	constexpr const char* c_GameFilesAdvice = "StrataRuntime plays games exported from Strata Editor (File > Export Game...): run the "
		"executable the export wrote, in its folder with the game's .stgame manifest and .stpak pack, or pass --game <file>.";

	struct RuntimeOptions
	{
		std::filesystem::path ManifestPath;
		std::filesystem::path ScreenshotPath; // Saves the window to this PNG on the last of MaxFrames frames
		std::optional<uint64_t> MaxFrames;
		std::optional<uint64_t> TextureBudget; // --asset-budget-mb: the GPU texture budget of the game's assets, in bytes
		std::filesystem::path PerfLogPath;     // --perf-log: a CSV row per frame (FramePerfLog)
		std::filesystem::path SaveDirectory;   // --save-directory: where the game keeps its save files (empty: the player's data)
	};

	// Runs the game of a manifest and, with a window, renders it every frame. Startup failures end the process with exit
	// code 1; a game that quits ends it with the exit code it chose.
	class RuntimeLayer : public Layer
	{
	public:
		explicit RuntimeLayer(RuntimeOptions options)
			: Layer("RuntimeLayer"), m_Options(std::move(options))
		{
		}

		void OnAttach() override
		{
			// The other budgets keep their defaults, which depend on the graphics device (initialized by now).
			GameRuntimeOptions runtimeOptions;
			// A game runs on the wall clock; runs of a fixed number of frames (--frames: checks, screenshots) keep every step.
			runtimeOptions.TimeDilation = !m_Options.MaxFrames;
			runtimeOptions.SaveFilesDirectory = m_Options.SaveDirectory;
			// Headless runs (servers, CI) have no window to render to; with one, scenes have their render proxies built before
			// they start, so the frame that shows one does not build them all.
			const bool renders = Application::Get().GetWindow() && Renderer::IsInitialized();
			runtimeOptions.PrepareRenderScenes = renders;
			if (m_Options.TextureBudget)
			{
				runtimeOptions.AssetBudgets = AssetManagerBase::GetDefaultResidencyBudgets();
				runtimeOptions.AssetBudgets->GpuTextures = *m_Options.TextureBudget;
			}
			// The game's display options (scripts' Game::SetWindowMode, SetResolution, SetVSync) change this window, or nothing
			// in a headless run; from the game's first script call on.
			m_WindowService = CreateScope<GameWindowService>(Application::Get().GetWindow());
			m_WindowProvider = CreateScope<EngineServices::ScopedProvider<IGameWindow>>(*m_WindowService);
			std::string error;
			m_Runtime = GameRuntime::Create(m_Options.ManifestPath, &error, runtimeOptions);
			if (!m_Runtime)
			{
				ReportStartFailure(error);
				return;
			}
			if (renders)
				m_Renderer = CreateScope<GameRenderer>();
			if (!m_Options.PerfLogPath.empty())
				StartPerfLog();
		}

		// A game that cannot start ends the process with exit code 1, and a person who started it learns why (a dialog where
		// nothing else would show it: Application::ReportFatalError).
		void ReportStartFailure(const std::string& error)
		{
			Application& application = Application::Get();
			Application::ReportFatalError(application.GetSpecification().Name, fmt::format("Cannot start the game '{}': {}",
				FileSystem::ToUTF8(m_Options.ManifestPath), error), "Its files may be incomplete or damaged: export the game again, or "
				"download or unpack it again.");
			application.SetExitCode(1);
			application.Close();
		}

		void OnDetach() override
		{
			StopPerfLog();
			m_Renderer.reset();
			m_Runtime.reset();
			m_WindowProvider.reset();
			m_WindowService.reset();
		}

		void OnUpdate(Timestep timestep) override
		{
			if (!m_Runtime)
				return;
			m_Runtime->Update(timestep);
			// A start scene that cannot be loaded leaves nothing to play.
			if (const std::optional<std::string> startError = m_Runtime->GetStartError())
			{
				ReportStartFailure(*startError);
				m_Runtime.reset();
				return;
			}
			// A headless run (a server, CI) has nobody to show a broken game to: a script crash ends it with exit code 2. A
			// windowed game keeps running without its scripts; the crash is in the log.
			if (m_Runtime->GetScriptFault() && Application::Get().GetSpecification().Headless)
			{
				ST_CRITICAL("Stopping the headless game after a script crash");
				Application::Get().SetExitCode(c_ScriptCrashExitCode);
				Application::Get().Close();
				m_Runtime.reset();
				return;
			}
			// The game asked to end (Scene::RequestQuit): the process exits with its code.
			if (const std::optional<int32_t> exitCode = m_Runtime->GetQuitRequest())
			{
				Application::Get().SetExitCode(*exitCode);
				Application::Get().Close();
				return;
			}
			// Window changes the game asked for during its update; the swapchain follows at the next frame.
			m_WindowService->ApplyRequests();
			if (m_Renderer)
			{
				// Frames of minimized windows are skipped by the application; this frame has a back buffer, which the GPU waits
				// for only from here on (after the frame's asset uploads, which the update above submitted).
				GraphicsDevice& device = Renderer::GetGraphicsDevice();
				Renderer::PrepareBackBuffer();
				// Until the first scene (or the loading scene) plays, the frame is cleared.
				if (nvrhi::IFramebuffer* backBuffer = device.GetBackBufferFramebuffer())
				{
					if (m_Runtime->GetScene())
						m_Renderer->Render(m_Runtime->GetScene(), backBuffer, device.GetBackBufferSize());
					else
						m_Renderer->RenderEmpty(backBuffer, device.GetBackBufferSize());
				}
			}
			RequestScreenshot();
		}
	private:
		// Every frame from the first one on gets a row, written when the frame profiler records it (after the frame's work
		// time is measured, so writing it costs the frame nothing).
		void StartPerfLog()
		{
			std::string error;
			m_PerfLog = FramePerfLog::Create(m_Options.PerfLogPath, FrameProfiler::Get(), &error);
			if (!m_PerfLog)
			{
				ST_CRITICAL("Cannot write the perf log: {}", error);
				Application::Get().SetExitCode(1);
				Application::Get().Close();
				return;
			}
			m_PerfLogListener = FrameProfiler::Get().AddFrameListener([this](const FrameSample& frame)
			{
				const std::optional<AssetManagerStats> assets = m_Runtime ? std::optional<AssetManagerStats>(m_Runtime->GetAssetManager()->GetStats()) : std::nullopt;
				const GPUUploadQueueStats* uploads = Renderer::IsInitialized() ? &Renderer::GetUploadQueue().GetStats() : nullptr;
				std::optional<FramePerfDeviceMemory> device;
				if (Renderer::IsInitialized())
				{
					const StagingPool& staging = Renderer::GetStagingPool();
					device = FramePerfDeviceMemory { Renderer::GetGraphicsDevice().GetMemoryBudget().Usage, staging.GetIdleBytes() + staging.GetPendingBytes() };
				}
				if (!m_PerfLog->WriteFrame(frame, assets ? &*assets : nullptr, uploads, device ? &*device : nullptr))
					Application::Get().SetExitCode(1); // A requested log that is incomplete fails the run, so CI notices
			});
			ST_INFO("Writing a perf log of every frame to {}", FileSystem::ToUTF8(m_Options.PerfLogPath));
		}

		void StopPerfLog()
		{
			if (!m_PerfLog)
				return;
			FrameProfiler::Get().RemoveFrameListener(m_PerfLogListener);
			std::string error;
			if (m_PerfLog->Close(&error))
			{
				ST_INFO("Wrote the perf log of {} frames to {}", m_PerfLog->GetRowCount(), FileSystem::ToUTF8(m_PerfLog->GetPath()));
			}
			else
			{
				ST_ERROR("The perf log is incomplete: {}", error);
				Application::Get().SetExitCode(1);
			}
			m_PerfLog.reset();
		}

		void RequestScreenshot()
		{
			Application& application = Application::Get();
			const bool lastFrame = m_Options.MaxFrames && application.GetFrameCount() + 1 == *m_Options.MaxFrames;
			if (!lastFrame || m_Options.ScreenshotPath.empty())
				return;
			// This frame is already rendered: a game that shows the missing-camera message, or a cleared frame because nothing
			// plays yet, cannot be what a screenshot is for (the loading scene can).
			const bool showingMessage = m_Renderer && m_Renderer->IsShowingMessage();
			const bool loading = !m_Runtime->GetScene();
			application.RequestBackBufferCapture([path = m_Options.ScreenshotPath, showingMessage, loading](const ReadbackImage& image)
			{
				// A requested screenshot that cannot be made, or shows no game, fails the run, so scripted runs (CI) notice.
				std::string error;
				if (!ImageWriter::SavePNG(image, path, true, &error))
				{
					ST_ERROR("Screenshot failed: {}", error);
					Application::Get().SetExitCode(1);
					return;
				}
				ST_INFO("Saved screenshot to {}", FileSystem::ToUTF8(path));
				if (showingMessage)
				{
					ST_ERROR("The screenshot shows the missing-camera message instead of the game");
					Application::Get().SetExitCode(1);
				}
				if (loading)
				{
					ST_ERROR("The screenshot shows a cleared frame: the game's start scene is still loading (more --frames give it time)");
					Application::Get().SetExitCode(1);
				}
			});
		}
	private:
		RuntimeOptions m_Options;
		Scope<GameWindowService> m_WindowService;
		Scope<EngineServices::ScopedProvider<IGameWindow>> m_WindowProvider; // After the service: ends first
		Scope<GameRuntime> m_Runtime;
		Scope<GameRenderer> m_Renderer;
		Scope<FramePerfLog> m_PerfLog;
		uint32_t m_PerfLogListener = 0;
	};

	class RuntimeApplication : public Application
	{
	public:
		RuntimeApplication(const ApplicationSpecification& specification, RuntimeOptions options)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new RuntimeLayer(std::move(options)));
		}
	};

	namespace
	{

		// What is wrong with the values of the command line's options, before anything starts.
		std::optional<std::string> CheckOptions(const CommandLine& commandLine)
		{
			if ((commandLine.HasFlag("--resolution") || commandLine.GetOption("--resolution")) && !commandLine.GetSizeOption("--resolution"))
				return fmt::format("--resolution needs the pixels of the window as <width>x<height>, each from 1 to {}", CommandLine::c_MaxSizeOption);
			if (commandLine.HasFlag("--asset-budget-mb") || commandLine.GetOption("--asset-budget-mb"))
			{
				const std::optional<int64_t> megabytes = commandLine.GetIntOption("--asset-budget-mb");
				if (!megabytes || *megabytes <= 0 || *megabytes > c_MaxAssetBudgetMB)
					return fmt::format("--asset-budget-mb needs a whole number of megabytes from 1 to {}", c_MaxAssetBudgetMB);
			}
			// The screenshot is taken on the last frame, so it needs a frame count and a window.
			const std::optional<int64_t> frames = commandLine.GetIntOption("--frames");
			if (commandLine.GetOption("--screenshot") && (!frames || *frames <= 0 || commandLine.HasFlag("--headless")))
				return std::string("--screenshot needs --frames <count> and a window (not --headless)");
			if (commandLine.HasFlag("--perf-log") && commandLine.GetOption("--perf-log").value_or(std::string()).empty())
				return std::string("--perf-log needs the path of the CSV file to write");
			if (commandLine.HasFlag("--save-directory") && commandLine.GetOption("--save-directory").value_or(std::string()).empty())
				return std::string("--save-directory needs the folder to keep the game's save files in");
			return std::nullopt;
		}

	}

	ProgramRun GetProgramRun(const CommandLine& commandLine)
	{
		// Runs that end by themselves (servers, checks, screenshots, perf logs) and command lines with a mistake print into
		// the terminal that started them; a person playing in a window leaves it alone.
		if (commandLine.HasFlag("--headless") || commandLine.HasFlag("--frames") || CheckOptions(commandLine))
			return ProgramRun::Console;
		return ProgramRun::Interactive;
	}

	Application* CreateApplication(const CommandLine& commandLine)
	{
		// Games read cooked asset packs and never name the asset pipeline: the Application constructor registers the
		// engine's modules without it, and the importers are not linked into the runtime.
		RuntimeOptions options;
		if (std::optional<std::string> game = commandLine.GetOption("--game"))
			options.ManifestPath = FileSystem::FromUTF8(*game);
		else
			options.ManifestPath = GameManifest::FindForExecutable(Platform::GetExecutablePath());

		// The manifest names the game, which also names its data directory (logs, saves: SaveFiles::MakeGameDirectoryName).
		std::string error;
		std::optional<GameManifest> manifest = options.ManifestPath.empty() ? std::nullopt : GameManifest::Load(options.ManifestPath, &error);
		const std::string name = manifest ? manifest->Name : std::string("Strata Game");

		LogSpecification logSpecification;
		logSpecification.LogFile = Platform::GetUserDataDirectory(SaveFiles::MakeGameDirectoryName(name)) / "Logs" / "Game.log";
		Log::Init(logSpecification);
		ST_INFO("{} (Strata {}, {})", name, c_EngineVersion, Platform::GetName());
		// A Console run (GetProgramRun): the terminal that started it shows the mistake.
		if (const std::optional<std::string> problem = CheckOptions(commandLine))
		{
			ST_CRITICAL("{}", *problem);
			return nullptr;
		}
		if (!manifest)
		{
			Application::ReportFatalError(name, fmt::format("No game to run: {}", options.ManifestPath.empty()
				? std::string("no single .stgame manifest next to the executable (pass --game <file>)") : error), c_GameFilesAdvice);
			return nullptr;
		}

		// The options' values were checked above (CheckOptions).
		ApplicationSpecification specification;
		specification.Name = name;
		specification.CommandLineArgs = commandLine;
		// Headless runs the simulation only, like a dedicated server: no window and no GPU.
		specification.Headless = commandLine.HasFlag("--headless");
		specification.EnableRenderer = !specification.Headless;
		if (specification.Headless)
			specification.MaxFrameRate = c_HeadlessFrameRate;
		specification.EnableImGui = false;
		specification.Window.Title = name;
		specification.Window.Width = manifest->WindowWidth;
		specification.Window.Height = manifest->WindowHeight;
		specification.Window.Fullscreen = manifest->Fullscreen && !commandLine.HasFlag("--windowed");
		specification.Window.VSync = manifest->VSync;
		if (const std::optional<std::pair<uint32_t, uint32_t>> resolution = commandLine.GetSizeOption("--resolution"))
		{
			// A window of exactly that many pixels, whatever the display's scale: perf logs and screenshots that compare
			// across machines and displays.
			specification.Window.Width = resolution->first;
			specification.Window.Height = resolution->second;
			specification.Window.SizeInPixels = true;
			specification.Window.Fullscreen = false;
		}
		if (std::optional<int64_t> frames = commandLine.GetIntOption("--frames"); frames && *frames > 0)
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		// Megabytes of GPU texture memory the game's assets may keep resident (1 MB = 1048576 bytes).
		if (const std::optional<int64_t> megabytes = commandLine.GetIntOption("--asset-budget-mb"))
			options.TextureBudget = static_cast<uint64_t>(*megabytes) << 20;
		if (std::optional<std::string> screenshot = commandLine.GetOption("--screenshot"))
			options.ScreenshotPath = FileSystem::FromUTF8(*screenshot);
		if (std::optional<std::string> perfLog = commandLine.GetOption("--perf-log"))
			options.PerfLogPath = FileSystem::FromUTF8(*perfLog);
		if (std::optional<std::string> saves = commandLine.GetOption("--save-directory"))
		{
			// Saves somewhere else than the player's data directory: tests and tools that must not touch it.
			options.SaveDirectory = FileSystem::FromUTF8(*saves);
			ST_INFO("Keeping the game's save files in {}", FileSystem::ToUTF8(options.SaveDirectory));
		}
		return new RuntimeApplication(specification, std::move(options));
	}

}
