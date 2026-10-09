#include "Renderer/GPUTestUtils.h"

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
			specification.EnableValidation = true;
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
