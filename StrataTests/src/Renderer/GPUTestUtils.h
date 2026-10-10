#pragma once

#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/glm.hpp>

namespace Strata::Tests
{

	// Access to the headless graphics device and renderer shared by all GPU tests of a run. Like an application, the
	// test process creates its Vulkan device once: tearing instances down and up again for every test is not a usage
	// pattern of the engine, and some drivers and overlay layers handle that churn poorly. Validation is enabled (tests
	// check GetNewErrorCount() at the end) unless the environment variable STRATA_TEST_GPU_VALIDATION is "0", which the
	// perf tests' CTest sets (StrataTests.PerfGPU).
	class GPUContext
	{
	public:
		GPUContext();
		// Waits for the GPU and releases deferred resources, so each test starts from an idle device.
		~GPUContext();

		GPUContext(const GPUContext&) = delete;
		GPUContext& operator=(const GPUContext&) = delete;

		bool IsValid() const { return m_Device != nullptr; }
		GraphicsDevice& GetDevice() { return *m_Device; }
		nvrhi::IDevice* GetNvrhiDevice() { return m_Device->GetDevice(); }
		// Validation errors reported since this context was created.
		uint32_t GetNewErrorCount() const { return m_Device->GetErrorCount() - m_InitialErrorCount; }

		// Destroys the shared device (end of the test run).
		static void ShutdownShared();
	private:
		GraphicsDevice* m_Device = nullptr;
		uint32_t m_InitialErrorCount = 0;
	};

	inline glm::u8vec4 GetPixelRGBA8(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		const uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * image.BytesPerPixel;
		return glm::u8vec4(pixel[0], pixel[1], pixel[2], pixel[3]);
	}

}
