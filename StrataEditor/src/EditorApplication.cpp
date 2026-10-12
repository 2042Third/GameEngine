#include <Strata.h>
#include <Strata/Asset/AssetImporter.h>
#include <Strata/Core/EntryPoint.h>
#include <Strata/ImGui/ImGuiLayer.h>

#include "EditorHost.h"
#include "EditorIcon.h"
#include "EditorLayer.h"
#include "UI/EditorFonts.h"
#include "UI/Theme.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	constexpr uint32_t c_HeadlessFrameRate = 60;
	constexpr int64_t c_MaxIdleTimeoutSeconds = 7 * 24 * 60 * 60; // A week
	// The exit code of a command line the editor does not understand (like StrataCLI's usage errors).
	constexpr int c_UsageExitCode = 3;

	namespace
	{

		struct EditorArgument
		{
			const char* Name;
			const char* Value; // What follows it ("--frames 120" or "--frames=120"); null for flags
			const char* Description;
		};

		constexpr EditorArgument c_Arguments[] = {
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
			std::string usage = "Usage: StrataEditor [options]\n\nOptions:\n";
			for (const EditorArgument& argument : c_Arguments)
			{
				const std::string name = argument.Value ? fmt::format("{} {}", argument.Name, argument.Value) : std::string(argument.Name);
				usage += fmt::format("  {:<36} {}\n", name, argument.Description);
			}
			return usage;
		}

		// What is wrong with the command line: an argument the editor does not know, or an option without its value.
		std::optional<std::string> CheckArguments(const CommandLine& commandLine)
		{
			for (size_t index = 1; index < commandLine.GetCount(); index++)
			{
				const std::string& text = commandLine[index];
				const size_t equals = text.find('=');
				const std::string_view name = std::string_view(text).substr(0, equals);
				const EditorArgument* argument = nullptr;
				for (const EditorArgument& candidate : c_Arguments)
				{
					if (name == candidate.Name)
						argument = &candidate;
				}
				if (!argument)
					return fmt::format("Unknown argument '{}'", text);
				if (!argument->Value && equals != std::string::npos)
					return fmt::format("{} takes no value", argument->Name);
				if (argument->Value && equals == std::string::npos)
				{
					if (index + 1 >= commandLine.GetCount())
						return fmt::format("{} needs a value: {} {}", argument->Name, argument->Name, argument->Value);
					index++; // Its value
				}
			}
			return std::nullopt;
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

		// What is wrong with the values of the options that take numbers (CheckArguments made sure they have values).
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
		bool HasGraphicsDevice() const override { return m_Application.GetGraphicsDevice() != nullptr; }

		std::optional<GraphicsDeviceInfo> GetGraphicsDeviceInfo() const override
		{
			const GraphicsDevice* device = m_Application.GetGraphicsDevice();
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
			const ImGuiLayer* imgui = m_Application.GetImGuiLayer();
			return imgui ? imgui->GetUIScale() : 1.0f;
		}

		void SetMaxFrameRate(uint32_t framesPerSecond) override { m_Application.SetMaxFrameRate(framesPerSecond); }
		uint32_t GetMaxFrameRate() const override { return m_Application.GetMaxFrameRate(); }
		double GetLastFrameWorkTime() const override { return m_Application.GetLastFrameWorkTime(); }

		void RequestScreenshot(std::function<void(const ReadbackImage&)> callback) override
		{
			m_Application.RequestBackBufferCapture(std::move(callback));
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
			if (ImGuiLayer* imgui = GetImGuiLayer(); imgui && imgui->IsInitialized())
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

	Application* CreateApplication(const CommandLine& commandLine)
	{
		// Answered before anything starts: no log file, window or automation session.
		if (commandLine.HasFlag("--help") || commandLine.HasFlag("-h"))
		{
			std::fputs(GetUsage().c_str(), stdout);
			Application::SetStartupExitCode(0);
			return nullptr;
		}
		if (commandLine.HasFlag("--version"))
		{
			std::printf("Strata Editor %s\n", std::string(c_EngineVersion).c_str());
			Application::SetStartupExitCode(0);
			return nullptr;
		}
		std::optional<std::string> problem = CheckArguments(commandLine);
		if (!problem)
			problem = CheckValues(commandLine);
		if (problem)
		{
			std::fprintf(stderr, "%s\n\n%s", problem->c_str(), GetUsage().c_str());
			Application::SetStartupExitCode(c_UsageExitCode);
			return nullptr;
		}

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
		// The projects a person or an agent opens go to the user's recent projects; scripted runs only read them.
		options.RecentProjectsFile = RecentProjects::GetDefaultFile().value_or(std::filesystem::path());
		options.RecentProjectsReadOnly = !options.CommandScript.empty() || commandLine.GetIntOption("--frames").has_value();

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
		specification.EnableRenderer = !commandLine.HasFlag("--no-gpu");
		specification.Headless = commandLine.HasFlag("--headless") || !specification.EnableRenderer;
		specification.EnableImGui = !specification.Headless;
		// Without a window there is no vsync: a headless editor waiting for automation would otherwise spin a CPU core,
		// and playing scenes advance about as they would in a 60 Hz game.
		if (specification.Headless)
			specification.MaxFrameRate = c_HeadlessFrameRate;
		// The panel layout: --layout picks the file; otherwise the user's, except in scripted runs (a fixed number of
		// frames), which never overwrite it.
		if (std::optional<std::string> layout = commandLine.GetOption("--layout"))
			specification.ImGuiLayoutFile = FileSystem::FromUTF8(*layout);
		else if (!commandLine.GetIntOption("--frames"))
			specification.ImGuiLayoutFile = userData / "EditorLayout.ini";
		specification.Window.Title = "Strata Editor";
		specification.Window.Width = 1600;
		specification.Window.Height = 900;
		specification.Window.Maximized = !commandLine.HasFlag("--windowed");
		if (const std::optional<int64_t> frames = commandLine.GetIntOption("--frames"))
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		options.Headless = specification.Headless;

		return new EditorApplication(specification, options);
	}

}
