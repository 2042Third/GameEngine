#include <Strata.h>
#include <Strata/Core/EntryPoint.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Runtime/GameRenderer.h>
#include <Strata/Runtime/GameRuntime.h>

namespace Strata
{

	// Exit codes: 1 when the game cannot start, 2 when its scripts crashed in a headless run.
	constexpr int c_ScriptCrashExitCode = 2;

	struct RuntimeOptions
	{
		std::filesystem::path ManifestPath;
		std::filesystem::path ScreenshotPath; // Saves the window to this PNG on the last of MaxFrames frames
		std::optional<uint64_t> MaxFrames;
	};

	// Runs the game of a manifest and, with a window, renders it every frame. Startup failures end the process with exit
	// code 1.
	class RuntimeLayer : public Layer
	{
	public:
		explicit RuntimeLayer(RuntimeOptions options)
			: Layer("RuntimeLayer"), m_Options(std::move(options))
		{
		}

		void OnAttach() override
		{
			std::string error;
			m_Runtime = GameRuntime::Create(m_Options.ManifestPath, &error);
			if (!m_Runtime)
			{
				ST_CRITICAL("Cannot start the game '{}': {}", FileSystem::ToUTF8(m_Options.ManifestPath), error);
				Application::Get().SetExitCode(1);
				Application::Get().Close();
				return;
			}
			// Headless runs (servers, CI) have no window to render to.
			if (Application::Get().GetWindow() && Renderer::IsInitialized())
				m_Renderer = CreateScope<GameRenderer>();
		}

		void OnDetach() override
		{
			m_Renderer.reset();
			m_Runtime.reset();
		}

		void OnUpdate(Timestep timestep) override
		{
			if (!m_Runtime)
				return;
			m_Runtime->Update(timestep);
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
			if (m_Renderer)
			{
				// Frames of minimized windows are skipped by the application; this frame has a back buffer.
				GraphicsDevice& device = Renderer::GetGraphicsDevice();
				if (nvrhi::IFramebuffer* backBuffer = device.GetBackBufferFramebuffer())
					m_Renderer->Render(m_Runtime->GetScene(), backBuffer, device.GetBackBufferSize());
			}
			RequestScreenshot();
		}
	private:
		void RequestScreenshot()
		{
			Application& application = Application::Get();
			const bool lastFrame = m_Options.MaxFrames && application.GetFrameCount() + 1 == *m_Options.MaxFrames;
			if (!lastFrame || m_Options.ScreenshotPath.empty())
				return;
			// This frame is already rendered: a game that shows the missing-camera message cannot be what a screenshot is for.
			const bool showingMessage = m_Renderer && m_Renderer->IsShowingMessage();
			application.RequestBackBufferCapture([path = m_Options.ScreenshotPath, showingMessage](const ReadbackImage& image)
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
			});
		}
	private:
		RuntimeOptions m_Options;
		Scope<GameRuntime> m_Runtime;
		Scope<GameRenderer> m_Renderer;
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

	Application* CreateApplication(const CommandLine& commandLine)
	{
		RuntimeOptions options;
		if (std::optional<std::string> game = commandLine.GetOption("--game"))
			options.ManifestPath = FileSystem::FromUTF8(*game);
		else
			options.ManifestPath = GameManifest::FindForExecutable(Platform::GetExecutablePath());

		// The manifest names the game, which also names its log and data directory.
		std::string error;
		std::optional<GameManifest> manifest = options.ManifestPath.empty() ? std::nullopt : GameManifest::Load(options.ManifestPath, &error);
		const std::string name = manifest ? manifest->Name : std::string("Strata Game");

		LogSpecification logSpecification;
		logSpecification.LogFile = Platform::GetUserDataDirectory(name) / "Logs" / "Game.log";
		Log::Init(logSpecification);
		ST_INFO("{} (Strata {}, {})", name, c_EngineVersion, Platform::GetName());
		if (!manifest)
		{
			ST_CRITICAL("No game to run: {}", options.ManifestPath.empty() ? std::string("no single .stgame manifest next to the executable (pass --game <file>)") : error);
			return nullptr;
		}

		ApplicationSpecification specification;
		specification.Name = name;
		specification.CommandLineArgs = commandLine;
		// Headless runs the simulation only, like a dedicated server: no window and no GPU.
		specification.Headless = commandLine.HasFlag("--headless");
		specification.EnableRenderer = !specification.Headless;
		specification.EnableImGui = false;
		specification.Window.Title = name;
		specification.Window.Width = manifest->WindowWidth;
		specification.Window.Height = manifest->WindowHeight;
		specification.Window.Fullscreen = manifest->Fullscreen && !commandLine.HasFlag("--windowed");
		specification.Window.VSync = manifest->VSync;
		if (std::optional<int64_t> frames = commandLine.GetIntOption("--frames"); frames && *frames > 0)
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		if (std::optional<std::string> screenshot = commandLine.GetOption("--screenshot"))
		{
			// The screenshot is taken on the last frame, so it needs a frame count and a window.
			if (!options.MaxFrames || specification.Headless)
			{
				ST_CRITICAL("--screenshot needs --frames <count> and a window (not --headless)");
				return nullptr;
			}
			options.ScreenshotPath = FileSystem::FromUTF8(*screenshot);
		}
		return new RuntimeApplication(specification, std::move(options));
	}

}
