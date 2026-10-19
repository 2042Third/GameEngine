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
// The CPU's extraction still requests the frame's assets and fits the cascades to the casters; the GPU draws the meshes and
// materials the render scene's GPU copy holds (RenderSceneGPU's tables, which follow what is loaded).

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_CullGroupSize = 64;
		// Cull.comp's and CullBuild.comp's modes: bit 0 writes the draw list (else counts), bit 1 runs the view's second phase.
		constexpr uint32_t c_CullModeCount = 0;
		constexpr uint32_t c_CullModeWrite = 1;
		constexpr uint32_t c_CullModeSecondPhase = 2;

		// Scene/HZBBuild.comp's push constants.
		struct DepthPyramidParameters
		{
			glm::uvec2 SourceSize = glm::uvec2(0);
			glm::uvec2 DestinationSize = glm::uvec2(0);
			uint32_t UseMinimumSampler = 0;
		};
		// Shadow casters are drawn into a cascade only while they span at least this many of its texels (as the CPU path,
		// SceneRendererExtraction.cpp).
		constexpr float c_MinCasterTexels = 1.0f;
		constexpr uint32_t c_ArgumentWords = sizeof(nvrhi::DrawIndexedIndirectArguments) / sizeof(uint32_t);
		// Feedback frames bound a bucket's draw slots (paths without a GPU-written count) by the records of the meshes the
		// feedback saw, two or three frames old: this much more, so meshes the view turns to meanwhile are drawn too.
		constexpr uint32_t c_FeedbackSlotHeadroom = 16;
		// Feedback older than this many of the renderer's frames says too little of the view (a renderer that did not render
		// for a while): the CPU's marks run instead.
		constexpr uint64_t c_MaxFeedbackAge = 8;
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
		combined.Lights += gpu.Lights;
		combined.CulledLights += gpu.CulledLights;
		// The CPU counts every indexed proxy as visible or culled (on feedback frames only those it looked at as visible).
		const uint32_t indexed = stats.VisibleProxies + stats.CulledInstances;
		combined.VisibleProxies = gpu.VisibleProxies + gpu.SizeCulledProxies;
		combined.CulledInstances = indexed > combined.VisibleProxies ? indexed - combined.VisibleProxies : 0u;
		combined.SizeCulledProxies = gpu.SizeCulledProxies;
		combined.OccludedProxies = gpu.OccludedProxies;
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
			nvrhi::BindingLayoutItem::Texture_SRV(8),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(5),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(7),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(8),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(9),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(10),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(11),
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
		m_CullFeedbackPipeline = createPipeline("Scene/CullFeedback.comp");

		// The depth pyramid of occlusion culling: a level per dispatch, a minimum filter where the device has one.
		nvrhi::BindingLayoutDesc pyramidLayout;
		pyramidLayout.visibility = nvrhi::ShaderType::Compute;
		pyramidLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DepthPyramidParameters))
		};
		m_DepthPyramidLayout = m_Device->createBindingLayout(pyramidLayout);
		nvrhi::ComputePipelineDesc pyramidDesc;
		pyramidDesc.CS = shaders.Get("Scene/HZBBuild.comp");
		pyramidDesc.bindingLayouts = { m_DepthPyramidLayout };
		m_DepthPyramidPipeline = pyramidDesc.CS && m_DepthPyramidLayout ? m_Device->createComputePipeline(pyramidDesc) : nullptr;
		nvrhi::SamplerDesc pyramidSampler;
		pyramidSampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		if (Renderer::GetGraphicsDevice().GetInfo().SupportsSamplerMinMax)
			pyramidSampler.setAllFilters(true).setReductionType(nvrhi::SamplerReductionType::Minimum);
		else
			pyramidSampler.setAllFilters(false);
		m_DepthPyramidSampler = m_Device->createSampler(pyramidSampler);

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
		if (!m_CullViewPipeline || !m_CullShadowPipeline || !m_CullBuildPipeline || !m_CullFeedbackPipeline || !m_DepthPyramidPipeline || !m_DepthPyramidSampler
			|| !created || !m_CullConstantBuffer || !readbacks)
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

	void SceneRenderer::CreateLightCullResources()
	{
		if (!m_CullViewPipeline)
			return; // No GPU-driven path: the CPU collects the lights
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		// The frame's constants at b0 and the passes' parameters at b1, a slot no constant buffer of the layouts uses.
		nvrhi::BindingLayoutDesc cullLayout;
		cullLayout.visibility = nvrhi::ShaderType::Compute;
		cullLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::LightCullParameters)),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(18),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6)
		};
		m_LightCullLayout = m_Device->createBindingLayout(cullLayout);
		nvrhi::BindingLayoutDesc compactLayout;
		compactLayout.visibility = nvrhi::ShaderType::Compute;
		compactLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::LightCullParameters)),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(18),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(19),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(20),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(5),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6)
		};
		m_LightCompactLayout = m_Device->createBindingLayout(compactLayout);
		auto createPipeline = [&](const char* name, nvrhi::IBindingLayout* layout) -> nvrhi::ComputePipelineHandle
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shaders.Get(name);
			desc.bindingLayouts = { layout };
			if (!desc.CS || !layout)
				return nullptr;
			return m_Device->createComputePipeline(desc);
		};
		m_LightCullPipeline = createPipeline("Scene/LightCull.comp", m_LightCullLayout);
		m_LightCompactPipeline = createPipeline("Scene/LightCompact.comp", m_LightCompactLayout);
		if (!m_LightCullPipeline || !m_LightCompactPipeline)
		{
			// The CPU collects the lights instead (capped at MaxLights).
			ST_CORE_ERROR("{}: GPU light culling is unavailable (its pipelines could not be created); the CPU collects the lights", m_Specification.DebugName);
			m_LightCullPipeline = nullptr;
			m_LightCompactPipeline = nullptr;
		}
	}

	bool SceneRenderer::EnsureLightCullBindingSets()
	{
		const uint32_t groups = (m_GPULightRecords + RenderData::c_LightGroupSize - 1) / RenderData::c_LightGroupSize;
		bool recreated = false;
		if (!EnsureBufferCapacity(m_LightVisibleBuffer, m_GPULightRecords, sizeof(uint32_t), "LightVisible", recreated, BufferKind::StructuredUAV)
			|| !EnsureBufferCapacity(m_LightGroupCountBuffer, groups, sizeof(uint32_t), "LightGroupCounts", recreated, BufferKind::StructuredUAV))
			return false;
		nvrhi::IBuffer* records = m_SceneGPU->GetLightRecordBuffer();
		if (!records)
			return false;
		const std::array<const nvrhi::IBuffer*, 6> bound = { records, m_LightVisibleBuffer.Get(), m_LightGroupCountBuffer.Get(), m_LightBuffer.Get(),
			m_LightBoundsBuffer.Get(), m_LightCountBuffer.Get() };
		if (m_LightCullBindingSet && m_LightCompactBindingSet && bound == m_LightCullBound)
			return true;
		nvrhi::BindingSetDesc cullSet;
		cullSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::LightCullParameters)),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(18, records),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_LightVisibleBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_LightGroupCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_CullCounterBuffer)
		};
		nvrhi::BindingSetDesc compactSet;
		compactSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::LightCullParameters)),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(18, records),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(19, m_LightVisibleBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(20, m_LightGroupCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_LightBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_LightBoundsBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_LightCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_CullCounterBuffer)
		};
		m_LightCullBindingSet = m_Device->createBindingSet(cullSet, m_LightCullLayout);
		m_LightCompactBindingSet = m_Device->createBindingSet(compactSet, m_LightCompactLayout);
		m_LightCullBound = bound;
		return m_LightCullBindingSet && m_LightCompactBindingSet;
	}

	void SceneRenderer::RunGPULightCulling(nvrhi::ICommandList* commandList, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		if (!EnsureLightCullBindingSets())
		{
			// Nothing in view this frame: the clusters read a count of 0.
			m_Errors.Report("the GPU's light culling buffers could not be created; no point or spot light is drawn");
			commandList->clearBufferUInt(m_LightCountBuffer, 0);
			return;
		}
		RenderData::LightCullParameters parameters;
		// The view in camera-relative space, as the light records are tested (their positions minus the camera's).
		const Frustum relativeFrustum(camera.Projection * GetRelativeView(camera));
		for (size_t plane = 0; plane < 6; plane++)
			parameters.Planes[plane] = relativeFrustum.GetPlanes()[plane];
		parameters.RecordCount = m_GPULightRecords;
		parameters.GroupCount = (m_GPULightRecords + RenderData::c_LightGroupSize - 1) / RenderData::c_LightGroupSize;

		nvrhi::ComputeState state;
		state.pipeline = m_LightCullPipeline;
		state.bindings = { m_LightCullBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(parameters.GroupCount, 1, 1);
		state.pipeline = m_LightCompactPipeline;
		state.bindings = { m_LightCompactBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(parameters.GroupCount, 1, 1);
	}

	uint32_t SceneRenderer::GetBucketSlots(uint32_t bucket) const
	{
		const BucketParts parts = DecodeBucket(bucket);
		const size_t arena = parts.Geometry == RenderData::c_CullGeometryStatic ? 0 : 1;
		return parts.Shadow ? m_CullShadowSlots[parts.Cascade][arena] : m_CullViewSlots[arena];
	}

	void SceneRenderer::PrepareGPUCulling(const RenderScene& renderScene, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		// The GPU draws the meshes and materials of the scene's GPU copy's tables (RenderSceneGPU): whatever is loaded. The LOD
		// records of the meshes the CPU's marks found bound the draws a bucket can have: those in view for the view's buckets,
		// each cascade's casters' for its own.
		const RenderSceneGPU& mirror = *m_SceneGPU;
		const uint32_t meshSlots = mirror.GetMeshSlotCount();
		const auto arenaOf = [&mirror](uint32_t slot) -> size_t
		{
			return (mirror.GetMeshTableEntry(slot) & RenderData::c_CullMeshSkinnedArena) != 0 ? 1 : 0;
		};
		m_CullViewSlots = {};
		m_CullShadowSlots = {};
		for (const uint32_t slot : m_UsedMeshSlots)
		{
			if (slot < meshSlots && IsMeshInView(slot))
				m_CullViewSlots[arenaOf(slot)] += mirror.GetMeshRecords(slot);
		}
		if (m_FeedbackFrame)
		{
			// The feedback's view is a few frames old: room for meshes that came into view since (more show as overflow, after
			// which the CPU's marks run once).
			for (size_t arena = 0; arena < m_CullViewSlots.size(); arena++)
			{
				const uint32_t slots = m_CullViewSlots[arena];
				m_CullViewSlots[arena] = std::min(mirror.GetArenaRecords(static_cast<uint32_t>(arena)), slots + slots / 4 + c_FeedbackSlotHeadroom);
			}
		}
		if (++m_CullCasterStamp == 0)
		{
			for (std::vector<uint32_t>& stamps : m_CullCasterStamps)
				std::fill(stamps.begin(), stamps.end(), 0u);
			m_CullCasterStamp = 1;
		}
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			m_CullCasterStamps[cascade].resize(meshSlots, 0u);
		if (m_FeedbackFrame)
		{
			// The GPU finds the casters: the meshes the feedback saw cast, a quarter and some more, in every cascade.
			std::array<uint32_t, 2> casterRecords = {};
			for (const uint32_t slot : m_UsedMeshSlots)
			{
				if (slot < meshSlots && IsMeshCaster(slot))
					casterRecords[arenaOf(slot)] += mirror.GetMeshRecords(slot);
			}
			for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			{
				for (size_t arena = 0; arena < casterRecords.size(); arena++)
				{
					m_CullShadowSlots[cascade][arena] = std::min(mirror.GetArenaRecords(static_cast<uint32_t>(arena)),
						casterRecords[arena] + casterRecords[arena] / 4 + c_FeedbackSlotHeadroom);
				}
			}
		}
		for (const ShadowCaster& caster : m_ShadowCasters)
		{
			const uint32_t slot = renderScene.GetMeshSlot(caster.Proxy);
			if (slot >= meshSlots || mirror.GetMeshRecords(slot) == 0)
				continue;
			const size_t arena = arenaOf(slot);
			for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			{
				if ((caster.CascadeMask & (1u << cascade)) == 0 || m_CullCasterStamps[cascade][slot] == m_CullCasterStamp)
					continue;
				m_CullCasterStamps[cascade][slot] = m_CullCasterStamp;
				m_CullShadowSlots[cascade][arena] += mirror.GetMeshRecords(slot);
			}
		}
		const bool doubleSided = mirror.HasDoubleSidedMaterials();
		const bool alphaMask = mirror.HasAlphaMaskedMaterials();

		// The buckets the frame can have get dense indices, the view's first.
		const bool mirrored = renderScene.GetStats().MirroredProxies > 0;
		const std::array<bool, 3> geometries = { mirror.GetArenaRecords(0) > 0, mirror.GetArenaRecords(1) > 0,
			mirror.GetArenaRecords(1) > 0 && renderScene.GetStats().PosedProxies > 0 };
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
		const uint32_t firstPhaseViewBuckets = static_cast<uint32_t>(m_CullBuckets.size());
		const bool occlusion = IsOcclusionCulled();
		if (occlusion)
		{
			// The second phase's view buckets: the same states for what the first phase did not draw.
			const uint32_t count = static_cast<uint32_t>(m_CullBuckets.size());
			for (uint32_t dense = 0; dense < count; dense++)
			{
				const uint32_t bucket = m_CullBuckets[dense] | RenderData::MakeViewBucket(false, false, 0, true);
				m_CullBucketTable[bucket] = static_cast<uint32_t>(m_CullBuckets.size());
				m_CullBuckets.push_back(bucket);
			}
		}
		const uint32_t viewBuckets = static_cast<uint32_t>(m_CullBuckets.size());
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			addBuckets(true, cascade);

		RenderData::CullConstants& constants = m_CullConstants;
		constants = RenderData::CullConstants {};
		// The view in camera-relative space (the shaders' and the culling's: precision does not depend on the distance from the
		// origin).
		const glm::mat4 relativeViewProjection = camera.Projection * GetRelativeView(camera);
		const Frustum relativeFrustum(relativeViewProjection);
		for (size_t plane = 0; plane < 6; plane++)
			constants.Planes[plane] = relativeFrustum.GetPlanes()[plane];
		constants.CameraPosition = glm::vec4(camera.Position, camera.Orthographic ? 1.0f : 0.0f);
		// As LODSelection::GetScreenFraction computes them.
		const float viewExtent = camera.Orthographic ? std::max(camera.VerticalFOV, 1e-4f) : std::tan(std::max(camera.VerticalFOV, 1e-3f) * 0.5f);
		constants.LODParameters = glm::vec4(m_Specification.LODThreshold, viewExtent, camera.Near, std::max(m_Specification.MinProjectedPixels, 0.0f));
		constants.ViewParameters = glm::vec4(static_cast<float>(m_ViewportSize.y), std::max(camera.Near, 1e-4f), std::max(m_Specification.PrefetchDistance, 0.0f),
			m_Specification.HighPriorityScreenFraction);
		constants.LightView = m_LightView;
		constants.ViewProjection = relativeViewProjection;
		constants.OcclusionParameters = glm::vec4(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y),
			static_cast<float>(m_DepthPyramidLevels), occlusion ? 1.0f : 0.0f);
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
		{
			const CascadeBox& box = m_Cascades[cascade];
			constants.CascadeBoxes[cascade] = glm::vec4(box.RelativeMin, box.RelativeMax);
			constants.CascadeParameters[cascade] = glm::vec4(box.RelativeFarZ, c_MinCasterTexels * box.WorldPerTexel, 0.0f, 0.0f);
		}
		constants.ProxyCount = renderScene.GetProxyCapacity();
		constants.CascadeCount = m_CascadeCount;
		constants.LODCapacity = std::max(Renderer::GetGeometryPool()->GetLODCapacity(), 1u);
		constants.MeshSlotCount = meshSlots;
		constants.MaterialSlotCount = mirror.GetMaterialSlotCount();
		constants.DrawListCapacity = static_cast<uint32_t>(std::min<uint64_t>(renderScene.GetInstances().size() * (1ull + m_CascadeCount), 0xFFFFFFF0ull));
		constants.ArgumentsPerBucket = std::max({ mirror.GetArenaRecords(0), mirror.GetArenaRecords(1), 1u });
		constants.BucketCount = static_cast<uint32_t>(m_CullBuckets.size());
		constants.ViewBucketCount = viewBuckets;
		constants.DrawsBounded = GetGPUIndirectPath() != GPUIndirectPath::Count ? 1u : 0u;
		constants.Feedback = m_Specification.VisibilityFeedback ? 1u : 0u;
		constants.FirstPhaseViewBuckets = firstPhaseViewBuckets;
		constants.VisibilityParity = m_VisibilityParity;
		constants.LODErrorThreshold = m_LODCriteria.ErrorThreshold;
		// The bucket table's second part: each dense bucket's draw slots.
		for (const uint32_t bucket : m_CullBuckets)
			m_CullBucketTable.push_back(GetBucketSlots(bucket));
	}

	bool SceneRenderer::EnsureCullBuffers(bool& outRecreated)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const uint64_t keys = static_cast<uint64_t>(std::max(constants.BucketCount, 1u)) * constants.LODCapacity;
		const uint64_t arguments = static_cast<uint64_t>(std::max(constants.BucketCount, 1u)) * constants.ArgumentsPerBucket * c_ArgumentWords;
		return EnsureBufferCapacity(m_CullCountBuffer, keys, sizeof(uint32_t), "CullCounts", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_CullOffsetBuffer, keys, sizeof(uint32_t), "CullOffsets", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_CullArgumentBuffer, arguments, sizeof(uint32_t), "CullArguments", outRecreated, BufferKind::IndirectArgumentsUAV)
			&& EnsureBufferCapacity(m_GPUDrawListBuffer, constants.DrawListCapacity, sizeof(uint32_t), "GPUDrawList", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_FeedbackMeshBuffer, static_cast<size_t>(constants.MeshSlotCount) * 2, sizeof(uint32_t), "FeedbackMeshes", outRecreated,
				BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_FeedbackMaterialBuffer, static_cast<size_t>(constants.MaterialSlotCount) * 4, sizeof(uint32_t), "FeedbackMaterials", outRecreated,
				BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_FeedbackListBuffer, RenderData::c_FeedbackListHeader + static_cast<size_t>(constants.MeshSlotCount) + constants.MaterialSlotCount,
				sizeof(uint32_t), "FeedbackLists", outRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_FeedbackRecordBuffer, 1 + static_cast<size_t>(constants.MeshSlotCount) + constants.MaterialSlotCount,
				sizeof(RenderData::FeedbackRecord), "FeedbackRecords", outRecreated, BufferKind::StructuredUAV)
			&& EnsureVisibilityBuffer(outRecreated);
	}

	bool SceneRenderer::EnsureVisibilityBuffer(bool& outRecreated)
	{
		// A new buffer holds nothing of the previous frames: the view starts without history.
		bool recreated = false;
		if (!EnsureBufferCapacity(m_VisibilityBuffer, m_CullConstants.ProxyCount, sizeof(uint32_t), "Visibility", recreated, BufferKind::StructuredUAV))
			return false;
		m_ResetVisibility = m_ResetVisibility || recreated;
		outRecreated = outRecreated || recreated;
		return true;
	}

	bool SceneRenderer::IsOcclusionCulled() const
	{
		return m_GPUDrivenFrame && m_Specification.OcclusionCulling && m_DepthPyramid != nullptr;
	}

	bool SceneRenderer::CreateDepthPyramid()
	{
		if (!m_DepthPyramidPipeline)
			return true; // No GPU-driven path: nothing to cull with it
		// Level 0 is half the depth buffer (rounded up), down to one texel.
		m_DepthPyramidSize = glm::uvec2((m_ViewportSize.x + 1) / 2, (m_ViewportSize.y + 1) / 2);
		uint32_t levels = 1;
		while ((std::max(m_DepthPyramidSize.x, m_DepthPyramidSize.y) >> levels) > 0)
			levels++;
		nvrhi::TextureDesc desc;
		desc.width = m_DepthPyramidSize.x;
		desc.height = m_DepthPyramidSize.y;
		desc.mipLevels = levels;
		desc.format = nvrhi::Format::R32_FLOAT;
		desc.isUAV = true;
		desc.debugName = m_Specification.DebugName + ".DepthPyramid";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		m_DepthPyramid = m_Device->createTexture(desc);
		if (!m_DepthPyramid)
			return false;
		m_DepthPyramidLevels = levels;
		m_DepthPyramidSets.clear();
		for (uint32_t level = 0; level < levels; level++)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {
				level == 0 ? nvrhi::BindingSetItem::Texture_SRV(0, m_DepthTexture)
					: nvrhi::BindingSetItem::Texture_SRV(0, m_DepthPyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level - 1, 1, 0, 1)),
				nvrhi::BindingSetItem::Sampler(0, m_DepthPyramidSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, m_DepthPyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level, 1, 0, 1)),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(DepthPyramidParameters))
			};
			m_DepthPyramidSets.push_back(m_Device->createBindingSet(set, m_DepthPyramidLayout));
			if (!m_DepthPyramidSets.back())
				return false;
		}
		return true;
	}

	bool SceneRenderer::CreateCullBindingSet()
	{
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_CullConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_SceneGPU->GetProxyBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_SceneGPU->GetInstanceBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_SceneGPU->GetMeshTableBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_SceneGPU->GetMaterialFlagBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(4, pool.GetEntryBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(5, pool.GetSubmeshBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(6, pool.GetLODBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_BucketTableBuffer),
			nvrhi::BindingSetItem::Texture_SRV(8, m_DepthPyramid),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CullCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_CullOffsetBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(2, m_CullArgumentBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_CullBucketCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_CullCounterBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_GPUDrawListBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_FeedbackMeshBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(7, m_FeedbackMaterialBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(8, m_FeedbackListBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(9, m_FeedbackRecordBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(10, m_CasterDepthBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(11, m_VisibilityBuffer),
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
		if (!m_CullTablesUploaded || m_CullBucketTable != m_CullBucketTableUploaded)
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
		if (shadows)
			commandList->clearBufferUInt(m_CasterDepthBuffer, 0); // No caster yet in any cascade
		if (m_ResetVisibility)
		{
			// No history (a camera cut, the first frame, another scene): the first phase draws nothing.
			commandList->clearBufferUInt(m_VisibilityBuffer, 0);
			m_ResetVisibility = false;
		}
		const bool feedback = constants.Feedback != 0;
		if (feedback)
		{
			commandList->clearBufferUInt(m_FeedbackMeshBuffer, 0);
			commandList->clearBufferUInt(m_FeedbackMaterialBuffer, 0);
			commandList->clearBufferUInt(m_FeedbackListBuffer, 0);
		}

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
		const auto barrier = [&]() { CullBarrier(commandList); };

		cullPasses(c_CullModeCount);
		barrier();
		state.pipeline = m_CullBuildPipeline;
		commandList->setComputeState(state);
		const uint32_t mode = c_CullModeCount;
		commandList->setPushConstants(&mode, sizeof(mode));
		Dispatch(commandList, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		if (feedback)
		{
			// The touched slots' feedback, packed (it reads the counting passes' writes, as the build does).
			state.pipeline = m_CullFeedbackPipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&mode, sizeof(mode));
			Dispatch(commandList, 1 + static_cast<uint64_t>(constants.MeshSlotCount) + constants.MaterialSlotCount);
		}
		barrier();
		cullPasses(c_CullModeWrite);
		barrier();
	}

	void SceneRenderer::CullBarrier(nvrhi::ICommandList* commandList)
	{
		// The passes go through one binding set, and NVRHI places barriers only when binding sets change: each pass must see
		// the previous one's writes.
		for (nvrhi::IBuffer* buffer : { m_CullCountBuffer.Get(), m_CullOffsetBuffer.Get(), m_CullArgumentBuffer.Get(), m_CullBucketCountBuffer.Get(),
			m_CullCounterBuffer.Get(), m_GPUDrawListBuffer.Get(), m_FeedbackMeshBuffer.Get(), m_FeedbackMaterialBuffer.Get(), m_FeedbackListBuffer.Get(),
			m_FeedbackRecordBuffer.Get(), m_CasterDepthBuffer.Get(), m_VisibilityBuffer.Get() })
			commandList->setBufferState(buffer, nvrhi::ResourceStates::UnorderedAccess);
		commandList->commitBarriers();
	}

	void SceneRenderer::RunOcclusionPhase(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		// The depth pyramid of the first phase's depth, level by level.
		nvrhi::ComputeState pyramidState;
		pyramidState.pipeline = m_DepthPyramidPipeline;
		glm::uvec2 source = m_ViewportSize;
		glm::uvec2 destination = m_DepthPyramidSize;
		for (uint32_t level = 0; level < m_DepthPyramidLevels; level++)
		{
			pyramidState.bindings = { m_DepthPyramidSets[level] };
			commandList->setComputeState(pyramidState);
			DepthPyramidParameters parameters;
			parameters.SourceSize = source;
			parameters.DestinationSize = destination;
			parameters.UseMinimumSampler = Renderer::GetGraphicsDevice().GetInfo().SupportsSamplerMinMax ? 1u : 0u;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch((destination.x + 7) / 8, (destination.y + 7) / 8, 1);
			source = destination;
			destination = glm::max((destination + 1u) / 2u, glm::uvec2(1));
		}

		// The second phase: every proxy's visibility against the pyramid, and the draws of those the first phase did not draw.
		const RenderData::CullConstants& constants = m_CullConstants;
		nvrhi::ComputeState state;
		state.bindings = { m_CullBindingSet };
		state.pipeline = m_CullViewPipeline;
		const auto pass = [&](uint32_t mode, nvrhi::IComputePipeline* pipeline, uint64_t items)
		{
			state.pipeline = pipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&mode, sizeof(mode));
			Dispatch(commandList, items);
			CullBarrier(commandList);
		};
		pass(c_CullModeSecondPhase | c_CullModeCount, m_CullViewPipeline, constants.ProxyCount);
		pass(c_CullModeSecondPhase, m_CullBuildPipeline, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		pass(c_CullModeSecondPhase | c_CullModeWrite, m_CullViewPipeline, constants.ProxyCount);
		// This frame's visibility is the next one's history.
		m_VisibilityParity ^= 1u;
	}

	void SceneRenderer::RecordCullReadback(nvrhi::ICommandList* commandList)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const bool feedback = constants.Feedback != 0;
		CounterReadback& readback = m_CounterReadbacks[m_CounterReadbackIndex];
		readback.Readback->Record(commandList, m_CullCounterBuffer, 0, sizeof(RenderData::CullCounters));
		readback.Render = m_RenderCount + 1;
		readback.HasFeedback = false;
		if (!feedback)
			return;
		// The feedback's copy: a readback whose earlier copy may still be under way is replaced only once it completed.
		const uint64_t feedbackBytes = (1 + static_cast<uint64_t>(constants.MeshSlotCount) + constants.MaterialSlotCount) * sizeof(RenderData::FeedbackRecord);
		if (!readback.Feedback || readback.Feedback->GetCapacity() < feedbackBytes)
		{
			if (readback.Feedback && readback.Feedback->IsPending() && !readback.Feedback->IsReady())
				return;
			uint64_t capacity = readback.Feedback ? readback.Feedback->GetCapacity() : 4096;
			while (capacity < feedbackBytes)
				capacity *= 2;
			readback.Feedback = CreateScope<BufferReadback>(m_Device, capacity, m_Specification.DebugName + ".FeedbackReadback");
			if (!readback.Feedback->IsValid())
			{
				readback.Feedback.reset();
				return; // Logged; the CPU's marks run while there is no feedback
			}
		}
		readback.Feedback->Record(commandList, m_FeedbackRecordBuffer, 0, feedbackBytes);
		readback.HasFeedback = true;
		readback.RenderSceneId = m_RenderSceneId;
		readback.TablesVersion = m_SceneGPU->GetTablesVersion();
		readback.MeshSlots = constants.MeshSlotCount;
		readback.MaterialSlots = constants.MaterialSlotCount;
	}

	void SceneRenderer::SubmitCullReadback()
	{
		m_CounterReadbacks[m_CounterReadbackIndex].Readback->Submit();
		if (m_CounterReadbacks[m_CounterReadbackIndex].HasFeedback)
			m_CounterReadbacks[m_CounterReadbackIndex].Feedback->Submit();
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

	void SceneRenderer::DrawGPUView(nvrhi::ICommandList* commandList, Pass pass, uint32_t firstDense, uint32_t endDense)
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
		for (uint32_t dense = firstDense; dense < std::min(endDense, m_CullConstants.ViewBucketCount); dense++)
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
		stats.Lights = counters.VisibleLights;
		stats.CulledLights = counters.DrawnLights > counters.VisibleLights ? counters.DrawnLights - counters.VisibleLights : 0u;
		stats.Triangles = (static_cast<uint64_t>(counters.TrianglesHigh) << 32) | counters.TrianglesLow;
		stats.DrawListEntries = counters.DrawListUsed;
		stats.Overflow = counters.Overflow;
		stats.OccludedProxies = counters.Occluded;
		for (size_t lod = 0; lod < stats.InstancesPerLOD.size(); lod++)
			stats.InstancesPerLOD[lod] = counters.InstancesPerLOD[lod];

		// The frame's visibility feedback: the touched mesh slots' records, then the material slots' (each part's count in
		// the header, at most its places).
		if (!newest->HasFeedback || !newest->Feedback)
			return;
		RenderData::FeedbackRecord header;
		if (!newest->Feedback->Read(&header, sizeof(header)))
			return;
		const uint32_t meshes = std::min(header.Slot, newest->MeshSlots);
		const uint32_t materials = std::min(header.Use, newest->MaterialSlots);
		m_Feedback.Meshes.resize(meshes);
		m_Feedback.Materials.resize(materials);
		const bool read = (meshes == 0 || newest->Feedback->Read(m_Feedback.Meshes.data(), meshes * sizeof(RenderData::FeedbackRecord), sizeof(header)))
			&& (materials == 0 || newest->Feedback->Read(m_Feedback.Materials.data(), materials * sizeof(RenderData::FeedbackRecord),
				(1 + static_cast<uint64_t>(newest->MeshSlots)) * sizeof(RenderData::FeedbackRecord)));
		m_Feedback.Valid = read;
		m_Feedback.Render = newest->Render;
		m_Feedback.RenderSceneId = newest->RenderSceneId;
		m_Feedback.TablesVersion = newest->TablesVersion;
	}

	bool SceneRenderer::UsesFeedback(const RenderScene& renderScene) const
	{
		if (!m_GPUDrivenFrame || !m_Specification.VisibilityFeedback || !m_Feedback.Valid || m_Feedback.RenderSceneId != renderScene.GetId())
			return false;
		const uint64_t frame = m_RenderCount + 1;
		if (m_Feedback.Render < m_CutRender || frame - m_Feedback.Render > c_MaxFeedbackAge || m_Feedback.TablesVersion != m_SceneGPU->GetTablesVersion())
			return false;
		// Draws beyond the slots a feedback frame foresaw were left out: the CPU's marks bound them exactly once.
		return !(m_GPUStats.HasCounters && m_GPUStats.Overflow > 0 && m_GPUStats.CountersRender > m_MarksRender);
	}

}
