#include <Strata.h>
#include <Strata/Asset/AssetImporter.h>
#include <Strata/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace Strata
{

	constexpr uint32_t c_HeadlessFrameRate = 60;
	constexpr int64_t c_MaxIdleTimeoutSeconds = 7 * 24 * 60 * 60; // A week

	class EditorApplication : public Application
	{
	public:
		EditorApplication(const ApplicationSpecification& specification, const EditorOptions& options)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new EditorLayer(options));
		}
	};

	Application* CreateApplication(const CommandLine& commandLine)
	{
		const std::filesystem::path userData = Platform::GetUserDataDirectory("Strata");

		LogSpecification logSpecification;
		logSpecification.LogFile = userData / "Logs" / "StrataEditor.log";
		Log::Init(logSpecification);
		ST_INFO("Strata Editor {} ({})", c_EngineVersion, Platform::GetName());

		// The editor imports assets: it registers the modules with the asset pipeline, before the Application constructor
		// would register them without it.
		Engine::ModuleRegistrationOptions modules;
		modules.AssetPipeline = RegisterAssetPipeline;
		Engine::RegisterBuiltinModules(modules);

		EditorOptions options;
		options.ShowImGuiDemo = commandLine.HasFlag("--imgui-demo");
		if (std::optional<std::string> project = commandLine.GetOption("--project"))
			options.ProjectPath = FileSystem::FromUTF8(*project);
		if (std::optional<std::string> screenshot = commandLine.GetOption("--screenshot"))
			options.ScreenshotPath = FileSystem::FromUTF8(*screenshot);
		if (std::optional<std::string> commands = commandLine.GetOption("--commands"))
			options.CommandScript = FileSystem::FromUTF8(*commands);
		// Scripted runs whose length is unknown (script builds) end when their command script has finished.
		options.QuitAfterCommands = commandLine.HasFlag("--quit-after-commands");

		// Automation (StrataCLI, MCP): on by default, on a free loopback port unless --automation-port picks one.
		options.EnableAutomation = !commandLine.HasFlag("--no-automation");
		if (commandLine.HasFlag("--automation-port"))
		{
			const std::optional<int64_t> port = commandLine.GetIntOption("--automation-port");
			if (!port || *port < 0 || *port > UINT16_MAX)
			{
				ST_ERROR("--automation-port expects a port number from 0 to 65535 (0 picks a free port)");
				return nullptr;
			}
			options.AutomationPort = static_cast<uint16_t>(*port);
		}
		// Editors started by tools close themselves once no client has been connected for this long.
		if (commandLine.HasFlag("--idle-timeout"))
		{
			const std::optional<int64_t> seconds = commandLine.GetIntOption("--idle-timeout");
			if (!seconds || *seconds < 0 || *seconds > c_MaxIdleTimeoutSeconds)
			{
				ST_ERROR("--idle-timeout expects a number of seconds from 0 to {} (0: never)", c_MaxIdleTimeoutSeconds);
				return nullptr;
			}
			options.IdleTimeout = std::chrono::seconds(*seconds);
		}

		ApplicationSpecification specification;
		specification.Name = "Strata Editor";
		specification.CommandLineArgs = commandLine;
		// --no-gpu: automation that needs no rendering (export, asset processing) on machines without a GPU.
		specification.EnableRenderer = !commandLine.HasFlag("--no-gpu");
		specification.Headless = commandLine.HasFlag("--headless") || !specification.EnableRenderer;
		specification.EnableImGui = !specification.Headless;
		// Without a window there is no vsync: a headless editor waiting for automation would otherwise spin a CPU core,
		// and playing scenes advance about as they would in a 60 Hz game.
		if (specification.Headless)
			specification.MaxFrameRate = c_HeadlessFrameRate;
		// Scripted runs (a fixed number of frames) never overwrite the user's saved panel layout.
		if (!commandLine.GetIntOption("--frames"))
			specification.ImGuiLayoutFile = userData / "EditorLayout.ini";
		specification.Window.Title = "Strata Editor";
		specification.Window.Width = 1600;
		specification.Window.Height = 900;
		specification.Window.Maximized = !commandLine.HasFlag("--windowed");
		if (std::optional<int64_t> frames = commandLine.GetIntOption("--frames"); frames && *frames > 0)
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		options.Headless = specification.Headless;

		return new EditorApplication(specification, options);
	}

}
