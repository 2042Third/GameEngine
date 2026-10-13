#pragma once

// Include exactly once, in the client's main translation unit, after defining Strata::CreateApplication.

#include "Strata/Core/Application.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"

// On Windows the applications are GUI-subsystem programs (no console window opens with them) that keep this main (the
// linker's /ENTRY:mainCRTStartup, StrataEditor/CMakeLists.txt and StrataRuntime/CMakeLists.txt).
int main(int argc, char** argv)
{
	// Before anything prints or the log opens its console sink: output reaches pipes and terminals as from a console program.
	Strata::Platform::ConnectStandardStreams();
	Strata::CommandLine commandLine(argc, argv);

	Strata::Application* application = Strata::CreateApplication(commandLine);
	if (!application)
		return Strata::Application::GetStartupExitCode();

	application->Run();
	const int exitCode = application->GetExitCode();
	delete application;

	Strata::Log::Shutdown();
	return exitCode;
}
