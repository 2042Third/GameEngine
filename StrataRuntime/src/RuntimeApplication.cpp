#include <Strata.h>
#include <Strata/Core/EntryPoint.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Runtime/GameRuntime.h>

namespace Strata
{

	// Runs the game of a manifest. Startup failures end the process with exit code 1.
	class RuntimeLayer : public Layer
	{
	public:
		explicit RuntimeLayer(std::filesystem::path manifestPath)
			: Layer("RuntimeLayer"), m_ManifestPath(std::move(manifestPath))
		{
		}

		void OnAttach() override
		{
			std::string error;
			m_Runtime = GameRuntime::Create(m_ManifestPath, &error);
			if (!m_Runtime)
			{
				ST_CRITICAL("Cannot start the game '{}': {}", FileSystem::ToUTF8(m_ManifestPath), error);
				Application::Get().SetExitCode(1);
				Application::Get().Close();
			}
		}

		void OnDetach() override
		{
			m_Runtime.reset();
		}

		void OnUpdate(Timestep timestep) override
		{
			if (m_Runtime)
				m_Runtime->Update(timestep);
		}
	private:
		std::filesystem::path m_ManifestPath;
		Scope<GameRuntime> m_Runtime;
	};

	class RuntimeApplication : public Application
	{
	public:
		RuntimeApplication(const ApplicationSpecification& specification, std::filesystem::path manifestPath)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new RuntimeLayer(std::move(manifestPath)));
		}
	};

	Application* CreateApplication(const CommandLine& commandLine)
	{
		std::filesystem::path manifestPath;
		if (std::optional<std::string> game = commandLine.GetOption("--game"))
			manifestPath = FileSystem::FromUTF8(*game);
		else
			manifestPath = GameManifest::FindForExecutable(Platform::GetExecutablePath());

		// The manifest names the game, which also names its log and data directory.
		std::string error;
		std::optional<GameManifest> manifest = manifestPath.empty() ? std::nullopt : GameManifest::Load(manifestPath, &error);
		const std::string name = manifest ? manifest->Name : std::string("Strata Game");

		LogSpecification logSpecification;
		logSpecification.LogFile = Platform::GetUserDataDirectory(name) / "Logs" / "Game.log";
		Log::Init(logSpecification);
		ST_INFO("{} (Strata {}, {})", name, c_EngineVersion, Platform::GetName());
		if (!manifest)
		{
			ST_CRITICAL("No game to run: {}", manifestPath.empty() ? std::string("no single .stgame manifest next to the executable (pass --game <file>)") : error);
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
		return new RuntimeApplication(specification, manifestPath);
	}

}
