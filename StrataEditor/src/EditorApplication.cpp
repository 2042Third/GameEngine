#include <Strata.h>
#include <Strata/Core/EntryPoint.h>

#include "EditorLayer.h"

namespace Strata
{

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

		EditorOptions options;
		options.ShowImGuiDemo = commandLine.HasFlag("--imgui-demo");
		if (std::optional<std::string> project = commandLine.GetOption("--project"))
			options.ProjectPath = FileSystem::FromUTF8(*project);
		if (std::optional<std::string> screenshot = commandLine.GetOption("--screenshot"))
			options.ScreenshotPath = FileSystem::FromUTF8(*screenshot);

		ApplicationSpecification specification;
		specification.Name = "Strata Editor";
		specification.CommandLineArgs = commandLine;
		specification.Headless = commandLine.HasFlag("--headless");
		specification.EnableImGui = !specification.Headless;
		specification.ImGuiLayoutFile = userData / "EditorLayout.ini";
		specification.Window.Title = "Strata Editor";
		specification.Window.Width = 1600;
		specification.Window.Height = 900;
		specification.Window.Maximized = !commandLine.HasFlag("--windowed");
		if (std::optional<int64_t> frames = commandLine.GetIntOption("--frames"); frames && *frames > 0)
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;

		return new EditorApplication(specification, options);
	}

}
