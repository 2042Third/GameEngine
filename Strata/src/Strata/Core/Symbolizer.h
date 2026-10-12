#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Names the code at addresses of this process, for crash reports (script faults): the module (executable or shared
// library) holding an address, its function and, where the module's debug information has them, the source file and line.
//   Windows: the module from the loader, then DbgHelp reads the module's PDB (where the module names it, or found in the
//            search directories): function, file and line, and the functions the compiler inlined at the address.
//   POSIX:   dladdr: the module and the nearest dynamic symbol (demangled), no file or line (no DWARF reader).
namespace Strata::Symbolizer
{

	// A function the compiler inlined at an address: the code that ran there, as its source says.
	struct InlinedCall
	{
		std::string Function; // Empty when unknown
		std::string File;     // Empty when unknown
		uint32_t Line = 0;
	};

	struct Frame
	{
		uint64_t Address = 0;
		std::filesystem::path ModulePath; // Empty when no loaded module holds the address
		uint64_t ModuleBase = 0;
		uint64_t ModuleSize = 0;          // The loaded image's size, where the loader tells (Windows)
		std::string Function;             // Empty when unknown
		std::string File;                 // Empty when unknown
		uint32_t Line = 0;
		// The calls inlined at the address, innermost first (the code that ran first); Function, File and Line are then the
		// function they were inlined into, at the call of the outermost one. Empty when nothing was inlined there or the
		// debug information does not tell (POSIX).
		std::vector<InlinedCall> Inlined;
	};

	// What the loader knows of an address while its module is loaded: the module, and on POSIX also the function. Cheap
	// and needs no debug information; call it while the module is still loaded (e.g. right after a fault). Any thread.
	Frame Describe(uint64_t address);

	// Fills in the functions, files and lines of frames (and the calls inlined at them) from their modules' debug
	// information (frames without a module are left as they are). A module is read from its file at ModulePath as if loaded at ModuleBase, so it may be unloaded by
	// now; its debug information is also looked for in `searchDirectories`. Blocking (reads debug information files):
	// call it from a job. Calls are serialized (DbgHelp is single-threaded); any thread.
	void Resolve(std::span<Frame> frames, std::span<const std::filesystem::path> searchDirectories);

}
