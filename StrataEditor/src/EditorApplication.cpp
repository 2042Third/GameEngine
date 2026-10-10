#include <Strata.h>
#include <Strata/Core/EntryPoint.h>
#include <Strata/ImGui/ImGuiLayer.h>

#include "EditorHost.h"
#include "EditorLayer.h"
#include "UI/EditorFonts.h"
#include "UI/Theme.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace Strata
{

	constexpr uint32_t c_HeadlessFrameRate = 60;
	constexpr int64_t c_MaxIdleTimeoutSeconds = 7 * 24 * 60 * 60; // A week

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
		bool HasGraphicsDevice() const override { return m_Application.GetGraphicsDevice() != nullptr; }

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
			if (!IsRunning())
				return;
			// The editor's look: the Bedrock theme at the UI scale, and its own fonts (ImGui's built-in font is never used).
			if (ImGuiLayer* imgui = GetImGuiLayer(); imgui && imgui->IsInitialized())
			{
				if (options.UIScale)
					imgui->SetContentScaleOverride(*options.UIScale);
				imgui->SetStyleCallback(UI::ApplyTheme);
				if (!UI::EditorFonts::Load())
					ST_ERROR("The editor's fonts are unavailable; the UI uses ImGui's default font");
			}
			// The layer owns its host; the application outlives its layers.
			PushLayer(new EditorLayer(options, CreateScope<ApplicationEditorHost>(*this)));
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
		// A fixed UI scale instead of the display's (e.g. 1 to check the UI at 100% on a 150% display).
		if (commandLine.HasFlag("--ui-scale"))
		{
			// Parsed as a JSON number: independent of the C locale, unlike strtof.
			const std::optional<std::string> value = commandLine.GetOption("--ui-scale");
			const nlohmann::json parsed = value ? nlohmann::json::parse(*value, nullptr, false) : nlohmann::json();
			const double scale = parsed.is_number() ? parsed.get<double>() : 0.0;
			if (!std::isfinite(scale) || scale < ImGuiLayer::c_MinScale || scale > ImGuiLayer::c_MaxScale)
			{
				ST_ERROR("--ui-scale expects a factor from {} to {} (1 is 100%)", ImGuiLayer::c_MinScale, ImGuiLayer::c_MaxScale);
				return nullptr;
			}
			options.UIScale = static_cast<float>(scale);
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
		if (std::optional<int64_t> frames = commandLine.GetIntOption("--frames"); frames && *frames > 0)
			specification.MaxFrames = static_cast<uint64_t>(*frames);
		options.MaxFrames = specification.MaxFrames;
		options.Headless = specification.Headless;

		return new EditorApplication(specification, options);
	}

}
