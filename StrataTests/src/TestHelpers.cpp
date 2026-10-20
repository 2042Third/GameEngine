#include "TestHelpers.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/UUID.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
	#include <Windows.h>
	#include <winioctl.h>

	#include <cstring>
	#include <string>
#else
	#include <sys/mman.h>
#endif

namespace Strata::Tests
{

	static std::mutex s_TemporaryDirectoriesMutex;
	static std::vector<std::filesystem::path> s_TemporaryDirectories;

	namespace
	{

		// Reads a JSON text without building a document and keeps where the parser stopped when the text is invalid.
		struct JsonSyntaxCheck final : nlohmann::json_sax<nlohmann::json>
		{
			size_t ErrorPosition = 0; // The bytes the parser read, the one it stopped at included
			std::string Error;

			bool null() override { return true; }
			bool boolean(bool) override { return true; }
			bool number_integer(number_integer_t) override { return true; }
			bool number_unsigned(number_unsigned_t) override { return true; }
			bool number_float(number_float_t, const string_t&) override { return true; }
			bool string(string_t&) override { return true; }
			bool binary(binary_t&) override { return true; }
			bool start_object(std::size_t) override { return true; }
			bool key(string_t&) override { return true; }
			bool end_object() override { return true; }
			bool start_array(std::size_t) override { return true; }
			bool end_array() override { return true; }

			bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& exception) override
			{
				ErrorPosition = position;
				Error = exception.what();
				return false;
			}
		};

	}

	std::filesystem::path CreateTemporaryDirectory(const std::string& name)
	{
		std::filesystem::path directory = std::filesystem::temp_directory_path() / "StrataTests" / (name + "_" + UUID().ToString());
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		std::filesystem::create_directories(directory, error);

		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		s_TemporaryDirectories.push_back(directory);
		return directory;
	}

	std::filesystem::path CopySampleProject(const std::string& name, std::string* outError)
	{
		const std::filesystem::path copy = CreateTemporaryDirectory("Sample" + name) / FileSystem::FromUTF8(name);
		if (!FileSystem::CopyDirectory(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Samples" / FileSystem::FromUTF8(name), copy, outError))
			return {};
		// Only the sample's committed files: no editor state that opening it elsewhere may have left next to it.
		const std::filesystem::path editorState = copy / ".strata";
		if (FileSystem::Exists(editorState) && !FileSystem::Remove(editorState))
		{
			if (outError)
				*outError = "the copy's .strata directory could not be removed";
			return {};
		}
		return copy;
	}

	bool CopySamples(const std::filesystem::path& destination, const std::vector<std::string>& names, std::string* outError)
	{
		const std::filesystem::path samples = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Samples";
		const std::optional<std::string> text = FileSystem::ReadText(samples / "Samples.json");
		std::optional<nlohmann::json> index = text ? JsonUtils::Parse(*text) : std::nullopt;
		if (!index || !index->contains("Samples") || !(*index)["Samples"].is_array())
		{
			if (outError)
				*outError = "the repository's Samples/Samples.json could not be read";
			return false;
		}
		nlohmann::json kept = nlohmann::json::array();
		for (const nlohmann::json& entry : (*index)["Samples"])
		{
			if (std::find(names.begin(), names.end(), entry.value("Directory", std::string())) != names.end())
				kept.push_back(entry);
		}
		if (kept.size() != names.size())
		{
			if (outError)
				*outError = "Samples/Samples.json does not list every sample asked for";
			return false;
		}
		(*index)["Samples"] = std::move(kept);
		if (!FileSystem::CreateDirectories(destination) || !FileSystem::WriteText(destination / "Samples.json", index->dump(1, '\t') + "\n"))
		{
			if (outError)
				*outError = "the samples index could not be written";
			return false;
		}
		for (const std::string& name : names)
		{
			if (!FileSystem::CopyDirectory(samples / FileSystem::FromUTF8(name), destination / FileSystem::FromUTF8(name), outError))
				return false;
		}
		return true;
	}

	LogCapture::LogCapture()
		: m_Start(Log::GetBuffer().GetLatestSequence())
	{
	}

	std::vector<LogEntry> LogCapture::GetEntries() const
	{
		std::vector<LogEntry> entries = Log::GetBuffer().GetEntries(m_Start);
		REQUIRE_MESSAGE((entries.empty() || entries.front().Sequence == m_Start + 1), "The log buffer dropped entries; raise its capacity");
		return entries;
	}

	std::string DescribeJsonFile(const std::filesystem::path& file)
	{
		const std::string name = FileSystem::ToUTF8(file);
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(file);
		if (!bytes)
			return fmt::format("'{}' cannot be read", name);

		const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		JsonSyntaxCheck check;
		if (nlohmann::json::sax_parse(text.begin(), text.end(), &check))
			return fmt::format("'{}' ({} bytes) is valid JSON", name, text.size());

		constexpr size_t c_ContextBytes = 64;
		const size_t end = std::min(check.ErrorPosition, text.size());
		const size_t start = end - std::min(end, c_ContextBytes);
		std::string context;
		for (const char character : text.substr(start, end - start))
		{
			const auto byte = static_cast<unsigned char>(character);
			context += byte >= 0x20 && byte < 0x7F ? std::string(1, character) : fmt::format("\\x{:02X}", byte);
		}
		return fmt::format("'{}' ({} bytes) is not valid JSON: {}; bytes {} to {}: \"{}\"", name, text.size(), check.Error, start, end, context);
	}

	std::vector<uint8_t> CreateSineWav(float durationSeconds, uint32_t sampleRate, uint16_t channels, float frequency, float silentSeconds, float amplitude)
	{
		const uint32_t silentFrames = static_cast<uint32_t>(std::lround(silentSeconds * static_cast<float>(sampleRate)));
		const uint32_t frameCount = static_cast<uint32_t>(std::lround(durationSeconds * static_cast<float>(sampleRate)));
		const uint32_t blockAlign = channels * 2u;
		const uint32_t dataSize = frameCount * blockAlign;

		std::vector<uint8_t> wav;
		wav.reserve(44 + dataSize);
		const auto writeTag = [&](const char* tag) { wav.insert(wav.end(), tag, tag + 4); };
		const auto writeU16 = [&](uint16_t value)
		{
			wav.push_back(static_cast<uint8_t>(value & 0xFF));
			wav.push_back(static_cast<uint8_t>(value >> 8));
		};
		const auto writeU32 = [&](uint32_t value)
		{
			for (uint32_t byte = 0; byte < 4; byte++)
				wav.push_back(static_cast<uint8_t>(value >> (byte * 8)));
		};

		writeTag("RIFF");
		writeU32(36 + dataSize);
		writeTag("WAVE");

		writeTag("fmt ");
		writeU32(16);
		writeU16(1); // PCM
		writeU16(channels);
		writeU32(sampleRate);
		writeU32(sampleRate * blockAlign);
		writeU16(static_cast<uint16_t>(blockAlign));
		writeU16(16); // Bits per sample

		writeTag("data");
		writeU32(dataSize);
		for (uint32_t frame = 0; frame < frameCount; frame++)
		{
			const double phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / static_cast<double>(sampleRate);
			const int16_t sample = frame < silentFrames ? int16_t(0) : static_cast<int16_t>(std::lround(amplitude * std::sin(phase) * 32767.0));
			for (uint16_t channel = 0; channel < channels; channel++)
				writeU16(static_cast<uint16_t>(sample));
		}
		return wav;
	}

	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
	{
		return EncodePNG(width, height, 4, rgba);
	}

	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, uint32_t channels, const std::vector<uint8_t>& pixels)
	{
		std::vector<uint8_t> encoded;
		auto write = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
		};
		if (channels < 1 || channels > 4 || pixels.size() < static_cast<size_t>(width) * height * channels
			|| !stbi_write_png_to_func(write, &encoded, static_cast<int>(width), static_cast<int>(height), static_cast<int>(channels), pixels.data(),
				static_cast<int>(width * channels)))
			return {};
		return encoded;
	}

	std::vector<uint8_t> CreateSolidPNG(uint32_t width, uint32_t height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
	{
		std::vector<uint8_t> pixels;
		pixels.reserve(static_cast<size_t>(width) * height * 4);
		for (uint32_t index = 0; index < width * height; index++)
			AppendPixel(pixels, red, green, blue, alpha);
		return EncodePNG(width, height, pixels);
	}

	std::optional<std::vector<uint32_t>> ReadIconFileSizes(const std::filesystem::path& iconFile, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<std::vector<uint32_t>>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		const std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(iconFile);
		if (!data)
			return fail("it cannot be read");
		// ICONDIR: reserved, type (1: icon), count (little-endian WORDs), then 16-byte entries starting with the width.
		const auto read16 = [&data](size_t offset) { return static_cast<uint32_t>((*data)[offset] | ((*data)[offset + 1] << 8)); };
		if (data->size() < 6 || read16(0) != 0 || read16(2) != 1)
			return fail("it is not an icon file");
		const uint32_t count = read16(4);
		if (data->size() < 6 + static_cast<size_t>(count) * 16)
			return fail("its directory is cut off");
		std::vector<uint32_t> sizes;
		for (uint32_t index = 0; index < count; index++)
		{
			const uint8_t width = (*data)[6 + static_cast<size_t>(index) * 16];
			sizes.push_back(width == 0 ? 256u : width);
		}
		return sizes;
	}

#if defined(ST_PLATFORM_WINDOWS)
	bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target)
	{
		if (!CreateDirectoryW(link.c_str(), nullptr))
			return false;
		HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return false;

		// The mount point layout of REPARSE_DATA_BUFFER (declared in the driver kit, not the user-mode headers):
		// tag, data length, reserved, then the substitute and print names' offsets and lengths, then both names.
		const std::wstring printName = std::filesystem::absolute(target).wstring();
		const std::wstring substituteName = L"\\??\\" + printName;
		const size_t substituteBytes = (substituteName.size() + 1) * sizeof(wchar_t);
		const size_t printBytes = (printName.size() + 1) * sizeof(wchar_t);
		const size_t dataSize = 4 * sizeof(USHORT) + substituteBytes + printBytes;
		std::vector<uint8_t> buffer(8 + dataSize);
		auto write16 = [&buffer](size_t offset, size_t value)
		{
			const USHORT narrowed = static_cast<USHORT>(value);
			std::memcpy(buffer.data() + offset, &narrowed, sizeof(narrowed));
		};
		const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
		std::memcpy(buffer.data(), &tag, sizeof(tag));
		write16(4, dataSize);
		write16(8, 0);
		write16(10, substituteBytes - sizeof(wchar_t));
		write16(12, substituteBytes);
		write16(14, printBytes - sizeof(wchar_t));
		std::memcpy(buffer.data() + 16, substituteName.c_str(), substituteBytes);
		std::memcpy(buffer.data() + 16 + substituteBytes, printName.c_str(), printBytes);

		DWORD returned = 0;
		const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
		CloseHandle(handle);
		return created != FALSE;
	}

	std::optional<std::vector<uint32_t>> ReadExecutableIconSizes(const std::filesystem::path& executable, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<std::vector<uint32_t>>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		// As a data file: its resources are readable, nothing of it runs.
		HMODULE module = LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		if (!module)
			return fail("it cannot be loaded as a data file (error " + std::to_string(GetLastError()) + ")");

		// The first icon group, which Explorer shows for the executable.
		struct FirstGroup
		{
			bool Found = false;
			std::wstring Name;
			WORD Id = 0;
		} group;
		EnumResourceNamesW(module, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR parameter) -> BOOL
		{
			FirstGroup& first = *reinterpret_cast<FirstGroup*>(parameter);
			first.Found = true;
			if (IS_INTRESOURCE(name))
				first.Id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
			else
				first.Name = name;
			return FALSE; // Stop after the first one
		}, reinterpret_cast<LONG_PTR>(&group));
		std::optional<std::vector<uint32_t>> sizes;
		if (group.Found)
		{
			HRSRC resource = FindResourceW(module, group.Name.empty() ? MAKEINTRESOURCEW(group.Id) : group.Name.c_str(), MAKEINTRESOURCEW(14));
			HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
			const uint8_t* data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
			const DWORD size = resource ? SizeofResource(module, resource) : 0;
			// GRPICONDIR: reserved, type (1: icon), count (WORD each), then 14-byte entries starting with the width.
			if (data && size >= 6)
			{
				WORD count = 0;
				std::memcpy(&count, data + 4, sizeof(count));
				if (size >= 6 + static_cast<DWORD>(count) * 14)
				{
					sizes.emplace();
					for (WORD index = 0; index < count; index++)
					{
						const uint8_t width = data[6 + index * 14];
						sizes->push_back(width == 0 ? 256u : width);
					}
				}
			}
		}
		FreeLibrary(module);
		if (!group.Found)
			return fail("it has no icon group (RT_GROUP_ICON)");
		if (!sizes)
			return fail("its icon group cannot be read");
		return sizes;
	}

	namespace
	{

		// The console's lines up to its cursor, without trailing blanks; nothing when it cannot be read.
		std::optional<std::string> ReadConsoleText(HANDLE console)
		{
			CONSOLE_SCREEN_BUFFER_INFO info = {};
			if (!GetConsoleScreenBufferInfo(console, &info))
				return std::nullopt;
			std::string text;
			std::wstring line(static_cast<size_t>(info.dwSize.X), L' ');
			for (SHORT row = 0; row <= info.dwCursorPosition.Y; row++)
			{
				DWORD read = 0;
				if (!ReadConsoleOutputCharacterW(console, line.data(), static_cast<DWORD>(line.size()), COORD { 0, row }, &read))
					break;
				std::wstring shown = line.substr(0, read);
				const size_t end = shown.find_last_not_of(L' ');
				shown.resize(end == std::wstring::npos ? 0 : end + 1);
				text += FileSystem::ToUTF8(std::filesystem::path(shown)) + "\n";
			}
			return text;
		}

		// Shorter than the minute tests give the helper that runs this, so the program is ended here, never left behind.
		constexpr std::chrono::seconds c_ConsoleRunTimeout(45);

		// The Ctrl+C this process sends to its console reaches it too: it is handled here (not ignored with a null handler,
		// which the programs it starts would inherit).
		BOOL WINAPI KeepRunningOnInterrupt(DWORD type)
		{
			return type == CTRL_C_EVENT ? TRUE : FALSE;
		}

	}

	std::optional<ConsoleRun> RunInConsole(const std::filesystem::path& program, const std::vector<std::string>& arguments, std::string* outError,
		std::string_view interruptWhenShown)
	{
		auto fail = [outError](std::string message) -> std::optional<ConsoleRun>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		const HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
			nullptr);
		if (console == INVALID_HANDLE_VALUE)
			return fail("this process has no console");

		// Programs started from a terminal process Ctrl+C: whoever started this helper may have turned it off, which its
		// children would inherit. This process keeps running when it presses Ctrl+C itself.
		if (!interruptWhenShown.empty())
		{
			SetConsoleCtrlHandler(nullptr, FALSE);
			SetConsoleCtrlHandler(KeepRunningOnInterrupt, TRUE);
		}

		// A terminal's standard streams are its console; the program gets them as a terminal passes them (inherited, not
		// redirected). This process's own streams are given back once it has started.
		const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
		const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
		SetStdHandle(STD_OUTPUT_HANDLE, console);
		SetStdHandle(STD_ERROR_HANDLE, console);
		ProcessSpecification specification;
		specification.Executable = program;
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::Inherit;
		Process process;
		const bool started = process.Start(specification);
		SetStdHandle(STD_OUTPUT_HANDLE, output);
		SetStdHandle(STD_ERROR_HANDLE, error);
		if (!started)
		{
			CloseHandle(console);
			return fail("cannot start it: " + process.GetLastError());
		}
		ConsoleRun run;
		const auto deadline = std::chrono::steady_clock::now() + c_ConsoleRunTimeout;
		std::optional<int> exitCode;
		while (!exitCode && std::chrono::steady_clock::now() < deadline)
		{
			exitCode = process.Wait(std::chrono::milliseconds(50));
			if (exitCode || interruptWhenShown.empty() || run.Interrupted)
				continue;
			const std::optional<std::string> shown = ReadConsoleText(console);
			if (shown && shown->find(interruptWhenShown) != std::string::npos)
			{
				run.Interrupted = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != FALSE;
				if (!run.Interrupted)
				{
					process.Terminate();
					CloseHandle(console);
					return fail("cannot press Ctrl+C in the console");
				}
			}
		}
		if (!exitCode)
		{
			process.Terminate();
			CloseHandle(console);
			return fail(run.Interrupted ? "it did not end within 45 seconds, also after Ctrl+C" : "it did not end within 45 seconds");
		}

		run.ExitCode = *exitCode;
		const std::optional<std::string> text = ReadConsoleText(console);
		CloseHandle(console);
		if (!text)
			return fail("cannot read the console");
		run.Text = *text;
		return run;
	}
#endif

	void CleanupTemporaryDirectories()
	{
		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		for (const std::filesystem::path& directory : s_TemporaryDirectories)
		{
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
		s_TemporaryDirectories.clear();
	}

	FreshPages::FreshPages(size_t size)
	{
		if (size == 0)
			return;
#if defined(ST_PLATFORM_WINDOWS)
		void* pages = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!pages)
			return;
#else
		void* pages = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pages == MAP_FAILED)
			return;
#endif
		m_Data = static_cast<uint8_t*>(pages);
		m_Size = size;
	}

	FreshPages::~FreshPages()
	{
		if (!m_Data)
			return;
#if defined(ST_PLATFORM_WINDOWS)
		VirtualFree(m_Data, 0, MEM_RELEASE);
#else
		munmap(m_Data, m_Size);
#endif
	}


	bool IsFullSweep()
	{
		return Platform::GetEnvVar("STRATA_TEST_SWEEP").value_or(std::string()) == "full";
	}

	std::vector<size_t> SelectSweepOffsets(size_t size, const std::vector<SweepRange>& mustInclude, size_t boundary, size_t randomCount)
	{
		std::vector<size_t> offsets;
		if (size == 0)
			return offsets;
		if (IsFullSweep())
		{
			offsets.resize(size);
			for (size_t offset = 0; offset < size; offset++)
				offsets[offset] = offset;
			return offsets;
		}

		for (const SweepRange& range : mustInclude)
		{
			for (size_t offset = range.Begin; offset < std::min(range.End, size); offset++)
				offsets.push_back(offset);
		}
		constexpr size_t c_BoundaryReach = 8;
		if (boundary > 0)
		{
			for (size_t edge = 0; edge <= size; edge += boundary)
			{
				const size_t first = edge > c_BoundaryReach ? edge - c_BoundaryReach : 0;
				for (size_t offset = first; offset <= edge + c_BoundaryReach && offset < size; offset++)
					offsets.push_back(offset);
			}
		}
		// A fixed seed: every run tests the same offsets, so a failure repeats.
		std::mt19937_64 random(0x5EEDF00DCAFEull ^ size);
		std::uniform_int_distribution<size_t> pick(0, size - 1);
		for (size_t index = 0; index < randomCount; index++)
			offsets.push_back(pick(random));

		std::sort(offsets.begin(), offsets.end());
		offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
		return offsets;
	}

	uint32_t SelectSweepIterations(uint32_t sample, uint32_t full)
	{
		return IsFullSweep() ? full : std::min(sample, full);
	}
}
