#include "Renderer/GPUTestUtils.h"

#include "Strata/Core/Platform.h"

#include <cstring>
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

	std::vector<uint8_t> ReadBufferRange(GPUContext& gpu, nvrhi::IBuffer* buffer, uint64_t offset, uint64_t size)
	{
		if (!buffer || size == 0)
			return {};
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.cpuAccess = nvrhi::CpuAccessMode::Read;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "ReadbackBuffer";
		nvrhi::BufferHandle readback = device->createBuffer(desc);
		nvrhi::CommandListHandle commandList = device->createCommandList();
		if (!readback || !commandList)
			return {};
		commandList->open();
		commandList->copyBuffer(readback, 0, buffer, offset, size);
		commandList->close();
		if (!gpu.ExecuteAndWait(commandList))
			return {};
		const void* mapped = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
		if (!mapped)
			return {};
		std::vector<uint8_t> bytes(size);
		std::memcpy(bytes.data(), mapped, size);
		device->unmapBuffer(readback);
		return bytes;
	}

	std::optional<MeshGeometryBytes> ReadMeshGeometry(GPUContext& gpu, const Mesh& mesh)
	{
		const GeometryAllocation* geometry = mesh.GetGeometry();
		const Ref<GeometryPool> pool = Renderer::GetGeometryPool();
		if (!geometry || !pool)
			return std::nullopt;
		const uint64_t vertices = mesh.GetPositions().size();
		const uint64_t firstVertex = geometry->FirstVertex;
		MeshGeometryBytes bytes;
		bytes.Positions = ReadBufferRange(gpu, pool->GetPositionBuffer(geometry->Arena), firstVertex * GeometryPool::c_PositionStride, vertices * GeometryPool::c_PositionStride);
		bytes.Attributes = ReadBufferRange(gpu, pool->GetAttributeBuffer(geometry->Arena), firstVertex * GeometryPool::c_AttributeStride,
			vertices * GeometryPool::c_AttributeStride);
		if (geometry->Arena == GeometryArena::Skinned)
			bytes.Skin = ReadBufferRange(gpu, pool->GetSkinBuffer(), firstVertex * GeometryPool::c_SkinStride, vertices * GeometryPool::c_SkinStride);
		bytes.Indices = ReadBufferRange(gpu, pool->GetIndexBuffer(), uint64_t(geometry->FirstIndex) * GeometryPool::c_IndexStride,
			mesh.GetIndices().size() * GeometryPool::c_IndexStride);
		return bytes;
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
