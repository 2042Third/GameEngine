#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace Strata::Tests
{

	// What a built module (a script module, a shared library) holds for a debugger, read from its bytes without one: a
	// CodeView record naming its program database ("CodeView <pdb path>", PE files of MSVC), DWARF sections (".debug_info",
	// ELF files of GCC and Clang; a __DWARF segment in Mach-O) or the debug map that points lldb at the objects' DWARF (N_OSO
	// stabs, Mach-O files of Clang linked without dsymutil; a universal file's first slice). Empty when it holds none, with
	// the reason in `outReason` (also for truncated or unknown files: the reader never reads outside the bytes).
	std::string FindModuleDebugInformation(std::span<const uint8_t> bytes, std::string* outReason = nullptr);

}
