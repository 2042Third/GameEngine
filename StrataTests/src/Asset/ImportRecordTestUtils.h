#pragma once

#include <optional>
#include <string_view>

namespace Strata::Tests
{

	// The test executable as a process that a crash interrupts while it writes import records (ImportRecordStoreTests.cpp):
	//   --strata-test-helper=append-import-records <cache directory> <first index>
	// opens the store in the directory and, from the index on, endlessly writes a cache file "<handle>.bin" (renamed into
	// place) and then sets the record that names it, as an import publishes its result, until it is killed. Prints "ready"
	// once it has written its first record. Returns nullopt for other modes; 2 for wrong arguments.
	std::optional<int> RunImportRecordHelper(std::string_view mode, int argc, char** argv);

}
