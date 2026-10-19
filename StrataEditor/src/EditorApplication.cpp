#include <Strata/Asset/AssetImporter.h>
#include <Strata/Audio/AudioModule.h>
#include <Strata/Core/Application.h>
#include <Strata/Core/CommandLineOptions.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Timer.h>
#include <Strata/Core/Version.h>
#include <Strata/Engine/EntryPoint.h>
#include <Strata/ImGui/ImGuiLayer.h>
#include <Strata/ImGui/ImGuiModule.h>
#include <Strata/Project/ProjectModule.h>
#include <Strata/Renderer/GraphicsDevice.h>
#include <Strata/Renderer/GraphicsModule.h>
#include <Strata/Renderer/Renderer.h>

#include "Editor/ExternalEditor.h"
#include "EditorHost.h"
#include "EditorIcon.h"
#include "EditorLayer.h"
#include "UI/EditorFonts.h"
#include "UI/Theme.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	constexpr uint32_t c_HeadlessFrameRate = 60;
	constexpr int64_t c_MaxIdleTimeoutSeconds = 7 * 24 * 60 * 60; // A week

	namespace
	{

		// Every option the editor understands (its usage, and what CommandLineOptions::FindUnknownArgument accepts).
		constexpr CommandLineOption c_Arguments[] = {
			{ "--project", "<directory or .stproj>", "Open this project" },
			{ "--commands", "<script.json>", "Run a command script at startup (a JSON array of {command, parameters, expect, bind})" },
			{ "--quit-after-commands", nullptr, "Close once the command script finished" },
			{ "--frames", "<count>", "Stop after this many frames" },
			{ "--screenshot", "<file.png>", "Save the last frame (with --frames or --quit-after-commands)" },
			{ "--headless", nullptr, "Run without a window (rendering stays available for captures)" },
			{ "--no-gpu", nullptr, "Run without a graphics device (implies --headless)" },
			{ "--windowed", nullptr, "Open a 1600x900 window instead of a maximized one" },
			{ "--ui-scale", "<factor>", "Draw the UI at this scale instead of the display's (1 is 100%)" },
			{ "--layout", "<file.ini>", "Keep the panel layout in this file" },
			{ "--no-automation", nullptr, "Do not serve automation (StrataCLI, MCP)" },
			{ "--automation-port", "<port>", "Serve automation on this port (0, the default, picks a free one)" },
			{ "--idle-timeout", "<seconds>", "Close after this long without an automation client (0: never)" },
			{ "--imgui-demo", nullptr, "Offer ImGui's demo window in the Help menu" },
			{ "--version", nullptr, "Print the version and exit" },
			{ "--help", nullptr, "Print this help and exit" } };

		std::string GetUsage()
		{
			return CommandLineOptions::FormatUsage("StrataEditor", c_Arguments);
		}

		// The --ui-scale factor, parsed as a JSON number (independent of the C locale, unlike strtof); nullopt when it is not a
		// number within ImGuiLayer's range.
		std::optional<float> ParseUIScale(const std::string& value)
		{
			const nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
			const double scale = parsed.is_number() ? parsed.get<double>() : 0.0;
			if (!std::isfinite(scale) || scale < ImGuiLayer::c_MinScale || scale > ImGuiLayer::c_MaxScale)
				return std::nullopt;
			return static_cast<float>(scale);
		}

		// What is wrong with the values of the options that take numbers (FindUnknownArgument made sure they have values).
		std::optional<std::string> CheckValues(const CommandLine& commandLine)
		{
			if (commandLine.HasFlag("--frames"))
			{
				const std::optional<int64_t> frames = commandLine.GetIntOption("--frames");
				if (!frames || *frames <= 0)
					return std::string("--frames expects a positive number of frames");
			}
			if (commandLine.HasFlag("--automation-port"))
			{
				const std::optional<int64_t> port = commandLine.GetIntOption("--automation-port");
				if (!port || *port < 0 || *port > UINT16_MAX)
					return std::string("--automation-port expects a port number from 0 to 65535 (0 picks a free port)");
			}
			if (commandLine.HasFlag("--idle-timeout"))
			{
				const std::optional<int64_t> seconds = commandLine.GetIntOption("--idle-timeout");
				if (!seconds || *seconds < 0 || *seconds > c_MaxIdleTimeoutSeconds)
					return fmt::format("--idle-timeout expects a number of seconds from 0 to {} (0: never)", c_MaxIdleTimeoutSeconds);
			}
			if (commandLine.HasFlag("--ui-scale") && !ParseUIScale(commandLine.GetOption("--ui-scale").value_or(std::string())))
				return fmt::format("--ui-scale expects a factor from {} to {} (1 is 100%)", ImGuiLayer::c_MinScale, ImGuiLayer::c_MaxScale);
			return std::nullopt;
		}

	}

	// The editor layer's view of the application.
	class ApplicationEditorHost final : public EditorHost
	{
	public:
		explicit ApplicationEditorHost(Application& application)
			: m_Application(application)
		{
		}

		bool IsRunning() const override { return m_Application.IsRunning(); }
		void Close() override { m_Application.Close(); }
		void SetExitCode(int exitCode) override { m_Application.SetExitCode(exitCode); }
		uint64_t GetFrameCount() const override { return m_Application.GetFrameCount(); }
		double GetTime() const override { return Time::GetTime(); }
		std::optional<double> GetProcessUptime() const override { return Platform::GetProcessUptime(); }
		bool HasGraphicsDevice() const override { return GetDevice() != nullptr; }

		std::optional<GraphicsDeviceInfo> GetGraphicsDeviceInfo() const override
		{
			const GraphicsDevice* device = GetDevice();
			return device ? std::optional<GraphicsDeviceInfo>(device->GetInfo()) : std::nullopt;
		}

		bool HasWindow() const override { return m_Application.GetWindow() != nullptr; }

		void SetWindowTitle(const std::string& title) override
		{
			if (Window* window = m_Application.GetWindow())
				window->SetTitle(title);
		}

		glm::uvec2 GetWindowSize() const override
		{
			const Window* window = m_Application.GetWindow();
			return window ? glm::uvec2(window->GetWidth(), window->GetHeight()) : glm::uvec2(0);
		}

		bool IsWindowFocused() const override
		{
			const Window* window = m_Application.GetWindow();
			return window && window->IsFocused();
		}

		float GetUIScale() const override
		{
			const ImGuiModule* imgui = m_Application.GetModules().Find<ImGuiModule>();
			const ImGuiLayer* layer = imgui ? imgui->GetLayer() : nullptr;
			return layer ? layer->GetUIScale() : 1.0f;
		}

		void SetMaxFrameRate(uint32_t framesPerSecond) override { m_Application.SetMaxFrameRate(framesPerSecond); }
		uint32_t GetMaxFrameRate() const override { return m_Application.GetMaxFrameRate(); }
		double GetLastFrameWorkTime() const override { return m_Application.GetLastFrameWorkTime(); }

		void RequestScreenshot(std::function<void(const ReadbackImage&)> callback) override
		{
			if (GraphicsModule* graphics = m_Application.GetModules().Find<GraphicsModule>())
			{
				graphics->RequestBackBufferCapture(std::move(callback));
				return;
			}
			// Nothing will ever be rendered: the answer is an empty image, on the next frame as a capture's would be.
			m_Application.SubmitToMainThread([callback = std::move(callback)]() { callback(ReadbackImage()); });
		}
	private:
		GraphicsDevice* GetDevice() const
		{
			const GraphicsModule* graphics = m_Application.GetModules().Find<GraphicsModule>();
			return graphics ? graphics->GetDevice() : nullptr;
		}
	private:
		Application& m_Application;
	};

	class EditorApplication : public Application
	{
	public:
		EditorApplication(const ApplicationSpecification& specification, const EditorOptions& options)
			: Application(specification)
		{
			if (Window* window = GetWindow())
				window->SetIcon(GetEditorWindowIcon());
			if (!IsRunning())
				return;
			// The editor's look: the Bedrock theme at the UI scale, and its own fonts (ImGui's built-in font is never used).
			const ImGuiModule* imguiModule = GetModules().Find<ImGuiModule>();
			if (ImGuiLayer* imgui = imguiModule ? imguiModule->GetLayer() : nullptr)
			{
				if (options.UIScale)
					imgui->SetContentScaleOverride(*options.UIScale);
				imgui->SetStyleCallback(UI::ApplyTheme);
				if (UI::EditorFonts::Load())
					UI::EditorFonts::BeginLoadingFallback();
				else
					ST_ERROR("The editor's fonts are unavailable; the UI uses ImGui's default font");
			}
			// The layer owns its host; the application outlives its layers.
			PushLayer(new EditorLayer(options, CreateScope<ApplicationEditorHost>(*this)));
		}
	};

	ProgramRun GetProgramRun(const CommandLine& commandLine)
	{
		// What it prints is the point: information, a usage error, and the log of runs that tools and command scripts drive
		// (they end by themselves or through automation). A person's window leaves the terminal it was started from alone.
		if (commandLine.HasFlag("--help") || commandLine.HasFlag("-h") || commandLine.HasFlag("--version"))
			return ProgramRun::Console;
		if (CommandLineOptions::FindUnknownArgument(commandLine, c_Arguments) || CheckValues(commandLine))
			return ProgramRun::Console;
		for (const char* scripted : { "--headless", "--no-gpu", "--frames", "--commands", "--quit-after-commands" })
		{
			if (commandLine.HasFlag(scripted))
				return ProgramRun::Console;
		}
		return ProgramRun::Interactive;
	}

	std::optional<ApplicationLaunch> CreateApplication(const CommandLine& commandLine)
	{
		// Answered before anything starts: no log file, window or automation session.
		if (commandLine.HasFlag("--help") || commandLine.HasFlag("-h"))
		{
			std::fputs(GetUsage().c_str(), stdout);
			Application::SetStartupExitCode(0);
			return std::nullopt;
		}
		if (commandLine.HasFlag("--version"))
		{
			std::printf("Strata Editor %s\n", std::string(c_EngineVersion).c_str());
			Application::SetStartupExitCode(0);
			return std::nullopt;
		}
		std::optional<std::string> problem = CommandLineOptions::FindUnknownArgument(commandLine, c_Arguments);
		if (!problem)
			problem = CheckValues(commandLine);
		if (problem)
		{
			std::fprintf(stderr, "%s\n\n%s", problem->c_str(), GetUsage().c_str());
			Application::SetStartupExitCode(CommandLineOptions::c_UsageExitCode);
			return std::nullopt;
		}

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
		if (std::optional<std::string> commands = commandLine.GetOption("--commands"))
			options.CommandScript = FileSystem::FromUTF8(*commands);
		// Scripted runs whose length is unknown (script builds) end when their command script has finished.
		options.QuitAfterCommands = commandLine.HasFlag("--quit-after-commands");
		// The projects a person or an agent opens go to the user's recent projects; scripted runs only read them.
		options.RecentProjectsFile = RecentProjects::GetDefaultFile().value_or(std::filesystem::path());
		options.RecentProjectsReadOnly = !options.CommandScript.empty() || commandLine.GetIntOption("--frames").has_value();
		// The person's code editor for script sources (script.setExternalEditor).
		options.EditorSettingsFile = ExternalEditor::GetDefaultSettingsFile().value_or(std::filesystem::path());

		// Automation (StrataCLI, MCP): on by default, on a free loopback port unless --automation-port picks one. The values
		// of the options were checked before anything started (CheckValues).
		options.EnableAutomation = !commandLine.HasFlag("--no-automation");
		if (const std::optional<int64_t> port = commandLine.GetIntOption("--automation-port"))
			options.AutomationPort = static_cast<uint16_t>(*port);
		// Editors started by tools close themselves once no client has been connected for this long.
		if (const std::optional<int64_t> seconds = commandLine.GetIntOption("--idle-timeout"))
			options.IdleTimeout = std::chrono::seconds(*seconds);
		// A fixed UI scale instead of the display's (e.g. 1 to check the UI at 100% on a 150% display).
		if (const std::optional<std::string> scale = commandLine.GetOption("--ui-scale"))
			options.UIScale = ParseUIScale(*scale);

		ApplicationSpecification specification;
		specification.Name = "Strata Editor";
		specification.CommandLineArgs = commandLine;
		// --no-gpu: automation that needs no rendering (export, asset processing) on machines without a GPU.
		const bool graphics = !commandLine.HasFlag("--no-gpu");
		specification.Headless = commandLine.HasFlag("--headless") || !graphics;
		// Without a window there is no vsync: a headless editor waiting for automation would otherwise spin a CPU core,
		// and playing scenes advance about as they would in a 60 Hz game.
		if (specification.Headless)
			specification.MaxFrameRate = c_HeadlessFrameRate;
		// The modules: the device (offscreen when headless, for captures and thumbnails), sound (mixed without a device when
		// headless), the UI in a window, the project.
		if (graphics)
			specification.Modules.push_back(GraphicsModule::Factory());
		specification.Modules.push_back(AudioModule::Factory({ .NullDevice = specification.Headless }));
		if (!specification.Headless)
		{
			// The panel layout: --layout picks the file; otherwise the user's, except in scripted runs (a fixed number of
			// frames), which never overwrite it.
			ImGuiModuleSpecification imgui;
			if (std::optional<std::string> layout = commandLine.GetOption("--layout"))
				imgui.LayoutFile = FileSystem::FromUTF8(*layout);
			else if (!commandLine.GetIntOption("--frames"))
				imgui.LayoutFile = userData / "EditorLayout.ini";
			specification.Modules.push_back(ImGuiModule::Factory(std::move(imgui)));
		}
		specification.Modules.push_back(ProjectModule::Factory());
		specification.Window.Title = "Strata Editor";
		specification.Window.Width = 1600;
		specification.Window.Height = 900;
		specification.Window.Maximized = !commandLine.HasFlag("--windowed");
		if (const std::optional<int64_t> frames = commandLine.GetIntOption("--frames"))
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		options.Headless = specification.Headless;
		// A person's window shows the start scene loading; scripted runs and headless editors (driven by tools) start with it open.
		options.OpenStartSceneInBackground = !options.Headless && !options.MaxFrames && options.CommandScript.empty();

		// The editor imports assets: the composition root registers the modules with the asset pipeline.
		ApplicationLaunch launch;
		launch.Registration.AssetPipeline = RegisterAssetPipeline;
		launch.Create = [specification, options]() -> Scope<Application> { return CreateScope<EditorApplication>(specification, options); };
		return launch;
	}

}
