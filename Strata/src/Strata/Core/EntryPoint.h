#pragma once

// Include exactly once, in the client's main translation unit, after defining Strata::CreateApplication.

#include "Strata/Core/Application.h"
#include "Strata/Core/Log.h"

int main(int argc, char** argv)
{
	Strata::CommandLine commandLine(argc, argv);

	Strata::Application* application = Strata::CreateApplication(commandLine);
	if (!application)
		return 1;

	application->Run();
	delete application;

	Strata::Log::Shutdown();
	return 0;
}
