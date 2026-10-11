#include "stpch.h"
#include "Strata/Core/Symbolizer.h"

#include "Strata/Core/FileSystem.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace Strata::Symbolizer
{

	namespace
	{

		// DbgHelp is single-threaded, and its options are process-wide.
		std::mutex s_DbgHelpMutex;
		// The session handle of our DbgHelp calls: any unique value works when DbgHelp does not read the process itself
		// (modules are loaded from their files), and a value of our own keeps clear of other DbgHelp users (crash reporters)
		// that initialize it with the process handle.
		const char s_SessionTag = 0;

		HANDLE GetSessionHandle()
		{
			return const_cast<char*>(&s_SessionTag);
		}

		std::wstring ToWide(const std::filesystem::path& path)
		{
			return path.native();
		}

		std::string FromWide(const wchar_t* text)
		{
			return text ? FileSystem::ToUTF8(std::filesystem::path(text)) : std::string();
		}

	}

	Frame Describe(uint64_t address)
	{
		Frame frame;
		frame.Address = address;
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address),
			&module) || !module)
		{
			return frame;
		}
		std::vector<wchar_t> buffer(MAX_PATH);
		while (true)
		{
			const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return frame;
			if (length < buffer.size())
			{
				frame.ModulePath = std::filesystem::path(std::wstring(buffer.data(), length));
				break;
			}
			if (buffer.size() >= 32768)
				return frame;
			buffer.resize(buffer.size() * 2);
		}
		frame.ModuleBase = reinterpret_cast<uint64_t>(module);
		// The image's size, from its headers in memory (mapped while the module is loaded).
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
		if (dosHeader->e_magic == IMAGE_DOS_SIGNATURE)
		{
			const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
			if (ntHeaders->Signature == IMAGE_NT_SIGNATURE)
				frame.ModuleSize = ntHeaders->OptionalHeader.SizeOfImage;
		}
		return frame;
	}

	void Resolve(std::span<Frame> frames, std::span<const std::filesystem::path> searchDirectories)
	{
		std::lock_guard lock(s_DbgHelpMutex);
		std::wstring searchPath;
		for (const std::filesystem::path& directory : searchDirectories)
		{
			searchPath += searchPath.empty() ? L"" : L";";
			searchPath += ToWide(directory);
		}

		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_DEFERRED_LOADS);
		const HANDLE session = GetSessionHandle();
		if (!SymInitializeW(session, searchPath.empty() ? nullptr : searchPath.c_str(), FALSE))
			return;

		std::vector<uint64_t> loadedBases;
		for (Frame& frame : frames)
		{
			if (frame.ModulePath.empty() || frame.ModuleBase == 0)
				continue;
			if (std::find(loadedBases.begin(), loadedBases.end(), frame.ModuleBase) == loadedBases.end())
			{
				// The module as it was loaded: its file at its base and with its size (the file is read, the process is not; without
				// the size DbgHelp maps no address to the module). 0 with no error: loaded already.
				const std::wstring image = ToWide(frame.ModulePath);
				SetLastError(ERROR_SUCCESS);
				const DWORD64 loadedBase = SymLoadModuleExW(session, nullptr, image.c_str(), nullptr, frame.ModuleBase, static_cast<DWORD>(frame.ModuleSize), nullptr, 0);
				if (loadedBase == 0 && GetLastError() != ERROR_SUCCESS)
					continue;
				loadedBases.push_back(frame.ModuleBase);
			}

			alignas(SYMBOL_INFOW) uint8_t symbolBuffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)] = {};
			SYMBOL_INFOW* symbol = reinterpret_cast<SYMBOL_INFOW*>(symbolBuffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
			symbol->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement = 0;
			if (SymFromAddrW(session, frame.Address, &displacement, symbol))
				frame.Function = FromWide(symbol->Name);

			IMAGEHLP_LINEW64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddrW64(session, frame.Address, &lineDisplacement, &line))
			{
				frame.File = FromWide(line.FileName);
				frame.Line = line.LineNumber;
			}
		}
		for (uint64_t base : loadedBases)
			SymUnloadModule64(session, base);
		SymCleanup(session);
	}

}
