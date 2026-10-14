#pragma once

#include "Strata/Core/Application.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/Platform.h"
#include "Strata/Engine/BuiltinModules.h"

#include <functional>
#include <optional>

namespace Strata
{

	// What a program's CreateApplication hands the entry point (Strata/Engine/EntryPoint.h): the options of the composition
	// root, which main runs before anything else of the engine (the asset pipeline of tools, a game's components), and the
	// function that makes the application once the modules are registered.
	struct ApplicationLaunch
	{
		Engine::ModuleRegistrationOptions Registration;
		std::function<Scope<Application>()> Create;
	};

	// Implemented by the program (editor, runtime) next to CreateApplication: what kind of run the command line asks for
	// (ProgramRun), decided before anything prints. Console runs print into the terminal that started them; Interactive
	// ones leave it alone.
	ProgramRun GetProgramRun(const CommandLine& commandLine);
	// Implemented by the program: reads its command line and opens its log. nullopt when there is nothing to run: the
	// process exits with Application::GetStartupExitCode() (after --help, or a command line it cannot run).
	std::optional<ApplicationLaunch> CreateApplication(const CommandLine& commandLine);

}
