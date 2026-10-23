#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Strata::Tests
{

	// What the local shadows' memory helper measured (--strata-test-helper=local-shadows-memory <result file>): a renderer of
	// 1920x1080 drawing Render100kCubes' scene with temporal anti-aliasing under 16 shadowed point lights (their atlas, the
	// motion and temporal histories) for c_LocalShadowsMemoryFrames frames, as a game does. The peak is the process's: a
	// maximum by nature (what it needs at once).
	struct LocalShadowsMemoryResult
	{
		bool Completed = false;
		std::string Error;
		bool ValidationEnabled = false;
		double PeakPrivateMB = 0.0;
		uint32_t LocalShadowLights = 0;    // Of the last frame
		bool TemporalAntiAliasing = false; // The last frame was drawn with it
		uint32_t NewErrors = 0;            // The device's errors while it ran
	};

	constexpr uint32_t c_LocalShadowsMemoryFrames = 30;

	int RunLocalShadowsMemoryProcess(int argc, char** argv);
	std::optional<LocalShadowsMemoryResult> ReadLocalShadowsMemoryResult(const std::filesystem::path& file, std::string& error);

}
