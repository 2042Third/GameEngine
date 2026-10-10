#pragma once

#include <optional>
#include <string_view>

namespace Strata::Tests
{

	// Helper modes of the test executable (--strata-test-helper=<mode>) that register the engine's modules themselves, or
	// show what happens when nothing registers them; TestMain runs them before it registers the modules:
	//   custom-component <directory>    registers a component through ModuleRegistrationOptions::Extra, saves and reloads a
	//                                   scene with it there and tries registrations the registry must refuse; prints what
	//                                   it found and exits with 0 when everything behaved
	//   unregistered-registry <name>    uses a registry ("components", "loaders", "importers", "builtin-assets" or
	//                                   "systems") without registering the modules: prints the failed verify's message and
	//                                   exits with c_VerifyExitCode
	// Returns nothing for other modes.
	std::optional<int> RunModuleRegistrationHelper(std::string_view mode, int argc, char** argv);

	inline constexpr int c_VerifyExitCode = 3;

}
