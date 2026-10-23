#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/TextureTable.h"

#include <glm/gtc/matrix_transform.hpp>

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
		// Cull.comp's and CullBuild.comp's modes: bit 0 writes the draw list (else counts), bit 1 runs the second phase (of the
		// view, or with Scene/CullShadow.comp of the cascades); CullBuild.comp builds the cascades' second phase with bit 2.
		constexpr uint32_t c_CullModeCount = 0;
		constexpr uint32_t c_CullModeWrite = 1;
		constexpr uint32_t c_CullModeSecondPhase = 2;
		constexpr uint32_t c_CullModeShadowSecondPhase = 4;
		// Cull.comp: the local shadow views alone; CullBuild.comp: their buckets, or clearing them for the next batch of views.
		constexpr uint32_t c_CullModeLocal = 8;
		constexpr uint32_t c_CullModeResetLocal = 16;
		// A cascade's depth pyramid starts at a quarter of its shadow map each way (an eighth of the memory a half would take).
		constexpr uint32_t c_ShadowPyramidReduction = 4;
		// The cascades' occlusion culling costs GPU time (their pyramids, a second culling pass over the proxies, and the wait
		// for the first phase's shadow maps) that only hidden casters pay back: it runs while the latest frame that ran it found
		// at least 1 / c_ShadowOcclusionPaysShare of the cascades' casters hidden, and otherwise every
		// c_ShadowOcclusionProbeFrames frames to look again.
		constexpr uint64_t c_ShadowOcclusionPaysShare = 16;
		constexpr uint64_t c_ShadowOcclusionProbeFrames = 16;
		// Shadow cascade caching follows the old bounds of at most this many proxies that changed (beyond, a change of one it
		// does not follow redraws every cascade), and tests boxes against the cascades with a margin of this many texels.
		constexpr size_t c_MaxShadowCacheBounds = 65536;
		constexpr float c_ShadowCacheMarginTexels = 2.0f;

		// Scene/HZBBuild.comp's push constants.
		struct DepthPyramidParameters
		{
			glm::uvec2 SourceSize = glm::uvec2(0);
			glm::uvec2 DestinationSize = glm::uvec2(0);
			uint32_t UseMinimumSampler = 0;
			uint32_t Reduction = 2; // Source texels per destination texel each way
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

		// A bucket's parts (RenderData::MakeViewBucket, MakeShadowBucket, MakeLocalShadowBucket).
		struct BucketParts
		{
			bool Shadow = false;
			bool Local = false;    // A local shadow view's (Cascade: its place in its batch)
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
			if (bucket >= RenderData::c_CullFirstLocalShadowBucket)
			{
				parts.Local = true;
				parts.Cascade = (bucket - RenderData::c_CullFirstLocalShadowBucket) / RenderData::c_CullLocalShadowBucketsPerView;
				local = (bucket - RenderData::c_CullFirstLocalShadowBucket) % RenderData::c_CullLocalShadowBucketsPerView;
				parts.AlphaMask = (local & RenderData::c_CullShadowAlphaMask) != 0;
			}
			else if (bucket >= RenderData::c_CullViewBuckets)
			{
				parts.Shadow = true;
				parts.Cascade = (bucket - RenderData::c_CullViewBuckets) / RenderData::c_CullShadowBucketsPerCascade;
				local = (bucket - RenderData::c_CullViewBuckets) % RenderData::c_CullShadowBucketsPerCascade;
				parts.AlphaMask = (local & RenderData::c_CullShadowAlphaMask) != 0;
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
		combined.LocalShadowCasters = gpu.LocalShadowInstances;
		combined.LocalShadowDrawCalls = gpu.LocalShadowDraws;
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
		combined.OccludedCasters = gpu.OccludedCasters;
		for (size_t lod = 0; lod < combined.InstancesPerLOD.size(); lod++)
			combined.InstancesPerLOD[lod] += gpu.InstancesPerLOD[lod];
		return combined;
	}

	bool SceneRenderer::IsGPUDriven() const
	{
		return m_Specification.GPUDriven == GPUDrivenMode::Auto && m_CullViewPipeline && m_CullShadowPipeline && m_CullBuildPipeline
			&& Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	uint32_t SceneRenderer::GetMaxDrawsPerIndirectCall() const
	{
		const uint32_t device = Renderer::GetGraphicsDevice().GetInfo().MaxDrawIndirectCount;
		const uint32_t limit = m_Specification.MaxDrawsPerIndirectCall > 0 ? std::min(m_Specification.MaxDrawsPerIndirectCall, device) : device;
		return std::max(limit, 1u);
	}

	SceneRenderer::GPUIndirectPath SceneRenderer::GetGPUIndirectPath(uint32_t argumentsPerBucket) const
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
		// A bucket's GPU-written count is at most its slots (its draws are records of its arena), and the count must stay
		// within the limit: buckets of more slots are drawn without it, by calls of at most the limit.
		if (info.SupportsDrawIndirectCount && argumentsPerBucket <= GetMaxDrawsPerIndirectCall())
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
			nvrhi::BindingLayoutItem::Texture_SRV(9),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11),
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
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(12),
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
		// The cascades' pyramids: layers of array textures.
		pyramidDesc.CS = shaders.Get("Scene/HZBBuildArray.comp");
		m_ShadowPyramidPipeline = pyramidDesc.CS && m_DepthPyramidLayout ? m_Device->createComputePipeline(pyramidDesc) : nullptr;
		nvrhi::SamplerDesc pyramidSampler;
		pyramidSampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		if (Renderer::GetGraphicsDevice().GetInfo().SupportsSamplerMinMax)
			pyramidSampler.setAllFilters(true).setReductionType(nvrhi::SamplerReductionType::Minimum);
		else
			pyramidSampler.setAllFilters(false);
		m_DepthPyramidSampler = m_Device->createSampler(pyramidSampler);
		// Bound in place of the cascades' pyramids until the shadow map exists (nothing reads it then).
		nvrhi::TextureDesc dummyPyramid;
		dummyPyramid.width = 1;
		dummyPyramid.height = 1;
		dummyPyramid.arraySize = RenderData::c_CullMaxCascades;
		dummyPyramid.dimension = nvrhi::TextureDimension::Texture2DArray;
		dummyPyramid.format = nvrhi::Format::R32_FLOAT;
		dummyPyramid.debugName = m_Specification.DebugName + ".DummyShadowPyramid";
		dummyPyramid.initialState = nvrhi::ResourceStates::ShaderResource;
		dummyPyramid.keepInitialState = true;
		m_DummyShadowPyramid = m_Device->createTexture(dummyPyramid);

		bool recreated = false;
		const bool created = EnsureBufferCapacity(m_BucketTableBuffer, 2 * RenderData::c_CullBuckets, sizeof(uint32_t), "CullBuckets", recreated)
			&& EnsureBufferCapacity(m_CullBucketCountBuffer, RenderData::c_CullBuckets, sizeof(uint32_t), "CullBucketCounts", recreated, BufferKind::IndirectArgumentsUAV)
			&& EnsureBufferCapacity(m_CullCounterBuffer, sizeof(RenderData::CullCounters) / sizeof(uint32_t), sizeof(uint32_t), "CullCounters", recreated,
				BufferKind::StructuredUAV);
		m_CullLocalViewBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_MaxShadowedLocalLights * RenderData::c_LocalShadowFaces * sizeof(RenderData::CullLocalShadowView))
			.setStructStride(sizeof(RenderData::CullLocalShadowView))
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".CullLocalShadowViews"));
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
			|| !m_ShadowPyramidPipeline || !m_DummyShadowPyramid || !created || !m_CullConstantBuffer || !m_CullLocalViewBuffer || !readbacks)
		{
			// The CPU path draws instead.
			ST_CORE_ERROR("{}: the GPU-driven path is unavailable (its pipelines or buffers could not be created); the CPU culls and draws",
				m_Specification.DebugName);
			ReleaseCullResources();
		}
	}

	void SceneRenderer::ReleaseCullResources()
	{
		// None of the path stays, so nothing sized with the viewport (the depth pyramid) or the shadow map follows it either.
		m_CullBindingLayout = nullptr;
		m_CullViewPipeline = nullptr;
		m_CullShadowPipeline = nullptr;
		m_CullBuildPipeline = nullptr;
		m_CullFeedbackPipeline = nullptr;
		m_DepthPyramidLayout = nullptr;
		m_DepthPyramidPipeline = nullptr;
		m_ShadowPyramidPipeline = nullptr;
		m_DepthPyramidSampler = nullptr;
		m_DummyShadowPyramid = nullptr;
		m_BucketTableBuffer = nullptr;
		m_CullBucketCountBuffer = nullptr;
		m_CullCounterBuffer = nullptr;
		m_CullConstantBuffer = nullptr;
		m_CullLocalViewBuffer = nullptr;
		for (CounterReadback& entry : m_CounterReadbacks)
			entry.Readback.reset();
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
		if (parts.Local)
			return m_CullLocalSlots[parts.Cascade][arena];
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
		// The buckets of the view, a cascade or a local shadow view (`cascade`: its place in its batch).
		enum class BucketKind : uint8_t { View, Cascade, LocalView };
		const auto addBuckets = [&](BucketKind kind, uint32_t cascade)
		{
			const bool shadow = kind != BucketKind::View;
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
							uint32_t bucket = RenderData::MakeViewBucket(mirror != 0, sided != 0, geometry);
							if (kind == BucketKind::Cascade)
								bucket = RenderData::MakeShadowBucket(cascade, mirror != 0, sided != 0, geometry, masked != 0);
							else if (kind == BucketKind::LocalView)
								bucket = RenderData::MakeLocalShadowBucket(cascade, mirror != 0, sided != 0, geometry, masked != 0);
							m_CullBucketTable[bucket] = static_cast<uint32_t>(m_CullBuckets.size());
							m_CullBuckets.push_back(bucket);
						}
					}
				}
			}
		};
		addBuckets(BucketKind::View, 0);
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
			addBuckets(BucketKind::Cascade, cascade);
		const uint32_t firstPhaseBuckets = static_cast<uint32_t>(m_CullBuckets.size());
		const uint64_t frame = m_RenderCount + 1;
		// Without history (a camera cut, the first frame) the first phase would draw nothing: the cascades are drawn whole, and
		// that records their history.
		const bool shadowOcclusion = occlusion && m_CascadeCount > 0 && m_ShadowPyramid != nullptr && !m_ResetVisibility
			&& (m_ShadowOcclusionPays || frame >= m_ShadowOcclusionRender + c_ShadowOcclusionProbeFrames);
		m_Stats.ShadowOcclusionCulled = shadowOcclusion;
		if (shadowOcclusion)
		{
			m_ShadowOcclusionRender = frame;
			// The cascades' second-phase buckets: the same states for what their first phase did not draw.
			for (uint32_t dense = viewBuckets; dense < firstPhaseBuckets; dense++)
			{
				const uint32_t bucket = m_CullBuckets[dense] + RenderData::c_CullShadowSecondPhase;
				m_CullBucketTable[bucket] = static_cast<uint32_t>(m_CullBuckets.size());
				m_CullBuckets.push_back(bucket);
			}
		}
		// The local shadow views' buckets, last: one set per place in a batch of views. Their draw slots are the records of the
		// meshes within each view's light's range (the CPU's query, which the GPU's exact tests stay within), the most any
		// batch's view at that place has; their instances bound the draw list.
		const uint32_t firstLocalBucket = static_cast<uint32_t>(m_CullBuckets.size());
		const uint32_t localViews = static_cast<uint32_t>(m_LocalShadowViews.size());
		for (uint32_t view = 0; view < std::min(localViews, RenderData::c_CullMaxLocalShadowViews); view++)
			addBuckets(BucketKind::LocalView, view);
		m_CullLocalSlots = {};
		m_CullLocalViews.clear();
		uint64_t localEntries = 0;
		m_CullLocalStamps.resize(meshSlots, 0u);
		const glm::dvec3 cameraPosition(camera.Position);
		for (uint32_t index = 0; index < localViews; index++)
		{
			const LocalShadowDrawView& view = m_LocalShadowViews[index];
			if (++m_CullLocalStamp == 0)
			{
				std::fill(m_CullLocalStamps.begin(), m_CullLocalStamps.end(), 0u);
				m_CullLocalStamp = 1;
			}
			std::array<uint32_t, 2> records = {};
			for (uint32_t item = view.FirstCandidate; item < view.FirstCandidate + view.CandidateCount; item++)
			{
				const uint32_t proxy = m_LocalShadowCandidates[item];
				if ((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::CastShadows) == 0)
					continue;
				const uint32_t slot = renderScene.GetMeshSlot(proxy);
				if (slot >= meshSlots || mirror.GetMeshRecords(slot) == 0)
					continue;
				localEntries += renderScene.GetInstanceCount(proxy);
				if (m_CullLocalStamps[slot] == m_CullLocalStamp)
					continue;
				m_CullLocalStamps[slot] = m_CullLocalStamp;
				records[arenaOf(slot)] += mirror.GetMeshRecords(slot);
			}
			std::array<uint32_t, 2>& slots = m_CullLocalSlots[index % RenderData::c_CullMaxLocalShadowViews];
			for (size_t arena = 0; arena < slots.size(); arena++)
				slots[arena] = std::max(slots[arena], records[arena]);

			const LocalShadowViewKey& key = m_LocalShadowLights[view.Light].Views[view.Face].Key;
			RenderData::CullLocalShadowView record;
			for (size_t plane = 0; plane < 6; plane++)
				record.Planes[plane] = view.Planes[plane];
			record.Light = glm::vec4(key.Position, key.Range);
			record.RelativeLight = glm::vec4(glm::vec3(glm::dvec3(key.Position) - cameraPosition), key.Near);
			record.Parameters = glm::vec4(key.TanHalfAngle, static_cast<float>(key.Tile.Size), view.LOD.ErrorThreshold, 0.0f);
			m_CullLocalViews.push_back(record);
		}

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
		// A view instance whose level cross-fades is drawn twice.
		const uint64_t viewEntries = m_Specification.FadeLevelsOfDetail ? 2ull : 1ull;
		constants.DrawListCapacity = static_cast<uint32_t>(std::min<uint64_t>(renderScene.GetInstances().size() * (viewEntries + m_CascadeCount) + localEntries,
			0xFFFFFFF0ull));
		m_CullInstanceCount = static_cast<uint32_t>(renderScene.GetInstances().size());
		constants.ArgumentsPerBucket = std::max({ mirror.GetArenaRecords(0), mirror.GetArenaRecords(1), 1u });
		constants.BucketCount = static_cast<uint32_t>(m_CullBuckets.size());
		constants.ViewBucketCount = viewBuckets;
		constants.DrawsBounded = GetGPUIndirectPath(constants.ArgumentsPerBucket) != GPUIndirectPath::Count ? 1u : 0u;
		constants.Feedback = m_Specification.VisibilityFeedback ? 1u : 0u;
		constants.FirstPhaseViewBuckets = firstPhaseViewBuckets;
		constants.VisibilityParity = m_VisibilityParity;
		constants.LODErrorThreshold = m_LODCriteria.ErrorThreshold;
		constants.LODFadeFrame = m_LODFadeFrame;
		constants.LODFade = m_Specification.FadeLevelsOfDetail ? 1u : 0u;
		constants.ShadowOcclusion = shadowOcclusion ? 1u : 0u;
		constants.SecondPhaseShadowBuckets = firstPhaseBuckets;
		constants.ShadowMapSize = m_Specification.ShadowMapSize;
		constants.ShadowPyramidLevels = m_ShadowPyramidLevels;
		constants.FirstLocalBucket = firstLocalBucket;
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
			&& EnsureVisibilityBuffer(outRecreated) && EnsureLODFadeBuffer(outRecreated);
	}

	bool SceneRenderer::EnsureLODFadeBuffer(bool& outRecreated)
	{
		// A new buffer holds no history: its instances draw their levels at once.
		bool recreated = false;
		if (!EnsureBufferCapacity(m_LODFadeBuffer, m_CullInstanceCount, sizeof(RenderData::LODFadeState), "LODFades", recreated, BufferKind::StructuredUAV))
			return false;
		m_ClearLODFades = m_ClearLODFades || recreated;
		outRecreated = outRecreated || recreated;
		return true;
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
		if (!m_CullViewPipeline)
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
			nvrhi::BindingSetItem::Texture_SRV(9, m_ShadowPyramid ? m_ShadowPyramid.Get() : m_DummyShadowPyramid.Get()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_CascadeBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(11, m_CullLocalViewBuffer),
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
			nvrhi::BindingSetItem::StructuredBuffer_UAV(12, m_LODFadeBuffer),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(uint32_t))
		};
		m_CullBindingSet = m_Device->createBindingSet(desc, m_CullBindingLayout);
		m_CullBoundSceneVersion = m_SceneGPU->GetBufferVersion();
		m_CullBoundTableVersion = pool.GetTableVersion();
		m_CullBoundShadowPyramid = m_ShadowPyramid.Get();
		return m_CullBindingSet != nullptr;
	}

	bool SceneRenderer::UploadCullTables(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		GeometryPool& pool = *Renderer::GetGeometryPool();
		if (!pool.RecordTableUploads(commandList))
			return false; // Logged by the pool
		// Tables replaced (the pool's, the scene copy's or the renderer's): the binding set follows.
		if (!m_CullBindingSet || m_CullBoundSceneVersion != m_SceneGPU->GetBufferVersion() || m_CullBoundTableVersion != pool.GetTableVersion()
			|| m_CullBoundShadowPyramid != m_ShadowPyramid.Get())
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
		if (!m_CullLocalViews.empty())
			commandList->writeBuffer(m_CullLocalViewBuffer, m_CullLocalViews.data(), m_CullLocalViews.size() * sizeof(RenderData::CullLocalShadowView));
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
		// What the cascades' occlusion culling finds says whether it pays only where it had history.
		m_ShadowOcclusionMeasured = constants.ShadowOcclusion != 0 && !m_ResetVisibility;
		if (m_ResetVisibility)
		{
			// No history (a camera cut, the first frame, another scene): the first phase draws nothing.
			commandList->clearBufferUInt(m_VisibilityBuffer, 0);
			m_ResetVisibility = false;
		}
		if (m_ClearLODFades)
		{
			commandList->clearBufferUInt(m_LODFadeBuffer, 0);
			m_ClearLODFades = false;
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
		// The cascades and the first batch of local shadow views are culled together; later batches after the first's draws
		// (DrawGPULocalShadows).
		const uint32_t localViews = static_cast<uint32_t>(m_LocalShadowViews.size());
		const uint32_t localMode = GetLocalShadowCullMode(0, std::min(localViews, RenderData::c_CullMaxLocalShadowViews));
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
			const uint32_t shadowMode = mode | localMode;
			commandList->setPushConstants(&shadowMode, sizeof(shadowMode));
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
		if (localViews > 0)
		{
			commandList->setPushConstants(&c_CullModeLocal, sizeof(c_CullModeLocal));
			Dispatch(commandList, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		}
		// The feedback is packed once every counting pass ran: after the last batch of local shadow views.
		if (feedback && localViews <= RenderData::c_CullMaxLocalShadowViews)
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
			m_FeedbackRecordBuffer.Get(), m_CasterDepthBuffer.Get(), m_VisibilityBuffer.Get(), m_LODFadeBuffer.Get() })
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
		// This frame's visibility (the view's and the cascades') is the next one's history.
		m_VisibilityParity ^= 1u;
	}

	bool SceneRenderer::EnsureShadowPyramid()
	{
		if (m_ShadowPyramid || !m_ShadowPyramidPipeline || !m_CullShadowPipeline || !m_ShadowMap)
			return true; // Made, or not needed (no GPU-driven path; made with the shadow map)
		const uint32_t size = std::max((m_Specification.ShadowMapSize + c_ShadowPyramidReduction - 1) / c_ShadowPyramidReduction, 1u);
		uint32_t levels = 1;
		while ((size >> levels) > 0)
			levels++;
		nvrhi::TextureDesc desc;
		desc.width = size;
		desc.height = size;
		desc.arraySize = RenderData::c_MaxCascades;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.mipLevels = levels;
		desc.format = nvrhi::Format::R32_FLOAT;
		desc.isUAV = true;
		desc.debugName = m_Specification.DebugName + ".ShadowPyramid";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle pyramid = m_Device->createTexture(desc);
		if (!pyramid)
			return false;
		// A binding set per level, over every layer (a dispatch builds the level of each cascade): level 0 reads the shadow map.
		const nvrhi::ArraySlice layers = RenderData::c_MaxCascades;
		std::vector<nvrhi::BindingSetHandle> sets;
		for (uint32_t level = 0; level < levels; level++)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {
				level == 0 ? nvrhi::BindingSetItem::Texture_SRV(0, m_ShadowMap, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, layers))
					: nvrhi::BindingSetItem::Texture_SRV(0, pyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level - 1, 1, 0, layers)),
				nvrhi::BindingSetItem::Sampler(0, m_DepthPyramidSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, pyramid, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level, 1, 0, layers)),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(DepthPyramidParameters))
			};
			sets.push_back(m_Device->createBindingSet(set, m_DepthPyramidLayout));
			if (!sets.back())
				return false;
		}
		m_ShadowPyramid = pyramid;
		m_ShadowPyramidSize = size;
		m_ShadowPyramidLevels = levels;
		m_ShadowPyramidSets = std::move(sets);
		return true;
	}

	void SceneRenderer::RunShadowOcclusionPhase(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		// The cascades' depth pyramids of their first phase's shadow maps, level by level, every cascade in one dispatch (few
		// dispatches and state changes: MoltenVK's submission costs CPU time per dispatch).
		const bool minimumSampler = Renderer::GetGraphicsDevice().GetInfo().SupportsSamplerMinMax;
		nvrhi::ComputeState pyramidState;
		pyramidState.pipeline = m_ShadowPyramidPipeline;
		glm::uvec2 source(m_Specification.ShadowMapSize);
		glm::uvec2 destination(m_ShadowPyramidSize);
		for (uint32_t level = 0; level < m_ShadowPyramidLevels; level++)
		{
			pyramidState.bindings = { m_ShadowPyramidSets[level] };
			commandList->setComputeState(pyramidState);
			DepthPyramidParameters parameters;
			parameters.SourceSize = source;
			parameters.DestinationSize = destination;
			parameters.UseMinimumSampler = minimumSampler ? 1u : 0u;
			parameters.Reduction = level == 0 ? c_ShadowPyramidReduction : 2u;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch((destination.x + 7) / 8, (destination.y + 7) / 8, m_CascadeCount);
			source = destination;
			destination = glm::max((destination + 1u) / 2u, glm::uvec2(1));
		}

		// The second phase: every caster's visibility in each cascade against its pyramid, and the draws the first phase did
		// not make. The view's second phase builds its own buckets later.
		const RenderData::CullConstants& constants = m_CullConstants;
		nvrhi::ComputeState state;
		state.bindings = { m_CullBindingSet };
		const auto pass = [&](uint32_t mode, nvrhi::IComputePipeline* pipeline, uint64_t items)
		{
			state.pipeline = pipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&mode, sizeof(mode));
			Dispatch(commandList, items);
			CullBarrier(commandList);
		};
		pass(c_CullModeSecondPhase | c_CullModeCount, m_CullShadowPipeline, constants.ProxyCount);
		pass(c_CullModeShadowSecondPhase, m_CullBuildPipeline, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		pass(c_CullModeSecondPhase | c_CullModeWrite, m_CullShadowPipeline, constants.ProxyCount);
	}

	void SceneRenderer::RecordCullReadback(nvrhi::ICommandList* commandList)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const bool feedback = constants.Feedback != 0;
		CounterReadback& readback = m_CounterReadbacks[m_CounterReadbackIndex];
		readback.Readback->Record(commandList, m_CullCounterBuffer, 0, sizeof(RenderData::CullCounters));
		readback.Render = m_RenderCount + 1;
		readback.HasFeedback = false;
		readback.ShadowOcclusion = m_ShadowOcclusionMeasured;
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
			{
				// Calls of at most the device's (or the specification's) draws each.
				const uint32_t maxDraws = GetMaxDrawsPerIndirectCall();
				for (uint32_t first = 0; first < slots;)
				{
					const uint32_t draws = std::min(maxDraws, slots - first);
					commandList->drawIndexedIndirect(offset + first * static_cast<uint32_t>(sizeof(nvrhi::DrawIndexedIndirectArguments)), draws);
					m_Stats.DrawCommands++;
					first += draws;
				}
				break;
			}
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
		state.framebuffer = pass == Pass::Prepass ? GetPrepassFramebuffer() : m_ForwardFramebuffer.Get();
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
			const PipelineKey key { pass, parts.DoubleSided, parts.Mirrored, parts.Geometry == RenderData::c_CullGeometrySkinned, m_MotionFrame };
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

	bool SceneRenderer::ShadowCacheKey::operator==(const ShadowCacheKey& other) const
	{
		const auto sameCamera = [](const SceneCamera& a, const SceneCamera& b)
		{
			return a.View == b.View && a.Projection == b.Projection && a.Position == b.Position && a.Near == b.Near && a.Far == b.Far
				&& a.VerticalFOV == b.VerticalFOV && a.Orthographic == b.Orthographic;
		};
		return sameCamera(Camera, other.Camera) && ViewportSize == other.ViewportSize && LightView == other.LightView && Box == other.Box
			&& CascadeCount == other.CascadeCount && ShadowMapSize == other.ShadowMapSize && LOD.LODThreshold == other.LOD.LODThreshold
			&& LOD.ErrorThreshold == other.LOD.ErrorThreshold && LOD.ViewportHeight == other.LOD.ViewportHeight;
	}

	uint32_t SceneRenderer::GetCascadesReached(const AABB& worldBounds, const glm::vec3& cameraPosition) const
	{
		if (!worldBounds.IsValid())
			return 0;
		// Light view space relative to the camera, as the cascades' squares and the GPU's casters are.
		const AABB lightBounds = worldBounds.Transformed(m_LightView * glm::translate(glm::mat4(1.0f), -cameraPosition));
		uint32_t mask = 0;
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
		{
			const CascadeBox& box = m_Cascades[cascade];
			const float margin = c_ShadowCacheMarginTexels * box.WorldPerTexel;
			if (lightBounds.Max.x < box.RelativeMin.x - margin || lightBounds.Min.x > box.RelativeMax.x + margin
				|| lightBounds.Max.y < box.RelativeMin.y - margin || lightBounds.Min.y > box.RelativeMax.y + margin
				|| lightBounds.Max.z < box.RelativeFarZ - margin)
				continue;
			mask |= 1u << cascade;
		}
		return mask;
	}

	uint32_t SceneRenderer::PrepareShadowCache(const RenderScene& renderScene, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		const uint32_t allCascades = (1u << m_CascadeCount) - 1u;
		// What the cache held until now: a failed frame leaves nothing (only CommitShadowCache restores it).
		const uint32_t valid = m_ShadowCacheValid;
		m_ShadowCacheValid = 0;
		m_ShadowCachePending = true;
		ShadowCacheContent& content = m_ShadowCachePendingContent;
		content = {};
		content.RenderSceneId = renderScene.GetId();
		content.TablesVersion = m_SceneGPU->GetTablesVersion();
		content.PoolContentVersion = Renderer::GetGeometryPool()->GetContentVersion();
		content.TextureUploadVersion = m_SceneGPU->HasAlphaMaskedMaterials() ? Renderer::GetTextureTable().GetUploadVersion() : 0;
		content.ShadowMap = m_ShadowMap.Get();
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
		{
			ShadowCacheKey& key = m_ShadowCachePendingKeys[cascade];
			key.Camera = camera;
			key.ViewportSize = m_ViewportSize;
			key.LightView = m_LightView;
			key.Box = m_Cascades[cascade];
			key.CascadeCount = m_CascadeCount;
			key.ShadowMapSize = m_Specification.ShadowMapSize;
			key.LOD = m_LODCriteria;
		}

		// The proxies that changed since the cache last looked: the cascades their old and new bounds reach are redrawn.
		const uint64_t proxyVersion = renderScene.GetProxyVersion();
		uint32_t dirty = 0;
		m_ShadowCacheChanges.clear();
		if (!m_Specification.CacheShadowCascades || valid == 0 || !(content == m_ShadowCacheContent))
		{
			// Nothing to keep: the changes so far need no following (old bounds start again from the next change).
			dirty = allCascades;
			m_ShadowCacheBounds.clear();
		}
		else if (proxyVersion != m_ShadowCacheProxyVersion && !renderScene.GetProxyChanges(m_ShadowCacheProxyVersion, m_ShadowCacheChanges))
		{
			dirty = allCascades; // More changes than the log keeps
			m_ShadowCacheBounds.clear();
		}
		else
		{
			for (const uint32_t proxy : m_ShadowCacheChanges)
			{
				const bool alive = proxy < renderScene.GetProxyCapacity() && renderScene.GetEntity(proxy) != entt::null;
				const AABB bounds = alive ? renderScene.GetWorldBounds(proxy) : AABB();
				auto it = m_ShadowCacheBounds.find(proxy);
				if (it == m_ShadowCacheBounds.end())
					dirty = allCascades; // Its bounds before the change are not known
				else
					dirty |= GetCascadesReached(it->second, camera.Position);
				dirty |= GetCascadesReached(bounds, camera.Position);
				if (!alive)
				{
					if (it != m_ShadowCacheBounds.end())
						m_ShadowCacheBounds.erase(it);
				}
				else if (it != m_ShadowCacheBounds.end())
					it->second = bounds;
				else if (m_ShadowCacheBounds.size() < c_MaxShadowCacheBounds)
					m_ShadowCacheBounds.emplace(proxy, bounds);
			}
		}
		m_ShadowCacheProxyVersion = proxyVersion;

		uint32_t cached = 0;
		for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
		{
			if ((valid & (1u << cascade)) != 0 && (dirty & (1u << cascade)) == 0 && m_ShadowCachePendingKeys[cascade] == m_ShadowCacheKeys[cascade])
				cached |= 1u << cascade;
		}
		return cached;
	}

	void SceneRenderer::CommitShadowCache()
	{
		if (!m_ShadowCachePending)
			return;
		m_ShadowCachePending = false;
		if (!m_Specification.CacheShadowCascades)
			return;
		// Every cascade's layer now holds what its key describes: drawn by this frame, or kept for the same key.
		m_ShadowCacheKeys = m_ShadowCachePendingKeys;
		m_ShadowCacheContent = m_ShadowCachePendingContent;
		m_ShadowCacheValid = (1u << m_CascadeCount) - 1u;
	}

	void SceneRenderer::DrawGPUShadows(nvrhi::ICommandList* commandList)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const uint32_t cached = m_CachedCascadeMask;
		const uint32_t allCascades = (1u << m_CascadeCount) - 1u;
		if (cached == allCascades)
		{
			// Every layer is kept: no draws, and no occlusion culling of the cascades to measure.
			m_ShadowOcclusionMeasured = false;
			return;
		}
		if (cached == 0)
			commandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::AllSubresources, true, 0.0f, false, 0);
		else
		{
			for (uint32_t cascade = 0; cascade < m_CascadeCount; cascade++)
			{
				if ((cached & (1u << cascade)) == 0)
					commandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::TextureSubresourceSet(0, 1, cascade, 1), true, 0.0f, false, 0);
			}
		}
		DrawGPUShadowBuckets(commandList, constants.ViewBucketCount, constants.SecondPhaseShadowBuckets);
		if (constants.ShadowOcclusion != 0)
		{
			// What the first phase did not draw, found visible against its depth, in the same frame.
			RunShadowOcclusionPhase(commandList);
			DrawGPUShadowBuckets(commandList, constants.SecondPhaseShadowBuckets, constants.FirstLocalBucket);
		}
	}

	void SceneRenderer::DrawGPUShadowBuckets(nvrhi::ICommandList* commandList, uint32_t firstDense, uint32_t endDense)
	{
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		const bool count = GetGPUIndirectPath() == GPUIndirectPath::Count;
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);
		nvrhi::GraphicsState state;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(mapSize, mapSize));
		state.bindings = { m_GPUShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
		state.indexBuffer = nvrhi::IndexBufferBinding { pool.GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
		state.indirectParams = m_CullArgumentBuffer;
		state.indirectCountBuffer = count ? m_CullBucketCountBuffer.Get() : nullptr;
		for (uint32_t dense = firstDense; dense < std::min(endDense, m_CullConstants.BucketCount); dense++)
		{
			const uint32_t bucket = m_CullBuckets[dense];
			const BucketParts parts = DecodeBucket(bucket);
			const uint32_t slots = GetBucketSlots(bucket);
			if ((slots == 0 && !count) || (m_CachedCascadeMask & (1u << parts.Cascade)) != 0)
				continue; // Nothing to draw, or a kept layer
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

	uint32_t SceneRenderer::GetLocalShadowCullMode(uint32_t firstView, uint32_t viewCount)
	{
		ST_CORE_ASSERT(firstView < 256 && viewCount <= RenderData::c_CullMaxLocalShadowViews, "a batch of local shadow views out of range");
		return (firstView << 8) | (viewCount << 16);
	}

	void SceneRenderer::RunLocalShadowCulling(nvrhi::ICommandList* commandList, uint32_t firstView, uint32_t viewCount, bool lastBatch)
	{
		ST_PROFILE_FUNCTION();
		const RenderData::CullConstants& constants = m_CullConstants;
		const auto barrier = [&]() { CullBarrier(commandList); };
		nvrhi::ComputeState state;
		state.bindings = { m_CullBindingSet };
		// The previous batch's counts, draws and arguments were recorded into its draws: cleared for this batch's.
		state.pipeline = m_CullBuildPipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&c_CullModeResetLocal, sizeof(c_CullModeResetLocal));
		const uint64_t localBuckets = constants.BucketCount - constants.FirstLocalBucket;
		Dispatch(commandList, localBuckets * std::max<uint64_t>(constants.LODCapacity, static_cast<uint64_t>(constants.ArgumentsPerBucket) * c_ArgumentWords));
		barrier();
		const uint32_t viewMode = GetLocalShadowCullMode(firstView, viewCount) | c_CullModeLocal;
		const uint32_t countMode = viewMode | c_CullModeCount;
		state.pipeline = m_CullShadowPipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&countMode, sizeof(countMode));
		Dispatch(commandList, constants.ProxyCount);
		barrier();
		state.pipeline = m_CullBuildPipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&c_CullModeLocal, sizeof(c_CullModeLocal));
		Dispatch(commandList, static_cast<uint64_t>(constants.BucketCount) * constants.LODCapacity);
		if (lastBatch && constants.Feedback != 0)
		{
			// Every counting pass ran: the touched slots' feedback, packed.
			state.pipeline = m_CullFeedbackPipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&c_CullModeCount, sizeof(c_CullModeCount));
			Dispatch(commandList, 1 + static_cast<uint64_t>(constants.MeshSlotCount) + constants.MaterialSlotCount);
		}
		barrier();
		const uint32_t writeMode = viewMode | c_CullModeWrite;
		state.pipeline = m_CullShadowPipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&writeMode, sizeof(writeMode));
		Dispatch(commandList, constants.ProxyCount);
		barrier();
	}

	void SceneRenderer::DrawGPULocalShadows(nvrhi::ICommandList* commandList)
	{
		const RenderData::CullConstants& constants = m_CullConstants;
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		const bool count = GetGPUIndirectPath() == GPUIndirectPath::Count;
		const uint32_t views = static_cast<uint32_t>(m_LocalShadowViews.size());
		for (uint32_t first = 0; first < views; first += RenderData::c_CullMaxLocalShadowViews)
		{
			const uint32_t batchViews = std::min(views - first, RenderData::c_CullMaxLocalShadowViews);
			if (first > 0)
				RunLocalShadowCulling(commandList, first, batchViews, first + batchViews == views);
			for (uint32_t place = 0; place < batchViews; place++)
			{
				const LocalShadowDrawView& drawView = m_LocalShadowViews[first + place];
				const ShadowAtlasTile& tile = m_LocalShadowLights[drawView.Light].Views[drawView.Face].Key.Tile;
				ClearLocalShadowTile(commandList, tile);
				nvrhi::GraphicsState state;
				state.framebuffer = m_LocalShadowFramebuffer;
				state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(tile.X), static_cast<float>(tile.X + tile.Size),
					static_cast<float>(tile.Y), static_cast<float>(tile.Y + tile.Size), 0.0f, 1.0f));
				state.bindings = { m_GPUShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
				state.indexBuffer = nvrhi::IndexBufferBinding { pool.GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
				state.indirectParams = m_CullArgumentBuffer;
				state.indirectCountBuffer = count ? m_CullBucketCountBuffer.Get() : nullptr;
				// The view's matrix is its light's slot's (a push constant after the state).
				const uint32_t viewIndex = drawView.Light * RenderData::c_LocalShadowFaces + drawView.Face;
				for (uint32_t dense = constants.FirstLocalBucket; dense < constants.BucketCount; dense++)
				{
					const uint32_t bucket = m_CullBuckets[dense];
					const BucketParts parts = DecodeBucket(bucket);
					const uint32_t slots = GetBucketSlots(bucket);
					if (parts.Cascade != place || (slots == 0 && !count))
						continue;
					const PipelineKey key { parts.AlphaMask ? Pass::LocalShadowMasked : Pass::LocalShadow, parts.DoubleSided, parts.Mirrored,
						parts.Geometry == RenderData::c_CullGeometrySkinned };
					const GeometryArena arena = parts.Geometry == RenderData::c_CullGeometryStatic ? GeometryArena::Static : GeometryArena::Skinned;
					state.pipeline = GetScenePipeline(key);
					state.vertexBuffers = {
						nvrhi::VertexBufferBinding { pool.GetPositionBuffer(arena), 0, 0 },
						nvrhi::VertexBufferBinding { pool.GetAttributeBuffer(arena), 1, 0 }
					};
					if (key.Skinned)
						state.vertexBuffers.push_back(nvrhi::VertexBufferBinding { pool.GetSkinBuffer(), 2, 0 });
					commandList->setGraphicsState(state);
					commandList->setPushConstants(&viewIndex, sizeof(viewIndex));
					DrawGPUBucket(commandList, dense, slots);
				}
			}
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
		stats.SecondPhaseShadowDraws = counters.SecondPhaseShadowDraws;
		stats.LocalShadowInstances = counters.LocalShadowInstances;
		stats.LocalShadowDraws = counters.LocalShadowDraws;
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
		stats.OccludedCasters = counters.OccludedCasters;
		for (size_t lod = 0; lod < stats.InstancesPerLOD.size(); lod++)
			stats.InstancesPerLOD[lod] = counters.InstancesPerLOD[lod];
		if (newest->ShadowOcclusion)
		{
			// The casters it tested: those it drew and those it found hidden (in cascade instances and proxies, alike for meshes
			// of one submesh; one drawn by the first phase and then found hidden counts twice, which only lowers the share).
			const uint64_t tested = static_cast<uint64_t>(counters.ShadowInstances) + counters.OccludedCasters;
			m_ShadowOcclusionPays = counters.OccludedCasters > 0 && counters.OccludedCasters * c_ShadowOcclusionPaysShare >= tested;
		}

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
