#include "stpch.h"
#include "Strata/Core/Symbolizer.h"

#include "Strata/Core/FileSystem.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>
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

		// DbgHelp names the anonymous namespace of an inlined function by its tag ("?A0x1a2b3c4d::Helper") and that of other
		// functions as "`anonymous namespace'::Caller": the frames of one stack get the second form.
		std::string NameAnonymousNamespaces(std::string name)
		{
			constexpr std::string_view c_Tag = "?A0x";
			constexpr std::string_view c_Anonymous = "`anonymous namespace'";
			for (size_t position = name.find(c_Tag); position != std::string::npos; position = name.find(c_Tag, position))
			{
				size_t end = position + c_Tag.size();
				while (end < name.size() && std::isxdigit(static_cast<unsigned char>(name[end])))
					end++;
				if (end == position + c_Tag.size() || name.compare(end, 2, "::") != 0)
				{
					position = end;
					continue;
				}
				name.replace(position, end - position, c_Anonymous);
				position += c_Anonymous.size();
			}
			return name;
		}

		// A SYMBOL_INFOW with room for the longest name DbgHelp returns.
		struct SymbolBuffer
		{
			SymbolBuffer()
			{
				Symbol()->SizeOfStruct = sizeof(SYMBOL_INFOW);
				Symbol()->MaxNameLen = MAX_SYM_NAME;
			}

			SYMBOL_INFOW* Symbol() { return reinterpret_cast<SYMBOL_INFOW*>(Bytes); }

			alignas(SYMBOL_INFOW) uint8_t Bytes[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)] = {};
		};

		// The calls the compiler inlined at the frame's address. DbgHelp numbers the inline frames of an address from the
		// context SymQueryInlineTrace gives for it, the innermost first.
		void ResolveInlinedCalls(HANDLE session, Frame& frame)
		{
			const DWORD count = SymAddrIncludeInlineTrace(session, frame.Address);
			if (count == 0)
				return;
			DWORD context = 0;
			DWORD frameIndex = 0;
			if (!SymQueryInlineTrace(session, frame.Address, 0, frame.Address, frame.Address, &context, &frameIndex))
				return;
			for (DWORD index = 0; index < count; index++, context++)
			{
				InlinedCall call;
				SymbolBuffer symbol;
				DWORD64 displacement = 0;
				if (SymFromInlineContextW(session, frame.Address, context, &displacement, symbol.Symbol()))
					call.Function = NameAnonymousNamespaces(FromWide(symbol.Symbol()->Name));
				IMAGEHLP_LINEW64 line = {};
				line.SizeOfStruct = sizeof(line);
				DWORD lineDisplacement = 0;
				if (SymGetLineFromInlineContextW(session, frame.Address, context, 0, &lineDisplacement, &line))
				{
					call.File = FromWide(line.FileName);
					call.Line = line.LineNumber;
				}
				frame.Inlined.push_back(std::move(call));
			}
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

			SymbolBuffer symbol;
			DWORD64 displacement = 0;
			if (SymFromAddrW(session, frame.Address, &displacement, symbol.Symbol()))
				frame.Function = NameAnonymousNamespaces(FromWide(symbol.Symbol()->Name));

			IMAGEHLP_LINEW64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddrW64(session, frame.Address, &lineDisplacement, &line))
			{
				frame.File = FromWide(line.FileName);
				frame.Line = line.LineNumber;
			}
			frame.Inlined.clear();
			ResolveInlinedCalls(session, frame);
		}
		for (uint64_t base : loadedBases)
			SymUnloadModule64(session, base);
		SymCleanup(session);
	}

}
