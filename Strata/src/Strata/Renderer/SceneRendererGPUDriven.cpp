#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>

// The GPU-driven path (SceneRendererSpecification::GPUDriven): the render scene's GPU copy (RenderSceneGPU) is culled on the
// GPU against the view and the shadow cascades, levels of detail are chosen there, and the draws are built there
// (Scene/Cull.comp counts the instances of each bucket and LOD record, CullBuild.comp gives each record a range of the draw
// list and an indirect draw, and Cull.comp then writes the instances into their ranges). The CPU records a fixed number of
// dispatches and one indirect draw per pass and bucket the frame can have, and draws transparent surfaces itself, sorted.
// The CPU's extraction still resolves the frame's assets (the mesh and material tables follow what it resolved) and fits
// the cascades to the casters.

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_CullGroupSize = 64;
		constexpr uint32_t c_CullModeCount = 0;
		constexpr uint32_t c_CullModeWrite = 1;
		// Shadow casters are drawn into a cascade only while they span at least this many of its texels (as the CPU path,
		// SceneRendererExtraction.cpp).
		constexpr float c_MinCasterTexels = 1.0f;
		constexpr uint32_t c_ArgumentWords = sizeof(nvrhi::DrawIndexedIndirectArguments) / sizeof(uint32_t);
		static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 5 * sizeof(uint32_t));

		// The workgroups of a dispatch over `items` items: in rows of at most 65,535 groups (the dispatch limit); the shaders
		// that may exceed it number their items across rows.
		void Dispatch(nvrhi::ICommandList* commandList, uint64_t items)
		{
			const uint64_t groups = (items + c_CullGroupSize - 1) / c_CullGroupSize;
			if (groups == 0)
				return;
			const uint32_t x = static_cast<uint32_t>(std::min<uint64_t>(groups, 65535));
			const uint32_t y = static_cast<uint32_t>((groups + x - 1) / x);
			commandList->dispatch(x, y, 1);
		}

		// A bucket's parts (RenderData::MakeViewBucket, MakeShadowBucket).
		struct BucketParts
		{
			bool Shadow = false;
			uint32_t Cascade = 0;
			bool Mirrored = false;
			bool DoubleSided = false;
			uint32_t Geometry = RenderData::c_CullGeometryStatic;
			bool AlphaMask = false;
		};

		BucketParts DecodeBucket(uint32_t bucket)
		{
			BucketParts parts;
			uint32_t local = bucket;
			if (bucket >= RenderData::c_CullViewBuckets)
			{
				parts.Shadow = true;
				parts.Cascade = (bucket - RenderData::c_CullViewBuckets) / RenderData::c_CullShadowBucketsPerCascade;
				local = (bucket - RenderData::c_CullViewBuckets) % RenderData::c_CullShadowBucketsPerCascade;
				parts.AlphaMask = (local & 16u) != 0;
			}
			parts.Mirrored = (local & 1u) != 0;
			parts.DoubleSided = (local & 2u) != 0;
			parts.Geometry = (local >> 2) & 3u;
			return parts;
		}

	}

	SceneRendererStats AddGPUCounts(const SceneRendererStats& stats, const SceneRendererGPUStats& gpu)
	{
		SceneRendererStats combined = stats;
		if (!stats.GPUDriven || !gpu.HasCounters)
			return combined;
		combined.Instances += gpu.Instances;
		combined.DrawCalls += gpu.Draws;
		combined.OpaqueDrawCalls = gpu.Draws;
		combined.ShadowDrawCalls = gpu.ShadowDraws;
		combined.ShadowCasters = gpu.ShadowInstances;
		combined.SubTexelCasters = gpu.SubTexelCasters;
		combined.SkinnedInstances += gpu.SkinnedInstances;
		combined.Triangles += gpu.Triangles;
		combined.VisibleProxies = gpu.VisibleProxies + gpu.SizeCulledProxies;
		combined.SizeCulledProxies = gpu.SizeCulledProxies;
		for (size_t lod = 0; lod < combined.InstancesPerLOD.size(); lod++)
			combined.InstancesPerLOD[lod] += gpu.InstancesPerLOD[lod];
		return combined;
	}

	bool SceneRenderer::IsGPUDriven() const
	{
		return m_Specification.GPUDriven == GPUDrivenMode::Auto && m_CullViewPipeline && m_CullShadowPipeline && m_CullBuildPipeline
			&& Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	SceneRenderer::GPUIndirectPath SceneRenderer::GetGPUIndirectPath() const
	{
		const GraphicsDeviceInfo& info = Renderer::GetGraphicsDevice().GetInfo();
		switch (m_Specification.IndirectMode)
		{
			case IndirectDrawMode::PerDraw:
				return GPUIndirectPath::PerDraw;
			case IndirectDrawMode::NoCount:
				return info.SupportsMultiDrawIndirect ? GPUIndirectPath::NoCount : GPUIndirectPath::PerDraw;
			case IndirectDrawMode::Auto:
				break;
		}
		if (info.SupportsDrawIndirectCount)
			return GPUIndirectPath::Count;
		return info.SupportsMultiDrawIndirect ? GPUIndirectPath::NoCount : GPUIndirectPath::PerDraw;
	}

	void SceneRenderer::CreateCullResources()
	{
		if (m_Specification.GPUDriven == GPUDrivenMode::Off || !Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance)
			return;
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Compute;
		layout.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(5),
			// The pass (counting or writing): a slot no constant buffer of the layout uses.
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(uint32_t))
		};
		m_CullBindingLayout = m_Device->createBindingLayout(layout);
		auto createPipeline = [&](const char* name) -> nvrhi::ComputePipelineHandle
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shaders.Get(name);
			desc.bindingLayouts = { m_CullBindingLayout };
			if (!desc.CS || !m_CullBindingLayout)
				return nullptr;
			return m_Device->createComputePipeline(desc);
		};
		m_CullViewPipeline = createPipeline("Scene/Cull.comp");
		m_CullShadowPipeline = createPipeline("Scene/CullShadow.comp");
		m_CullBuildPipeline = createPipeline("Scene/CullBuild.comp");

		bool recreated = false;
		const bool created = EnsureBufferCapacity(m_BucketTableBuffer, 2 * RenderData::c_CullBuckets, sizeof(uint32_t), "CullBuckets", recreated)
			&& EnsureBufferCapacity(m_CullBucketCountBuffer, RenderData::c_CullBuckets, sizeof(uint32_t), "CullBucketCounts", recreated, BufferKind::IndirectArgumentsUAV)
			&& EnsureBufferCapacity(m_CullCounterBuffer, sizeof(RenderData::CullCounters) / sizeof(uint32_t), sizeof(uint32_t), "CullCounters", recreated,
				BufferKind::StructuredUAV);
		m_CullConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::CullConstants))
			.setIsConstantBuffer(true)
			.setInitialState(nvrhi::ResourceStates::ConstantBuffer)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".CullConstants"));
		for (uint32_t index = 0; index < c_GPURecordCount; index++)
			m_CounterReadbacks[index].Readback = CreateScope<BufferReadback>(m_Device, sizeof(RenderData::CullCounters), m_Specification.DebugName + ".CullCounterReadback");
		const bool readbacks = std::all_of(m_CounterReadbacks.begin(), m_CounterReadbacks.end(), [](const CounterReadback& entry) { return entry.Readback->IsValid(); });
		if (!m_CullViewPipeline || !m_CullShadowPipeline || !m_CullBuildPipeline || !created || !m_CullConstantBuffer || !readbacks)
		{
			// The CPU path draws instead.
			ST_CORE_ERROR("{}: the GPU-driven path is unavailable (its pipelines or buffers could not be created); the CPU culls and draws",
				m_Specification.DebugName);
			m_CullViewPipeline = nullptr;
			m_CullShadowPipeline = nullptr;
			m_CullBuildPipeline = nullptr;
			return;
		}
	}

	uint32_t SceneRenderer::GetBucketSlots(uint32_t bucket) const
	{
		const BucketParts parts = DecodeBucket(bucket);
		const size_t arena = parts.Geometry == RenderData::c_CullGeometryStatic ? 0 : 1;
		return parts.Shadow ? m_CullShadowSlots[parts.Cascade][arena] : m_CullViewSlots[arena];
	}

	void SceneRenderer::PrepareGPUCulling(const RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum)
	{
		ST_PROFILE_FUNCTION();
		// The meshes the frame draws on the CPU path: those it resolved with their geometry in the pool. Their LOD records
		// bound the draws a bucket can have: those in view for the view's buckets, each cascade's casters' for its own.
		const uint32_t meshSlots = renderScene.GetMeshSlotCapacity();
		m_CullMeshTable.assign(meshSlots, 0u);
		m_CullMeshRecords.assign(meshSlots, 0u);
		m_CullViewSlots = {};
		m_CullShadowSlots = {};
		m_CullArenaRecords = {};
		for (const uint32_t slot : m_UsedMeshSlots)
		{
			const Mesh* mesh = m_FrameMeshes[slot];
			if (!mesh)
				continue;
			uint32_t records = 0;
			for (const Submesh& submesh : mesh->GetSubmeshes())
				records += static_cast<uint32_t>(submesh.LODs.size());
			const bool skinnedArena = m_FrameGeometry[slot].Arena == GeometryArena::Skinned;
			const GeometryHandle handle = mesh->GetGeometryHandle();
			ST_CORE_ASSERT(handle != c_InvalidGeometry && handle < RenderData::c_CullMeshSkinnedArena, "Geometry handle {} does not fit the mesh table", handle);
			m_CullMeshTable[slot] = handle | (skinnedArena ? RenderData::c_CullMeshSkinnedArena : 0u);
			m_CullMeshRecords[slot] = records;
			const size_t arena = skinnedArena ? 1 : 0;
			m_CullArenaRecords[arena] += records;
			if (IsMeshInView(slot))
				m_CullViewSlots[arena] += records;
		}
		if (++m_CullCasterStamp == 0)
		{
			for (std::vector<uint32_t>& stamps : m_CullCasterStamps)
				std::fill(stamps.begin(), stamps.end(), 0u);
			m_CullCasterStamp = 1;
		}
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			m_CullCasterStamps[cascade].resize(meshSlots, 0u);
		for (const ShadowCaster& caster : m_ShadowCasters)
		{
			const uint32_t slot = renderScene.GetMeshSlot(caster.Proxy);
			if (slot >= meshSlots || m_CullMeshRecords[slot] == 0)
				continue;
			const size_t arena = m_FrameGeometry[slot].Arena == GeometryArena::Skinned ? 1 : 0;
			for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			{
				if ((caster.CascadeMask & (1u << cascade)) == 0 || m_CullCasterStamps[cascade][slot] == m_CullCasterStamp)
					continue;
				m_CullCasterStamps[cascade][slot] = m_CullCasterStamp;
				m_CullShadowSlots[cascade][arena] += m_CullMeshRecords[slot];
			}
		}
		m_CullMaterialFlags.assign(m_MaterialFlags.begin(), m_MaterialFlags.end());
		bool doubleSided = false;
		bool alphaMask = false;
		for (const uint32_t slot : m_UsedMaterialSlots)
		{
			doubleSided = doubleSided || (m_MaterialFlags[slot] & RenderData::c_CullMaterialDoubleSided) != 0;
			alphaMask = alphaMask || (m_MaterialFlags[slot] & RenderData::c_CullMaterialAlphaMask) != 0;
		}

		// The buckets the frame can have get dense indices, the view's first.
		const bool mirrored = renderScene.GetStats().MirroredProxies > 0;
		const std::array<bool, 3> geometries = { m_CullArenaRecords[0] > 0, m_CullArenaRecords[1] > 0,
			m_CullArenaRecords[1] > 0 && renderScene.GetStats().PosedProxies > 0 };
		m_CullBuckets.clear();
		m_CullBucketTable.assign(RenderData::c_CullBuckets, RenderData::c_CullNoBucket);
		// A bucket of the frame's geometry with nothing to draw (no mesh of its arena in view, or casting into its cascade)
		// still gets its index: the GPU may draw there what the CPU's bound did not foresee (counted as overflow).
		const auto addBuckets = [&](bool shadow, uint32_t cascade)
		{
			for (uint32_t geometry = 0; geometry < geometries.size(); geometry++)
			{
				if (!geometries[geometry])
					continue;
				for (uint32_t masked = 0; masked < (shadow && alphaMask ? 2u : 1u); masked++)
				{
					for (uint32_t sided = 0; sided < (doubleSided ? 2u : 1u); sided++)
					{
						for (uint32_t mirror = 0; mirror < (mirrored ? 2u : 1u); mirror++)
						{
							const uint32_t bucket = shadow ? RenderData::MakeShadowBucket(cascade, mirror != 0, sided != 0, geometry, masked != 0)
								: RenderData::MakeViewBucket(mirror != 0, sided != 0, geometry);
							m_CullBucketTable[bucket] = static_cast<uint32_t>(m_CullBuckets.size());
							m_CullBuckets.push_back(bucket);
						}
					}
				}
			}
		};
		addBuckets(false, 0);
		const uint32_t viewBuckets = static_cast<uint32_t>(m_CullBuckets.size());
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			addBuckets(true, cascade);

		RenderData::CullConstants& constants = m_CullConstants;
		constants = RenderData::CullConstants {};
		for (size_t plane = 0; plane < 6; plane++)
			constants.Planes[plane] = frustum.GetPlanes()[plane];
		constants.CameraPosition = glm::vec4(camera.Position, camera.Orthographic ? 1.0f : 0.0f);
		// As LODSelection::GetScreenFraction computes them.
		const float viewExtent = camera.Orthographic ? std::max(camera.VerticalFOV, 1e-4f) : std::tan(std::max(camera.VerticalFOV, 1e-3f) * 0.5f);
		constants.LODParameters = glm::vec4(m_Specification.LODThreshold, viewExtent, camera.Near, std::max(m_Specification.MinProjectedPixels, 0.0f));
		constants.ViewParameters = glm::vec4(static_cast<float>(m_ViewportSize.y), 0.0f, 0.0f, 0.0f);
		constants.LightView = m_LightView;
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
		{
			const CascadeBox& box = m_Cascades[cascade];
			constants.CascadeBoxes[cascade] = glm::vec4(box.Min, box.Max);
			constants.CascadeParameters[cascade] = glm::vec4(box.FarZ, c_MinCasterTexels * box.WorldPerTexel, 0.0f, 0.0f);
		}
		constants.ProxyCount = renderScene.GetProxyCapacity();
		constants.CascadeCount = m_CascadeCount;
		constants.LODCapacity = std::max(Renderer::GetGeometryPool()->GetLODCapacity(), 1u);
		constants.MeshSlotCount = static_cast<uint32_t>(m_CullMeshTable.size());
		constants.MaterialSlotCount = static_cast<uint32_t>(m_CullMaterialFlags.size());
		constants.DrawListCapacity = static_cast<uint32_t>(std::min<uint64_t>(renderScene.GetInstances().size() * (1ull + m_CascadeCount), 0xFFFFFFF0ull));
		constants.ArgumentsPerBucket = std::max({ m_CullArenaRecords[0], m_CullArenaRecords[1], 1u });
		constants.BucketCount = static_cast<uint32_t>(m_CullBuckets.size());
		constants.ViewBucketCount = viewBuckets;
		constants.DrawsBounded = GetGPUIndirectPath() != GPUIndirectPath::Count ? 1u : 0u;
		// The bucket table's second part: each dense bucket's draw slots.
		for (const uint32_t bucket : m_CullBuckets)
			m_CullBucketTable.push_back(GetBucketSlots(bucket));
	}

	bool SceneRenderer::EnsureCullBuffers(bool& outRecreated)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const uint64_t keys = static_cast<uint64_t>(std::max(constants.BucketCount, 1u)) * constants.LODCapacity;
		const uint64_t arguments = static_cast<uint64_t>(std::max(constants.BucketCount, 1u)) * constants.ArgumentsPerBucket * c_ArgumentWords;
		return EnsureBufferCapacity(m_MeshTableBuffer, m_CullMeshTable.size(), sizeof(uint32_t), "CullMeshTable", outRecreated)
			&& EnsureBufferCapacity(m_MaterialFlagBuffer, m_CullMaterialFlags.size(), sizeof(uint32_t), "CullMaterialFlags", outRecreated)
			&& EnsureBufferCapacity(m_CullCountBuffer, keys, sizeof(uint32_t), "CullCounts", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_CullOffsetBuffer, keys, sizeof(uint32_t), "CullOffsets", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_CullArgumentBuffer, arguments, sizeof(uint32_t), "CullArguments", outRecreated, BufferKind::IndirectArgumentsUAV)
			&& EnsureBufferCapacity(m_GPUDrawListBuffer, constants.DrawListCapacity, sizeof(uint32_t), "GPUDrawList", outRecreated, BufferKind::StructuredUAV);
	}

	bool SceneRenderer::CreateCullBindingSet()
	{
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_CullConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_SceneGPU->GetProxyBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_SceneGPU->GetInstanceBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_MeshTableBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_MaterialFlagBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(4, pool.GetEntryBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(5, pool.GetSubmeshBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(6, pool.GetLODBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_BucketTableBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CullCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_CullOffsetBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_CullArgumentBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_CullBucketCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_CullCounterBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_GPUDrawListBuffer),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(uint32_t))
		};
		m_CullBindingSet = m_Device->createBindingSet(desc, m_CullBindingLayout);
		m_CullBoundSceneVersion = m_SceneGPU->GetBufferVersion();
		m_CullBoundTableVersion = pool.GetTableVersion();
		return m_CullBindingSet != nullptr;
	}

	bool SceneRenderer::UploadCullTables(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		GeometryPool& pool = *Renderer::GetGeometryPool();
		if (!pool.RecordTableUploads(commandList))
			return false; // Logged by the pool
		// Tables replaced (the pool's, the scene copy's or the renderer's): the binding set follows.
		if (!m_CullBindingSet || m_CullBoundSceneVersion != m_SceneGPU->GetBufferVersion() || m_CullBoundTableVersion != pool.GetTableVersion())
		{
			if (!CreateCullBindingSet())
			{
				m_Errors.Report("failed to create the culling binding set");
				return false;
			}
		}
		const bool full = !m_CullTablesUploaded;
		m_Stats.InstanceBytesUploaded += UploadChangedRange(commandList, m_MeshTableBuffer, m_CullMeshTable, m_CullMeshTableUploaded, full);
		m_Stats.InstanceBytesUploaded += UploadChangedRange(commandList, m_MaterialFlagBuffer, m_CullMaterialFlags, m_CullMaterialFlagsUploaded, full);
		if (full || m_CullBucketTable != m_CullBucketTableUploaded)
		{
			commandList->writeBuffer(m_BucketTableBuffer, m_CullBucketTable.data(), m_CullBucketTable.size() * sizeof(uint32_t));
			m_CullBucketTableUploaded = m_CullBucketTable;
		}
		commandList->writeBuffer(m_CullConstantBuffer, &m_CullConstants, sizeof(m_CullConstants));
		m_CullTablesUploaded = true;
		return true;
	}

	void SceneRenderer::RunGPUCulling(nvrhi::ICommandList* commandList, bool shadows)
	{
		ST_PROFILE_FUNCTION();
		const RenderData::CullConstants& constants = m_CullConstants;
		commandList->clearBufferUInt(m_CullCountBuffer, 0);
		commandList->clearBufferUInt(m_CullBucketCountBuffer, 0);
		commandList->clearBufferUInt(m_CullCounterBuffer, 0);
		// Draw slots no record takes draw nothing.
		commandList->clearBufferUInt(m_CullArgumentBuffer, 0);

		nvrhi::ComputeState state;
		state.bindings = { m_CullBindingSet };
		const auto cullPasses = [&](uint32_t mode)
		{
			state.pipeline = m_CullViewPipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&mode, sizeof(mode));
			Dispatch(commandList, constants.ProxyCount);
			if (!shadows)
				return;
			state.pipeline = m_CullShadowPipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&mode, sizeof(mode));
			Dispatch(commandList, constants.ProxyCount);
		};
		// The passes go through one binding set, and NVRHI places barriers only when binding sets change: each pass must see
		// the previous one's writes.
		const auto barrier = [&]()
		{
			for (nvrhi::IBuffer* buffer : { m_CullCountBuffer.Get(), m_CullOffsetBuffer.Get(), m_CullArgumentBuffer.Get(), m_CullBucketCountBuffer.Get(),
				m_CullCounterBuffer.Get(), m_GPUDrawListBuffer.Get() })
				commandList->setBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
			commandList->commitBarriers();
		};

		cullPasses(c_CullModeCount);
		barrier();
		state.pipeline = m_CullBuildPipeline;
		commandList->setComputeState(state);
		const uint32_t mode = c_CullModeCount;
		commandList->setPushConstants(&mode, sizeof(mode));
		Dispatch(commandList, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		barrier();
		cullPasses(c_CullModeWrite);
		barrier();

		CounterReadback& readback = m_CounterReadbacks[m_CounterReadbackIndex];
		readback.Readback->Record(commandList, m_CullCounterBuffer, 0, sizeof(RenderData::CullCounters));
		readback.Render = m_RenderCount + 1;
	}

	void SceneRenderer::SubmitCullReadback()
	{
		m_CounterReadbacks[m_CounterReadbackIndex].Readback->Submit();
		m_CounterReadbackIndex = (m_CounterReadbackIndex + 1) % c_GPURecordCount;
	}

	void SceneRenderer::DrawGPUBucket(nvrhi::ICommandList* commandList, uint32_t dense, uint32_t slots)
	{
		const uint32_t perBucket = m_CullConstants.ArgumentsPerBucket;
		slots = std::min(slots, perBucket);
		const uint32_t offset = dense * perBucket * static_cast<uint32_t>(sizeof(nvrhi::DrawIndexedIndirectArguments));
		switch (GetGPUIndirectPath())
		{
			case GPUIndirectPath::Count:
				// The GPU's count is exact: every slot of the bucket's storage may be drawn, whatever the CPU foresaw.
				commandList->drawIndexedIndirectCount(offset, dense * static_cast<uint32_t>(sizeof(uint32_t)), perBucket);
				m_Stats.DrawCommands++;
				break;
			case GPUIndirectPath::NoCount:
				if (slots == 0)
					break;
				commandList->drawIndexedIndirect(offset, slots);
				m_Stats.DrawCommands++;
				break;
			case GPUIndirectPath::PerDraw:
				for (uint32_t slot = 0; slot < slots; slot++)
					commandList->drawIndexedIndirect(offset + slot * static_cast<uint32_t>(sizeof(nvrhi::DrawIndexedIndirectArguments)), 1);
				m_Stats.DrawCommands += slots;
				break;
		}
	}

	void SceneRenderer::DrawGPUView(nvrhi::ICommandList* commandList, Pass pass)
	{
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		const bool count = GetGPUIndirectPath() == GPUIndirectPath::Count;
		nvrhi::GraphicsState state;
		state.framebuffer = pass == Pass::Prepass ? m_PrepassFramebuffer.Get() : m_ForwardFramebuffer.Get();
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_GPUSceneBindingSet, Renderer::GetBindlessTextures().GetTable() };
		state.indexBuffer = nvrhi::IndexBufferBinding { pool.GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
		state.indirectParams = m_CullArgumentBuffer;
		state.indirectCountBuffer = count ? m_CullBucketCountBuffer.Get() : nullptr;
		for (uint32_t dense = 0; dense < m_CullConstants.ViewBucketCount; dense++)
		{
			const uint32_t bucket = m_CullBuckets[dense];
			const BucketParts parts = DecodeBucket(bucket);
			const uint32_t slots = GetBucketSlots(bucket);
			if (slots == 0 && !count)
				continue;
			const PipelineKey key { pass, parts.DoubleSided, parts.Mirrored, parts.Geometry == RenderData::c_CullGeometrySkinned };
			const GeometryArena arena = parts.Geometry == RenderData::c_CullGeometryStatic ? GeometryArena::Static : GeometryArena::Skinned;
			state.pipeline = GetScenePipeline(key);
			state.vertexBuffers = {
				nvrhi::VertexBufferBinding { pool.GetPositionBuffer(arena), 0, 0 },
				nvrhi::VertexBufferBinding { pool.GetAttributeBuffer(arena), 1, 0 }
			};
			if (key.Skinned)
				state.vertexBuffers.push_back(nvrhi::VertexBufferBinding { pool.GetSkinBuffer(), 2, 0 });
			commandList->setGraphicsState(state);
			DrawGPUBucket(commandList, dense, slots);
		}
	}

	void SceneRenderer::DrawGPUShadows(nvrhi::ICommandList* commandList)
	{
		commandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::AllSubresources, true, 0.0f, false, 0);
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		const bool count = GetGPUIndirectPath() == GPUIndirectPath::Count;
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);
		nvrhi::GraphicsState state;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(mapSize, mapSize));
		state.bindings = { m_GPUShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
		state.indexBuffer = nvrhi::IndexBufferBinding { pool.GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
		state.indirectParams = m_CullArgumentBuffer;
		state.indirectCountBuffer = count ? m_CullBucketCountBuffer.Get() : nullptr;
		for (uint32_t dense = m_CullConstants.ViewBucketCount; dense < m_CullConstants.BucketCount; dense++)
		{
			const uint32_t bucket = m_CullBuckets[dense];
			const BucketParts parts = DecodeBucket(bucket);
			const uint32_t slots = GetBucketSlots(bucket);
			if (slots == 0 && !count)
				continue;
			const PipelineKey key { parts.AlphaMask ? Pass::ShadowMasked : Pass::Shadow, parts.DoubleSided, parts.Mirrored,
				parts.Geometry == RenderData::c_CullGeometrySkinned };
			const GeometryArena arena = parts.Geometry == RenderData::c_CullGeometryStatic ? GeometryArena::Static : GeometryArena::Skinned;
			state.pipeline = GetScenePipeline(key);
			state.framebuffer = m_ShadowFramebuffers[parts.Cascade];
			state.vertexBuffers = {
				nvrhi::VertexBufferBinding { pool.GetPositionBuffer(arena), 0, 0 },
				nvrhi::VertexBufferBinding { pool.GetAttributeBuffer(arena), 1, 0 }
			};
			if (key.Skinned)
				state.vertexBuffers.push_back(nvrhi::VertexBufferBinding { pool.GetSkinBuffer(), 2, 0 });
			commandList->setGraphicsState(state);
			// The vertex shader learns its cascade's projection from a push constant (after the state).
			const uint32_t cascade = parts.Cascade;
			commandList->setPushConstants(&cascade, sizeof(cascade));
			DrawGPUBucket(commandList, dense, slots);
		}
	}

	void SceneRenderer::ResolveCullCounters(bool wait)
	{
		// The newest readback whose copy completed; with `wait`, the newest one submitted.
		CounterReadback* newest = nullptr;
		for (CounterReadback& entry : m_CounterReadbacks)
		{
			if (!entry.Readback || !entry.Readback->IsPending() || entry.Render <= m_GPUStats.CountersRender)
				continue;
			if (wait)
				entry.Readback->Wait();
			if (entry.Readback->IsReady() && (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		if (!newest)
			return;
		RenderData::CullCounters counters;
		if (!newest->Readback->Read(&counters, sizeof(counters)))
			return;
		SceneRendererGPUStats& stats = m_GPUStats;
		stats.HasCounters = true;
		stats.CountersRender = newest->Render;
		stats.Instances = counters.ViewInstances;
		stats.ShadowInstances = counters.ShadowInstances;
		stats.Draws = counters.ViewDraws;
		stats.ShadowDraws = counters.ShadowDraws;
		stats.VisibleProxies = counters.VisibleProxies;
		stats.SizeCulledProxies = counters.SizeCulled;
		stats.SubTexelCasters = counters.SubTexelCasters;
		stats.SkinnedInstances = counters.SkinnedInstances;
		stats.Triangles = (static_cast<uint64_t>(counters.TrianglesHigh) << 32) | counters.TrianglesLow;
		stats.DrawListEntries = counters.DrawListUsed;
		stats.Overflow = counters.Overflow;
		for (size_t lod = 0; lod < stats.InstancesPerLOD.size(); lod++)
			stats.InstancesPerLOD[lod] = counters.InstancesPerLOD[lod];
	}

}
