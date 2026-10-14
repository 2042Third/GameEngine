#pragma once

#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Strata::Tests
{

	// Creates a fresh, empty directory under the system temp directory. All directories created this way
	// are removed when the test run finishes.
	std::filesystem::path CreateTemporaryDirectory(const std::string& name);
	void CleanupTemporaryDirectories();
	// Copies a sample project of the repository (Samples/<name>, without editor state in .strata) into a new temporary
	// directory and returns the copy's directory, or an empty path when the copy failed (outError says why). Tests never
	// open the samples in place.
	std::filesystem::path CopySampleProject(const std::string& name, std::string* outError = nullptr);

	// Describes a JSON file a process failed to load, for the failure message: its path and size, and whether it is valid
	// JSON or else the parser's error with the last 64 bytes it read, up to the one it stopped at (non-printable bytes as
	// \xNN). Tells a damaged file from a reader that failed on a good one. Reads without building a document, so huge
	// files cost little memory.
	std::string DescribeJsonFile(const std::filesystem::path& file);

	// Appends one RGBA texel to a pixel buffer. Tests use it instead of insert(end(), { r, g, b, a }), for which GCC 14 at
	// -O3 reports a false -Wstringop-overflow on byte vectors.
	inline void AppendPixel(std::vector<uint8_t>& pixels, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
	{
		pixels.push_back(red);
		pixels.push_back(green);
		pixels.push_back(blue);
		pixels.push_back(alpha);
	}

	// Polls condition until it returns true or the timeout elapses; returns the final condition value.
	inline bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	// Number of log messages containing `text` logged after `afterSequence` (see LogBuffer::GetLatestSequence).
	inline size_t CountLogMessages(uint64_t afterSequence, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(afterSequence))
		{
			if (entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	// Logs at `level` while it exists, then at the test process's level again (Warn, see TestMain.cpp): for tests that
	// check messages below it.
	class ScopedLogLevel
	{
	public:
		explicit ScopedLogLevel(LogLevel level) { Log::SetLevel(level); }
		~ScopedLogLevel() { Log::SetLevel(LogLevel::Warn); }

		ScopedLogLevel(const ScopedLogLevel&) = delete;
		ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
	};

	// Builds a 16-bit PCM WAV file containing a sine wave (the same signal on every channel), optionally preceded by
	// silence.
	std::vector<uint8_t> CreateSineWav(float durationSeconds, uint32_t sampleRate = 48000, uint16_t channels = 1, float frequency = 440.0f,
		float silentSeconds = 0.0f, float amplitude = 0.5f);

	// Encodes 8-bit RGBA pixels (tightly packed rows) as a PNG file.
	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
	// The same for 1 (gray), 2 (gray and alpha), 3 (RGB) or 4 (RGBA) channels; empty when the pixels do not fit.
	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, uint32_t channels, const std::vector<uint8_t>& pixels);
	// A PNG filled with one color.
	std::vector<uint8_t> CreateSolidPNG(uint32_t width, uint32_t height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255);
	// The pixel sizes (width; 256 for the largest) of the images in an .ico file, from its directory. Nullopt (outError
	// says why) when it cannot be read or is no icon file.
	std::optional<std::vector<uint32_t>> ReadIconFileSizes(const std::filesystem::path& iconFile, std::string* outError = nullptr);

#if defined(ST_PLATFORM_WINDOWS)
	// Creates a junction (a mount-point reparse point, which needs no special privilege) at link that redirects to target.
	// False when it cannot be created (e.g. on file systems without reparse points, such as exFAT).
	bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target);
	// The pixel sizes (width; 256 for the largest) of the images in an executable's first icon group (RT_GROUP_ICON),
	// read with the executable loaded as a data file. Nullopt (outError says why) when it cannot be loaded or has no
	// icon group.
	std::optional<std::vector<uint32_t>> ReadExecutableIconSizes(const std::filesystem::path& executable, std::string* outError = nullptr);

	// What a program printed into a console and the code it ended with.
	struct ConsoleRun
	{
		std::string Text; // The console's lines up to its cursor, without trailing blanks
		int ExitCode = -1;
		bool Interrupted = false; // Ctrl+C was pressed (interruptWhenShown appeared)
	};

	// Runs a program the way a terminal does: this process's console (a hidden one in a helper that Process started) is its
	// output, nothing redirected, and the program is waited for. With `interruptWhenShown`, Ctrl+C is pressed once the
	// console shows that text (this process ignores it; the program gets it as from a terminal). Nullopt (outError says why)
	// when this process has no console, the program cannot start or it runs longer than 45 seconds (it is then ended).
	std::optional<ConsoleRun> RunInConsole(const std::filesystem::path& program, const std::vector<std::string>& arguments, std::string* outError = nullptr,
		std::string_view interruptWhenShown = {});
#endif

	// Pages straight from the system (VirtualAlloc, mmap), not from the process's allocator, which may hand out memory it
	// already holds (freed earlier and kept for reuse), so touching them always grows the process's memory. Released when
	// the object is destroyed.
	class FreshPages
	{
	public:
		explicit FreshPages(size_t size);
		~FreshPages();

		FreshPages(const FreshPages&) = delete;
		FreshPages& operator=(const FreshPages&) = delete;

		uint8_t* GetData() const { return m_Data; } // Null when the system had no pages to give
		size_t GetSize() const { return m_Size; }
	private:
		uint8_t* m_Data = nullptr;
		size_t m_Size = 0;
	};

	inline std::filesystem::path GetTestExecutablePath()
	{
		return Platform::GetExecutablePath();
	}

	// Sets an environment variable for the lifetime of the object. An empty value counts as unset for every
	// variable Strata reads, so restoring an absent variable sets it to empty.
	class ScopedEnvironmentVariable
	{
	public:
		ScopedEnvironmentVariable(std::string name, const std::string& value)
			: m_Name(std::move(name)), m_Previous(Platform::GetEnvVar(m_Name))
		{
			Platform::SetEnvVar(m_Name, value);
		}

		~ScopedEnvironmentVariable()
		{
			Platform::SetEnvVar(m_Name, m_Previous.value_or(std::string()));
		}

		ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
		ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
	private:
		std::string m_Name;
		std::optional<std::string> m_Previous;
	};

}
