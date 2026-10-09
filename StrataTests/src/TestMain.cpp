#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Network/FakeEditorProcess.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "TestHelpers.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(ST_PLATFORM_LINUX)
	#include <sys/prctl.h>
#endif

// When launched with --strata-test-helper=<mode>, the test executable acts as a child process for the
// Process tests (and checks build products for CTest scripts) instead of running the test suites. This keeps
// those tests free of external programs.
static int RunHelperMode(std::string_view mode, int argc, char** argv)
{
	if (mode == "echo")
	{
		for (int index = 2; index < argc; index++)
			std::printf("%s%s", index > 2 ? " " : "", argv[index]);
		std::printf("\n");
		std::fflush(stdout);
		return 0;
	}
	if (mode == "stderr")
	{
		std::fprintf(stderr, "error-output\n");
		std::fflush(stderr);
		return 0;
	}
	if (mode == "exit-code")
		return argc > 2 ? std::atoi(argv[2]) : 0;
	if (mode == "sleep")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(argc > 2 ? std::atoi(argv[2]) : 10000));
		return 0;
	}
	if (mode == "cwd")
	{
		const std::u8string currentDirectory = std::filesystem::current_path().generic_u8string();
		std::printf("%s\n", reinterpret_cast<const char*>(currentDirectory.c_str()));
		std::fflush(stdout);
		return 0;
	}
	if (mode == "load-script-module")
	{
		// <module path> <class name>...: succeeds if the module loads and contains every class.
		if (argc < 3)
			return 2;
		Strata::ScriptEngine engine;
		std::string error;
		if (!engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		for (int index = 3; index < argc; index++)
		{
			if (!engine.FindClass(argv[index]))
			{
				std::fprintf(stderr, "The module has no script class '%s'\n", argv[index]);
				return 1;
			}
		}
		return 0;
	}
	if (mode == "check-image")
	{
		// <png>: succeeds if the image decodes and shows something: it is neither (nearly) black nor a single color.
		if (argc < 3)
			return 2;
		const std::optional<std::vector<uint8_t>> data = Strata::FileSystem::ReadBytes(Strata::FileSystem::FromUTF8(argv[2]));
		if (!data || data->empty())
		{
			std::fprintf(stderr, "Cannot read '%s'\n", argv[2]);
			return 1;
		}
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(data->data(), static_cast<int>(data->size()), &width, &height, &channels, 4);
		if (!pixels || width <= 0 || height <= 0)
		{
			std::fprintf(stderr, "'%s' is not a valid image\n", argv[2]);
			stbi_image_free(pixels);
			return 1;
		}
		int brightest = 0;
		int64_t differing = 0; // Pixels that differ from the top-left one
		const int64_t count = static_cast<int64_t>(width) * height;
		for (int64_t index = 0; index < count; index++)
		{
			const stbi_uc* pixel = pixels + index * 4;
			brightest = std::max({ brightest, static_cast<int>(pixel[0]), static_cast<int>(pixel[1]), static_cast<int>(pixel[2]) });
			for (int channel = 0; channel < 3; channel++)
			{
				if (std::abs(static_cast<int>(pixel[channel]) - static_cast<int>(pixels[channel])) > 24)
				{
					differing++;
					break;
				}
			}
		}
		stbi_image_free(pixels);
		std::printf("%dx%d pixels, brightest channel %d, %lld pixels differ from the corner\n", width, height, brightest, static_cast<long long>(differing));
		if (brightest < 32)
		{
			std::fprintf(stderr, "The image is black\n");
			return 1;
		}
		if (differing < count / 100)
		{
			std::fprintf(stderr, "The image is (nearly) a single color\n");
			return 1;
		}
		return 0;
	}

#if defined(ST_PLATFORM_LINUX)
	if (mode == "rename-when-file-exists" && argc > 2)
	{
		// Once the parent creates the file, takes a name containing ')' and a newline (which /proc/<pid>/stat shows
		// unescaped inside its parentheses), then runs until it is terminated.
		const std::filesystem::path trigger(argv[2]);
		std::error_code error;
		for (int attempt = 0; attempt < 1000 && !std::filesystem::exists(trigger, error); attempt++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// prctl reads its arguments as unsigned long, which is uintptr_t on Linux.
		if (prctl(PR_SET_NAME, reinterpret_cast<uintptr_t>("a)\nb"), 0UL, 0UL, 0UL) != 0)
			return 1;
		std::this_thread::sleep_for(std::chrono::seconds(30));
		return 0;
	}
#endif
	return 99;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);

	// Launched as an editor by the CLI launch tests (see Network/FakeEditorProcess.h).
	if (Strata::Tests::IsFakeEditorLaunch(argc, argv))
		return Strata::Tests::RunFakeEditor(argc, argv);

	Strata::LogSpecification logSpecification;
	logSpecification.Level = Strata::LogLevel::Warn;
	Strata::Log::Init(logSpecification);

	doctest::Context context(argc, argv);
	const int result = context.run();

	Strata::Tests::GPUContext::ShutdownShared();
	Strata::Log::Shutdown();
	Strata::Tests::CleanupTemporaryDirectories();
	return result;
}
