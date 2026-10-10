#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/StressProject.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "TestHelpers.h"

#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Frames without work, until releases deferred for frames in flight have happened.
	void SettleDevice(GPUContext& gpu)
	{
		for (uint32_t frame = 0; frame < gpu.GetDevice().GetMaxFramesInFlight() + 2; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			gpu.GetDevice().EndFrame();
		}
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

}

TEST_SUITE("GPU.Assets.Streaming")
{
	// The sweep of the streaming perf test (PerfGPU.Streaming) on a small stress project, under the validation layers.
	TEST_CASE("A camera sweep over a stress world stays within its texture budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ScopedJobSystem jobs(4, 2);

		StressProjectSpec spec;
		spec.Textures2k = 6;
		spec.Textures4k = 1;
		spec.Materials = 7;
		spec.Entities = 400;
		spec.GridResolution = 33;
		spec.SmallTextureSize = 128;
		spec.LargeTextureSize = 256;
		const std::filesystem::path directory = CreateTemporaryDirectory("StreamingSweep");
		std::string error;
		const std::optional<StressProject> project = WriteStressProject(directory / "Stress", spec, error);
		REQUIRE_MESSAGE(project, error);
		REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);

		StreamingSweepSettings settings;
		settings.Pack = directory / "Stress.stpak";
		settings.Scene = project->WorldScene;
		settings.Spec = spec;
		settings.Path.Stops = 4;
		settings.Path.MoveFrames = 12;
		settings.Path.HoldFrames = 60;
		settings.Width = 320;
		settings.Height = 180;
		// Three small textures: the stops need about two districts, and the moves cross more.
		settings.TextureBudget = 3 * GetStressTextureBytes(spec.SmallTextureSize);

		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t slotsBefore = bindless.GetAllocatedCount();
		const StreamingSweepResult result = RunStreamingSweep(gpu.GetDevice(), settings);
		REQUIRE_MESSAGE(result.Completed, result.Error);
		CHECK(result.Frames == settings.Path.GetFrameCount());
		CHECK(result.FailedAssets == 0);
		CHECK(result.MaxResidentTextureBytes <= settings.TextureBudget + project->LargestTextureBytes);
		CHECK(result.Evictions > 0);
		REQUIRE(result.FramesToSettle.size() == settings.Path.Stops);
		for (size_t stop = 0; stop < result.FramesToSettle.size(); stop++)
		{
			CAPTURE(stop);
			CHECK(result.FramesToSettle[stop] < settings.Path.HoldFrames);
		}
		CHECK(result.NewErrors == 0);

		// The sweep's manager is gone: its textures are released once the frames that might use them are done.
		SettleDevice(gpu);
		CHECK(bindless.GetAllocatedCount() == slotsBefore);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
