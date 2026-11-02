#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/EngineModule.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/Window.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// The window seam: what an Application does with the window its backend (or a test's factory) makes.
namespace
{

	// Records whether the application started it.
	class StartRecordingModule final : public EngineModule
	{
	public:
		explicit StartRecordingModule(bool& started)
			: m_Started(started)
		{
		}

		std::string_view GetName() const override { return "StartRecording"; }
		bool OnInit(EngineContext&) override
		{
			m_Started = true;
			return true;
		}
	private:
		bool& m_Started;
	};

	ApplicationSpecification MakeWindowedSpecification()
	{
		ApplicationSpecification specification;
		specification.Name = "WindowBackendTest";
		specification.Headless = false;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		return specification;
	}

}

TEST_SUITE("Core.WindowBackend")
{
	TEST_CASE("A window that cannot be opened ends the start through the fatal-error path with exit code 1")
	{
		ApplicationSpecification specification = MakeWindowedSpecification();
		specification.WindowFactory = [](const WindowSpecification&) -> Scope<Window> { return nullptr; };
		bool moduleStarted = false;
		specification.Modules.push_back([&moduleStarted]() -> Scope<EngineModule> { return CreateScope<StartRecordingModule>(moduleStarted); });

		const Tests::LogCapture log;
		Application application(specification);
		CHECK_FALSE(application.IsRunning());
		CHECK(application.GetExitCode() == 1);
		CHECK(application.GetWindow() == nullptr);
		CHECK_FALSE(moduleStarted);
		const auto start = std::chrono::steady_clock::now();
		application.Run();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
		CHECK(application.GetFrameCount() == 0);

		const std::vector<LogEntry> entries = log.GetEntries();
		const size_t reports = static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const LogEntry& entry)
		{
			return entry.Level == LogLevel::Critical && entry.Message.find("its window could not be opened") != std::string::npos;
		}));
		CHECK(reports == 1);
	}

#if defined(ST_PLATFORM_LINUX)
	TEST_CASE("Window::Create returns null without a display instead of aborting")
	{
		// The child's environment only (a helper sets it): no X11 display, no Wayland socket.
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=run-with-environment", "DISPLAY=", "WAYLAND_DISPLAY=", "XDG_RUNTIME_DIR=", "--",
			FileSystem::ToUTF8(Tests::GetTestExecutablePath()), "--strata-test-helper=open-window" };
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, std::chrono::seconds(60));
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		INFO(result.Output);
		INFO(result.ErrorOutput);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("No window") != std::string::npos);
		const std::string log = result.Output + result.ErrorOutput;
		CHECK(log.find("GLFW error") != std::string::npos);
		CHECK(log.find("could not be initialized") != std::string::npos);
	}
#endif
}
