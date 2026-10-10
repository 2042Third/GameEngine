#include "Renderer/GPUTestUtils.h"

#include "Strata/Core/Platform.h"

#include <optional>
#include <string>

namespace Strata::Tests
{

	namespace
	{

		struct SharedGPU
		{
			Scope<GraphicsDevice> Device;
			bool Initialized = false;
		};

		SharedGPU& GetSharedGPU()
		{
			static SharedGPU s_Shared;
			return s_Shared;
		}

	}

	GPUContext::GPUContext()
	{
		SharedGPU& shared = GetSharedGPU();
		if (!shared.Initialized)
		{
			shared.Initialized = true;
			GraphicsDeviceSpecification specification;
			specification.ApplicationName = "StrataTests";
			specification.Headless = true;
			// Validation layers multiply the CPU cost of every GPU call, so the perf tests measure without them: their
			// CTest (StrataTests.PerfGPU) sets STRATA_TEST_GPU_VALIDATION=0. Everything else validates.
			const std::optional<std::string> validation = Platform::GetEnvVar("STRATA_TEST_GPU_VALIDATION");
			specification.EnableValidation = !validation || *validation != "0";
			shared.Device = GraphicsDevice::Create(specification);
			if (shared.Device && !Renderer::Init(*shared.Device))
				shared.Device.reset();
		}

		m_Device = shared.Device.get();
		if (m_Device)
			m_InitialErrorCount = m_Device->GetErrorCount();
	}

	GPUContext::~GPUContext()
	{
		if (!m_Device)
			return;
		m_Device->WaitForIdle();
		m_Device->GetDevice()->runGarbageCollection();
		Renderer::GetDeferredReleases().Collect();
	}

	bool GPUContext::ExecuteAndWait(nvrhi::ICommandList* commandList)
	{
		nvrhi::IDevice* device = GetNvrhiDevice();
		device->executeCommandList(commandList);
		if (!device->waitForIdle())
			return false;
		device->runGarbageCollection();
		return true;
	}

	void GPUContext::ShutdownShared()
	{
		SharedGPU& shared = GetSharedGPU();
		if (shared.Device)
		{
			Renderer::Shutdown();
			shared.Device.reset();
		}
	}

}
