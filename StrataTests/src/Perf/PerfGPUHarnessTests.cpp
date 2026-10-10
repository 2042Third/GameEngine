#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Renderer/GPUTestUtils.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

using namespace Strata;

TEST_SUITE("PerfGPU.Harness")
{
	TEST_CASE("GPU perf tests measure on a device without validation")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Validation layers multiply the CPU cost of every GPU call, so numbers measured under them mean nothing. The
		// CTest StrataTests.PerfGPU creates the process's shared device without them (STRATA_TEST_GPU_VALIDATION=0); a
		// run of these suites in a process whose device validates fails here instead of measuring.
		REQUIRE_MESSAGE(!gpu.GetDevice().GetInfo().ValidationEnabled,
			"the shared GPU device validates: run the PerfGPU suites with STRATA_TEST_GPU_VALIDATION=0 (ctest -L perf does)");

		// A round trip of an empty command list: what every blocking GPU step of a perf test costs at least.
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::CommandListHandle commandList = device->createCommandList();
		REQUIRE(commandList);
		constexpr uint32_t c_Iterations = 50;
		const Tests::Perf::Measurement measurement = Tests::Perf::Measure("PerfGPU.Harness.EmptySubmit", 5, c_Iterations, [&]
		{
			commandList->open();
			commandList->close();
			device->executeCommandList(commandList);
			device->waitForIdle();
		});
		CHECK(measurement.Iterations == c_Iterations);
		CHECK(measurement.MinMs > 0.0);
		CHECK(measurement.MinMs <= measurement.MedianMs);
		CHECK(measurement.MedianMs <= measurement.MaxMs);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
