#pragma once

#include <optional>
#include <string_view>

namespace Strata::Tests
{

	// Helper modes of the test executable (--strata-test-helper=<mode>) that register the engine's modules themselves, or
	// show what happens when nothing registers them; TestMain runs them before it registers the modules:
	//   custom-component <directory>    registers the asset pipeline and a component through ModuleRegistrationOptions, saves
	//                                   and reloads a scene with the component there and tries registrations the registry
	//                                   must refuse; prints what it found and exits with 0 when everything behaved
	//   unregistered-registry <name>    uses a registry ("components", "loaders", "importers", "builtin-assets",
	//                                   "systems" or "mesh-geometry") without registering the modules: prints the failed
	//                                   verify's message and exits with c_VerifyExitCode
	//   no-asset-pipeline               registers the modules without options, like a shipped game: prints the loaders and
	//                                   systems it finds, then uses the importer registry, which fails a verify (the
	//                                   message is printed, the exit code is c_VerifyExitCode)
	//   register-without <modules> <scene file>
	//                                   registers the modules but <modules> (comma-separated, ModuleRegistrationOptions::
	//                                   Exclude): prints the registered modules and the systems' update order, the
	//                                   components of <scene file> that stayed unknown and how many saved back unchanged,
	//                                   then plays a scene whose script calls physics (the API test module's
	//                                   PhysicsWithoutSystem) and prints how it went and how often the absent module was
	//                                   reported; exits with 0 unless something could not run
	// Returns nothing for other modes.
	std::optional<int> RunModuleRegistrationHelper(std::string_view mode, int argc, char** argv);

	inline constexpr int c_VerifyExitCode = 3;

}
