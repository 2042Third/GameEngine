#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// Post-processing, from the HDR image (the frame's, or temporal anti-aliasing's resolved image) to the display: automatic
// exposure meters it into the exposure buffer (PostProcess/Histogram.comp, Exposure.comp) or writes the manual exposure
// there, bloom downsamples it into a half-resolution chain and upsamples back (PostProcess/Bloom.comp), tone mapping maps it
// with the exposure, the bloom, the vignette and the TAA's sharpening into the display target (PostProcess/Tonemap.frag),
// or into the LDR target that FXAA reads (PostProcess/FXAA.frag). RenderFrame records RenderPostHDR (exposure, bloom) and
// RenderPostDisplay (tone mapping, FXAA) in the GPU.Post stage, with the text and overlay preparation between them. Post
// passes take their parameters by push constants or buffers of their own, never FrameConstants. The temporal history's own
// post sets (m_TemporalExposureSets, m_TemporalBloomSets, m_TemporalTonemapSets) belong to the TAA's resources
// (SceneRenderer::EnsureTemporalResources).

namespace Strata
{

	namespace
	{

		uint32_t DispatchGroups(uint32_t size)
		{
			return (size + 7) / 8;
		}

		// The levels (2^bits - 1) the output's dither quantizes to in a target of `format` (Include/Dither.glsl): the 8-bit and
		// 10-bit UNORM formats; 0 (no dither) for the others a target may have (16-bit UNORM, the packed 5-6-5, 5-5-5-1 and
		// 4-4-4-4 formats), whose own rounding is left as it is.
		float GetDitherLevels(nvrhi::Format format)
		{
			switch (format)
			{
				case nvrhi::Format::R8_UNORM:
				case nvrhi::Format::RG8_UNORM:
				case nvrhi::Format::RGBA8_UNORM:
				case nvrhi::Format::BGRA8_UNORM:
				case nvrhi::Format::BGRX8_UNORM:
					return 255.0f;
				case nvrhi::Format::R10G10B10A2_UNORM:
					return 1023.0f;
				default:
					return 0.0f;
			}
		}

	}

	void SceneRenderer::CreatePostResources()
	{
		nvrhi::BindingLayoutDesc tonemapLayout;
		tonemapLayout.visibility = nvrhi::ShaderType::Pixel;
		tonemapLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)
		};
		m_TonemapBindingLayout = m_Device->createBindingLayout(tonemapLayout);
		// The tone mapping's parameters outgrew the 128 bytes of push constants NVRHI allows: a volatile constant buffer, written
		// once by every frame that tone-maps. Its versions need no reservation of their own: each such frame writes FrameConstants
		// first, and ReserveFrameConstantVersions starts a frame only while the frames in flight leave room for
		// c_MaxFrameConstantWrites more of those, so at most c_FrameConstantVersions - c_MaxFrameConstantWrites + 1 frames hold a
		// version of this buffer, within its c_FrameConstantVersions.
		static_assert(c_MaxFrameConstantWrites >= 1, "every frame that tone-maps holds a FrameConstants version");
		m_TonemapConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::TonemapConstants))
			.setIsConstantBuffer(true)
			.setIsVolatile(true)
			.setMaxVersions(c_FrameConstantVersions)
			.setDebugName(m_Specification.DebugName + ".TonemapConstants"));

		nvrhi::BindingLayoutDesc exposureLayout;
		exposureLayout.visibility = nvrhi::ShaderType::Compute;
		exposureLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::ExposureParameters))
		};
		m_ExposureBindingLayout = m_Device->createBindingLayout(exposureLayout);
		m_HistogramPipeline = CreateComputePipeline("PostProcess/Histogram.comp", m_ExposureBindingLayout);
		m_ExposurePipeline = CreateComputePipeline("PostProcess/Exposure.comp", m_ExposureBindingLayout);
		m_HistogramBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_HistogramBins * sizeof(uint32_t))
			.setStructStride(sizeof(uint32_t))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::UnorderedAccess)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".LuminanceHistogram"));
		m_ExposureBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_ExposureValues * sizeof(float))
			.setStructStride(sizeof(float))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".Exposure"));
		// The automatic exposure's results come back to choose the pre-exposure and for the statistics: three floats
		// (Include/Exposure.glsl: the adapted log2 luminance, the multiplier, the key's stops).
		for (ExposureReadback& entry : m_ExposureReadbacks)
			entry.Readback = CreateScope<BufferReadback>(m_Device, 3 * sizeof(float), m_Specification.DebugName + ".ExposureReadback");
		const bool exposureReadbacks = std::all_of(m_ExposureReadbacks.begin(), m_ExposureReadbacks.end(),
			[](const ExposureReadback& entry) { return entry.Readback->IsValid(); });

		nvrhi::BindingLayoutDesc bloomLayout;
		bloomLayout.visibility = nvrhi::ShaderType::Compute;
		bloomLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::BloomParameters))
		};
		m_BloomBindingLayout = m_Device->createBindingLayout(bloomLayout);
		m_BloomPipeline = CreateComputePipeline("PostProcess/Bloom.comp", m_BloomBindingLayout);

		nvrhi::BindingLayoutDesc fxaaLayout;
		fxaaLayout.visibility = nvrhi::ShaderType::Pixel;
		fxaaLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::FXAAParameters))
		};
		m_FXAABindingLayout = m_Device->createBindingLayout(fxaaLayout);
		// The overlays' copy of the output texture into a target (PostProcess/Copy.frag).
		nvrhi::BindingLayoutDesc copyLayout;
		copyLayout.visibility = nvrhi::ShaderType::Pixel;
		copyLayout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0) };
		m_CopyBindingLayout = m_Device->createBindingLayout(copyLayout);
		ST_CORE_VERIFY(m_TonemapBindingLayout && m_TonemapConstantBuffer && m_ExposureBindingLayout && m_HistogramPipeline && m_ExposurePipeline && m_HistogramBuffer
			&& m_ExposureBuffer && exposureReadbacks && m_BloomBindingLayout && m_BloomPipeline && m_FXAABindingLayout && m_CopyBindingLayout,
			"SceneRenderer: failed to create the post-processing resources");
	}

	bool SceneRenderer::CreatePostTargets(uint32_t width, uint32_t height, std::string& outFailure)
	{
		const std::string& name = m_Specification.DebugName;
		// Tone mapping writes here when FXAA follows.
		m_LDRTexture = CreateRenderTarget(m_Device, width, height, c_LDRFormat, name + ".LDR", false);
		m_LDRFramebuffer = m_LDRTexture ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_LDRTexture)) : nullptr;
		if (!m_LDRTexture || !m_LDRFramebuffer)
		{
			outFailure = "post-processing targets";
			return false;
		}

		// Automatic exposure meters the HDR image.
		nvrhi::BindingSetDesc exposureSet;
		exposureSet.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_HistogramBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_ExposureBuffer),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::ExposureParameters))
		};
		m_ExposureBindingSet = m_Device->createBindingSet(exposureSet, m_ExposureBindingLayout);

		// Bloom chain at half resolution, down to levels of a few pixels.
		const uint32_t bloomWidth = std::max(1u, (width + 1) / 2);
		const uint32_t bloomHeight = std::max(1u, (height + 1) / 2);
		uint32_t bloomLevels = 1;
		while (bloomLevels < RenderData::c_MaxBloomLevels && (std::min(bloomWidth, bloomHeight) >> bloomLevels) >= 4)
			bloomLevels++;
		nvrhi::TextureDesc bloomDesc;
		bloomDesc.width = bloomWidth;
		bloomDesc.height = bloomHeight;
		bloomDesc.mipLevels = bloomLevels;
		bloomDesc.format = c_HDRFormat;
		bloomDesc.isUAV = true;
		bloomDesc.debugName = name + ".Bloom";
		bloomDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		bloomDesc.keepInitialState = true;
		m_BloomTexture = m_Device->createTexture(bloomDesc);
		if (!m_ExposureBindingSet || !m_BloomTexture)
		{
			outFailure = "post-processing targets";
			return false;
		}
		m_BloomLevels = bloomLevels;

		auto createBloomSet = [&](nvrhi::ITexture* source, uint32_t sourceLevel, uint32_t destinationLevel)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(sourceLevel, 1, 0, 1)),
				nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(destinationLevel, 1, 0, 1)),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_ExposureBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::BloomParameters))
			};
			return m_Device->createBindingSet(desc, m_BloomBindingLayout);
		};
		m_BloomDownsampleSets.push_back(createBloomSet(m_HDRTexture, 0, 0));
		for (uint32_t level = 1; level < m_BloomLevels; level++)
			m_BloomDownsampleSets.push_back(createBloomSet(m_BloomTexture, level - 1, level));
		for (uint32_t level = 0; level + 1 < m_BloomLevels; level++)
			m_BloomUpsampleSets.push_back(createBloomSet(m_BloomTexture, level + 1, level));
		for (const std::vector<nvrhi::BindingSetHandle>* sets : { &m_BloomDownsampleSets, &m_BloomUpsampleSets })
		{
			for (const nvrhi::BindingSetHandle& set : *sets)
			{
				if (!set)
				{
					outFailure = "bloom bindings";
					return false;
				}
			}
		}

		nvrhi::BindingSetDesc tonemapSet;
		tonemapSet.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
			nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler),
			nvrhi::BindingSetItem::Texture_SRV(1, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
			nvrhi::BindingSetItem::Sampler(1, m_LinearClampSampler),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_ExposureBuffer),
			nvrhi::BindingSetItem::ConstantBuffer(0, m_TonemapConstantBuffer)
		};
		m_TonemapBindingSet = m_Device->createBindingSet(tonemapSet, m_TonemapBindingLayout);
		nvrhi::BindingSetDesc fxaaSet;
		fxaaSet.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_LDRTexture),
			nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::FXAAParameters))
		};
		m_FXAABindingSet = m_Device->createBindingSet(fxaaSet, m_FXAABindingLayout);
		if (!m_TonemapBindingSet || !m_FXAABindingSet)
		{
			outFailure = "post-processing bindings";
			return false;
		}
		return true;
	}

	void SceneRenderer::ReleasePostTargets(std::vector<nvrhi::ResourceHandle>& released)
	{
		auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		release(m_ExposureBindingSet);
		release(m_BloomTexture);
		m_BloomLevels = 0;
		for (std::vector<nvrhi::BindingSetHandle>* sets : { &m_BloomDownsampleSets, &m_BloomUpsampleSets })
		{
			for (nvrhi::BindingSetHandle& set : *sets)
				release(set);
			sets->clear();
		}
		release(m_TonemapBindingSet);
		release(m_LDRTexture);
		release(m_LDRFramebuffer);
		release(m_FXAABindingSet);
	}

	void SceneRenderer::UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, const SceneRenderOptions& options,
		nvrhi::IBindingSet* bindingSet, std::optional<float> keyReferenceLog2)
	{
		const auto now = std::chrono::steady_clock::now();
		// Rendering pauses (breakpoints, minimized windows) must not turn into one huge adaptation step. Tools that step time
		// give the time instead.
		float deltaTime = m_LastExposureTime ? std::clamp(std::chrono::duration<float>(now - *m_LastExposureTime).count(), 0.0f, 0.25f) : 0.0f;
		if (options.AdaptationSeconds)
			deltaTime = std::isfinite(*options.AdaptationSeconds) ? std::max(*options.AdaptationSeconds, 0.0f) : 0.0f;
		m_LastExposureTime = now;
		const float compensation = std::exp2(std::clamp(settings.Exposure, -20.0f, 20.0f));

		if (!settings.AutoExposure)
		{
			const float values[RenderData::c_ExposureValues] = { 0.0f, compensation, 0.0f, 0.0f };
			commandList->writeBuffer(m_ExposureBuffer, values, sizeof(values));
			m_ResetExposure = true; // Automatic exposure starts from the scene's brightness when turned on
			return;
		}

		auto adapt = [&](float speed) { return m_ResetExposure || !(speed > 0.0f) ? 1.0f : 1.0f - std::exp(-deltaTime * std::min(speed, 1000.0f)); };
		RenderData::ExposureParameters parameters = {};
		parameters.MinLog2 = std::clamp(settings.AutoExposureMinEV, RenderData::c_HistogramMinLog2, RenderData::c_HistogramMaxLog2);
		parameters.MaxLog2 = std::clamp(settings.AutoExposureMaxEV, parameters.MinLog2, RenderData::c_HistogramMaxLog2);
		// Light adaptation (toward a brighter scene) and dark adaptation (toward a darker one) at their own speeds.
		parameters.AdaptLight = adapt(settings.AutoExposureLightSpeed);
		parameters.AdaptDark = adapt(settings.AutoExposureSpeed);
		parameters.Compensation = compensation;
		// The metering window: the darkest and brightest pixels (deep shadows, light sources) do not move the exposure. At least
		// 0.02 of the pixels stay inside it.
		const float low = std::isfinite(settings.AutoExposureLowPercentile) ? std::clamp(settings.AutoExposureLowPercentile, 0.0f, 0.49f) : 0.1f;
		const float high = std::isfinite(settings.AutoExposureHighPercentile) ? std::clamp(settings.AutoExposureHighPercentile, 0.51f, 1.0f) : 0.9f;
		parameters.LowPercentile = low;
		parameters.HighPercentile = std::max(high, low + 0.02f);
		parameters.InversePreExposure = 1.0f / m_PreExposure;
		// The exposure key compares the metered average with a gray card facing the directional light; without one there is none.
		const float key = std::isfinite(settings.AutoExposureKey) ? std::clamp(settings.AutoExposureKey, 0.0f, 0.5f) : 0.0f;
		parameters.KeyStrength = keyReferenceLog2 ? key : 0.0f;
		parameters.KeyReferenceLog2 = keyReferenceLog2.value_or(0.0f);
		parameters.KeyMinStops = RenderData::c_ExposureKeyMinStops;
		parameters.KeyMaxStops = RenderData::c_ExposureKeyMaxStops;
		parameters.MeteringAspect = static_cast<float>(m_ViewportSize.x) / static_cast<float>(std::max(m_ViewportSize.y, 1u));
		parameters.Metering = static_cast<uint32_t>(settings.AutoExposureMetering);
		m_ResetExposure = false;

		commandList->clearBufferUInt(m_HistogramBuffer, 0);
		nvrhi::ComputeState state;
		state.pipeline = m_HistogramPipeline;
		state.bindings = { bindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch((m_ViewportSize.x + 15) / 16, (m_ViewportSize.y + 15) / 16, 1);

		// NVRHI places barriers only when the binding sets change: the histogram must be complete before it is read.
		commandList->setBufferState(m_HistogramBuffer, nvrhi::ResourceStates::UnorderedAccess);
		commandList->commitBarriers();
		state.pipeline = m_ExposurePipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(1, 1, 1);

		// The metered exposure comes back frames later to choose the pre-exposure (ChoosePreExposure). A readback whose earlier
		// copy is still under way is left to finish: the frame's is skipped.
		ExposureReadback& readback = m_ExposureReadbacks[m_ExposureReadbackIndex];
		if (readback.Readback->IsPending() && !readback.Readback->IsReady())
			return;
		readback.Readback->Record(commandList, m_ExposureBuffer, 0, 3 * sizeof(float));
		readback.Render = m_RenderCount + 1;
		readback.Epoch = m_PreExposureEpoch;
		readback.PreExposure = m_PreExposure;
		m_ExposureReadbackRecorded = true;
	}

	void SceneRenderer::SubmitExposureReadback()
	{
		if (!m_ExposureReadbackRecorded)
			return;
		m_ExposureReadbacks[m_ExposureReadbackIndex].Readback->Submit();
		m_ExposureReadbackIndex = (m_ExposureReadbackIndex + 1) % c_GPURecordCount;
		m_ExposureReadbackRecorded = false;
	}

	float SceneRenderer::GetBloomSpan(float radius, uint32_t height, uint32_t levels)
	{
		// The deepest level's texel spans 2^span output pixels: BloomRadius times the height. A span within 1e-4 of a whole
		// number is that number, so the default (64 / 1080) gives 1080 rows exactly six levels.
		const float safeRadius = std::isfinite(radius) ? std::clamp(radius, 0.01f, 0.25f) : 64.0f / 1080.0f;
		float span = std::log2(safeRadius * static_cast<float>(std::max(height, 1u)));
		if (std::abs(span - std::round(span)) < 1e-4f)
			span = std::round(span);
		return std::clamp(span, 1.0f, static_cast<float>(std::max(levels, 1u)));
	}

	void SceneRenderer::RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* prefilterSet)
	{
		RenderData::BloomParameters parameters = {};
		parameters.Threshold = std::max(settings.BloomThreshold, 0.0f);
		parameters.Knee = parameters.Threshold * 0.5f;
		parameters.InversePreExposure = 1.0f / m_PreExposure;
		// The levels the radius asks for: ceil(span), the deepest weighted by the fraction of it the span covers.
		m_BloomSpan = GetBloomSpan(settings.BloomRadius, m_ViewportSize.y, m_BloomLevels);
		const uint32_t levels = static_cast<uint32_t>(std::ceil(m_BloomSpan));
		const float deepestWeight = m_BloomSpan - static_cast<float>(levels - 1);
		m_Stats.BloomLevels = levels;

		const nvrhi::TextureDesc& desc = m_BloomTexture->getDesc();
		auto dispatch = [&](nvrhi::IBindingSet* bindingSet, uint32_t mode, uint32_t level, float upsampleWeight)
		{
			nvrhi::ComputeState state;
			state.pipeline = m_BloomPipeline;
			state.bindings = { bindingSet };
			commandList->setComputeState(state);
			parameters.Mode = mode;
			parameters.UpsampleWeight = upsampleWeight;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(DispatchGroups(std::max(1u, desc.width >> level)), DispatchGroups(std::max(1u, desc.height >> level)), 1);
		};

		dispatch(prefilterSet, RenderData::c_BloomPrefilter, 0, 1.0f);
		for (uint32_t level = 1; level < levels; level++)
			dispatch(m_BloomDownsampleSets[level], RenderData::c_BloomDownsample, level, 1.0f);
		for (uint32_t level = levels - 1; level-- > 0;)
			dispatch(m_BloomUpsampleSets[level], RenderData::c_BloomUpsample, level, level + 2 == levels ? deepestWeight : 1.0f);
	}

	bool SceneRenderer::RenderPostHDR(nvrhi::ICommandList* commandList, const PostProcessComponent& postProcess, const SceneRenderOptions& options, bool resolved,
		std::optional<float> keyReferenceLog2)
	{
		commandList->beginMarker("Exposure");
		UpdateExposure(commandList, postProcess, options, resolved ? m_TemporalExposureSets[m_HistoryIndex].Get() : m_ExposureBindingSet.Get(), keyReferenceLog2);
		commandList->endMarker();
		const bool bloom = postProcess.Bloom && postProcess.BloomIntensity > 0.0f;
		if (bloom)
		{
			commandList->beginMarker("Bloom");
			RenderBloom(commandList, postProcess, resolved ? m_TemporalBloomSets[m_HistoryIndex].Get() : m_BloomDownsampleSets[0].Get());
			commandList->endMarker();
		}
		return bloom;
	}

	glm::vec2 SceneRenderer::GetVignetteTangents(const SceneCamera& camera, glm::uvec2 viewportSize)
	{
		// Orthographic views have no field angle: they vignette as a 60 degree view of their aspect.
		const float aspect = static_cast<float>(viewportSize.x) / static_cast<float>(std::max(viewportSize.y, 1u));
		if (camera.Orthographic || camera.Projection[0][0] == 0.0f || camera.Projection[1][1] == 0.0f)
			return glm::vec2(aspect, 1.0f) * std::tan(glm::radians(30.0f));
		return glm::vec2(1.0f / std::abs(camera.Projection[0][0]), 1.0f / std::abs(camera.Projection[1][1]));
	}

	void SceneRenderer::RenderPostDisplay(nvrhi::ICommandList* commandList, const PostProcessComponent& postProcess, const TemporalAntiAliasingSettings& temporalSettings,
		bool resolved, bool bloom, const glm::vec2& vignetteTangents, nvrhi::IFramebuffer* output)
	{
		// Tone mapping into the display target, or into the intermediate target FXAA reads.
		RenderData::TonemapConstants tonemap = {};
		tonemap.BloomIntensity = bloom ? std::min(postProcess.BloomIntensity, 1.0f) : 0.0f;
		// Every level weighs the same (an octave each): the sum is normalized by the span of levels used.
		tonemap.BloomScale = 1.0f / m_BloomSpan;
		tonemap.Saturation = std::max(postProcess.Saturation, 0.0f);
		tonemap.Contrast = std::clamp(postProcess.Contrast, 0.1f, 4.0f);
		tonemap.Operator = static_cast<int32_t>(postProcess.Tonemapper);
		tonemap.BloomAdditive = postProcess.BloomThreshold > 0.0f ? 1u : 0u;
		tonemap.InversePreExposure = 1.0f / m_PreExposure;
		// Only the TAA's image is sharpened (it softens texture detail), and only on the way to the display: frames with FXAA, or
		// without TAA, skip the branch and stay as they were.
		tonemap.Sharpening = resolved ? temporalSettings.Sharpening : 0.0f;
		tonemap.Vignette = std::isfinite(postProcess.Vignette) ? std::clamp(postProcess.Vignette, 0.0f, 1.0f) : 0.0f;
		tonemap.VignetteTangents = vignetteTangents;
		const PostProcessMath::FilmicCurve filmic = PostProcessMath::MakeFilmicCurve(postProcess.Toe, postProcess.Shoulder);
		tonemap.FilmicToeSlope = filmic.ToeSlope;
		tonemap.FilmicWhiteStops = filmic.WhiteStops;
		tonemap.FilmicShoulderPower = filmic.ShoulderPower;
		const PostProcessMath::ColorGrading grading = GetColorGrading(postProcess);
		tonemap.ColorFlags = grading.Active ? RenderData::c_TonemapColorGrading : 0u;
		for (int row = 0; row < 3; row++)
			tonemap.ColorRows[row] = glm::vec4(grading.Matrix[0][row], grading.Matrix[1][row], grading.Matrix[2][row], 0.0f);
		tonemap.ColorOffset = glm::vec4(grading.Offset, 0.0f);
		tonemap.ColorPower = glm::vec4(grading.Power, 0.0f);

		// FXAA unless the image was resolved temporally.
		const bool fxaa = postProcess.AntiAliasing && !resolved;
		nvrhi::IFramebuffer* tonemapTarget = fxaa ? m_LDRFramebuffer.Get() : output;
		// The last pass that writes the display image dithers it, once: FXAA when it follows (the tone mapping's 10-bit image
		// stays undithered), else the tone mapping.
		const float outputLevels = m_Specification.DitherOutput ? GetDitherLevels(output->getFramebufferInfo().colorFormats[0]) : 0.0f;
		tonemap.DitherLevels = fxaa ? 0.0f : outputLevels;
		m_Stats.Dithered = outputLevels > 0.0f;
		commandList->beginMarker("Tonemap");
		ST_CORE_ASSERT(!m_TonemapConstantsWritten, "SceneRenderer: a frame writes the tone mapping's constants once");
		commandList->writeBuffer(m_TonemapConstantBuffer, &tonemap, sizeof(tonemap));
		m_TonemapConstantsWritten = true;
		DrawFullscreen(commandList, GetFullscreenPipeline(m_TonemapPipelines, m_TonemapPixelShader, m_TonemapBindingLayout, tonemapTarget->getFramebufferInfo()),
			tonemapTarget, resolved ? m_TemporalTonemapSets[m_HistoryIndex].Get() : m_TonemapBindingSet.Get(), nullptr, 0);
		commandList->endMarker();
		if (fxaa)
		{
			RenderData::FXAAParameters parameters = {};
			parameters.DitherLevels = outputLevels;
			commandList->beginMarker("FXAA");
			DrawFullscreen(commandList, GetFullscreenPipeline(m_FXAAPipelines, m_FXAAPixelShader, m_FXAABindingLayout, output->getFramebufferInfo()), output, m_FXAABindingSet,
				&parameters, sizeof(parameters));
			commandList->endMarker();
		}
	}

	PostProcessMath::ColorGrading SceneRenderer::GetColorGrading(const PostProcessComponent& postProcess)
	{
		// The white balance's matrix is computed in double: kept while the temperature and tint stay.
		const glm::vec2 key(postProcess.WhiteBalanceTemperature, postProcess.WhiteBalanceTint);
		if (key != m_GradingKey || !m_Grading)
		{
			m_Grading = PostProcessMath::MakeColorGrading(key.x, key.y, glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f));
			m_GradingKey = key;
		}
		PostProcessMath::ColorGrading grading = PostProcessMath::MakeColorGrading(6500.0f, 0.0f, postProcess.ColorSlope, postProcess.ColorOffset, postProcess.ColorPower);
		grading.Matrix = grading.Matrix * m_Grading->Matrix;
		grading.Active = grading.Active || m_Grading->Active;
		return grading;
	}

	void SceneRenderer::DrawFullscreen(nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindingSet,
		const void* pushConstants, size_t pushConstantSize)
	{
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { bindingSet };
		commandList->setGraphicsState(state);
		if (pushConstants)
			commandList->setPushConstants(pushConstants, pushConstantSize);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

}
