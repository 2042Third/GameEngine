#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/GraphicsDevice.h"
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
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::TonemapParameters))
		};
		m_TonemapBindingLayout = m_Device->createBindingLayout(tonemapLayout);

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
			.setByteSize(4 * sizeof(float))
			.setStructStride(sizeof(float))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".Exposure"));
		// The automatic exposure's results come back to choose the pre-exposure: two floats (Include/Exposure.glsl).
		for (ExposureReadback& entry : m_ExposureReadbacks)
			entry.Readback = CreateScope<BufferReadback>(m_Device, 2 * sizeof(float), m_Specification.DebugName + ".ExposureReadback");
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
		fxaaLayout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0) };
		m_FXAABindingLayout = m_Device->createBindingLayout(fxaaLayout);
		ST_CORE_VERIFY(m_TonemapBindingLayout && m_ExposureBindingLayout && m_HistogramPipeline && m_ExposurePipeline && m_HistogramBuffer && m_ExposureBuffer
			&& exposureReadbacks && m_BloomBindingLayout && m_BloomPipeline && m_FXAABindingLayout, "SceneRenderer: failed to create the post-processing resources");
	}

	bool SceneRenderer::CreatePostTargets(uint32_t width, uint32_t height, std::string& outFailure)
	{
		const std::string& name = m_Specification.DebugName;
		// Tone mapping writes here when FXAA follows.
		m_LDRTexture = CreateRenderTarget(m_Device, width, height, c_OutputFormat, name + ".LDR", false);
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
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::TonemapParameters))
		};
		m_TonemapBindingSet = m_Device->createBindingSet(tonemapSet, m_TonemapBindingLayout);
		nvrhi::BindingSetDesc fxaaSet;
		fxaaSet.bindings = { nvrhi::BindingSetItem::Texture_SRV(0, m_LDRTexture), nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler) };
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

	void SceneRenderer::UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* bindingSet)
	{
		const auto now = std::chrono::steady_clock::now();
		// Rendering pauses (breakpoints, minimized windows) must not turn into one huge adaptation step.
		const float deltaTime = m_LastExposureTime ? std::clamp(std::chrono::duration<float>(now - *m_LastExposureTime).count(), 0.0f, 0.25f) : 0.0f;
		m_LastExposureTime = now;
		const float compensation = std::exp2(std::clamp(settings.Exposure, -20.0f, 20.0f));

		if (!settings.AutoExposure)
		{
			const float values[4] = { 0.0f, compensation, 0.0f, 0.0f };
			commandList->writeBuffer(m_ExposureBuffer, values, sizeof(values));
			m_ResetExposure = true; // Automatic exposure starts from the scene's brightness when turned on
			return;
		}

		RenderData::ExposureParameters parameters = {};
		parameters.MinLog2 = std::clamp(settings.AutoExposureMinEV, RenderData::c_HistogramMinLog2, RenderData::c_HistogramMaxLog2);
		parameters.MaxLog2 = std::clamp(settings.AutoExposureMaxEV, parameters.MinLog2, RenderData::c_HistogramMaxLog2);
		parameters.AdaptFactor = m_ResetExposure || settings.AutoExposureSpeed <= 0.0f ? 1.0f : 1.0f - std::exp(-deltaTime * settings.AutoExposureSpeed);
		parameters.Compensation = compensation;
		// The darkest and brightest 10% of the pixels (deep shadows, light sources) do not move the exposure.
		parameters.LowPercentile = 0.1f;
		parameters.HighPercentile = 0.9f;
		parameters.InversePreExposure = 1.0f / m_PreExposure;
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
		readback.Readback->Record(commandList, m_ExposureBuffer, 0, 2 * sizeof(float));
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

	void SceneRenderer::RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* prefilterSet)
	{
		RenderData::BloomParameters parameters = {};
		parameters.Threshold = std::max(settings.BloomThreshold, 0.0f);
		parameters.Knee = parameters.Threshold * 0.5f;
		parameters.InversePreExposure = 1.0f / m_PreExposure;

		const nvrhi::TextureDesc& desc = m_BloomTexture->getDesc();
		auto dispatch = [&](nvrhi::IBindingSet* bindingSet, uint32_t mode, uint32_t level)
		{
			nvrhi::ComputeState state;
			state.pipeline = m_BloomPipeline;
			state.bindings = { bindingSet };
			commandList->setComputeState(state);
			parameters.Mode = mode;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(DispatchGroups(std::max(1u, desc.width >> level)), DispatchGroups(std::max(1u, desc.height >> level)), 1);
		};

		dispatch(prefilterSet, RenderData::c_BloomPrefilter, 0);
		for (uint32_t level = 1; level < m_BloomLevels; level++)
			dispatch(m_BloomDownsampleSets[level], RenderData::c_BloomDownsample, level);
		for (uint32_t level = m_BloomLevels - 1; level-- > 0;)
			dispatch(m_BloomUpsampleSets[level], RenderData::c_BloomUpsample, level);
	}

	bool SceneRenderer::RenderPostHDR(nvrhi::ICommandList* commandList, const PostProcessComponent& postProcess, bool resolved)
	{
		commandList->beginMarker("Exposure");
		UpdateExposure(commandList, postProcess, resolved ? m_TemporalExposureSets[m_HistoryIndex].Get() : m_ExposureBindingSet.Get());
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

	void SceneRenderer::RenderPostDisplay(nvrhi::ICommandList* commandList, const PostProcessComponent& postProcess, const TemporalAntiAliasingSettings& temporalSettings,
		bool resolved, bool bloom, nvrhi::IFramebuffer* output)
	{
		// Tone mapping into the display target, or into the intermediate target FXAA reads.
		RenderData::TonemapParameters tonemap = {};
		tonemap.BloomIntensity = bloom ? std::min(postProcess.BloomIntensity, 1.0f) : 0.0f;
		tonemap.BloomScale = 1.0f / static_cast<float>(m_BloomLevels);
		tonemap.Saturation = std::max(postProcess.Saturation, 0.0f);
		tonemap.Vignette = std::clamp(postProcess.Vignette, 0.0f, 1.0f);
		tonemap.Contrast = std::clamp(postProcess.Contrast, 0.1f, 4.0f);
		tonemap.Operator = static_cast<int32_t>(postProcess.Tonemapper);
		tonemap.BloomAdditive = postProcess.BloomThreshold > 0.0f ? 1u : 0u;
		tonemap.InversePreExposure = 1.0f / m_PreExposure;
		// Only the TAA's image is sharpened (it softens texture detail), and only on the way to the display: frames with FXAA, or
		// without TAA, skip the branch and stay as they were.
		tonemap.Sharpening = resolved ? temporalSettings.Sharpening : 0.0f;

		// FXAA unless the image was resolved temporally.
		const bool fxaa = postProcess.AntiAliasing && !resolved;
		nvrhi::IFramebuffer* tonemapTarget = fxaa ? m_LDRFramebuffer.Get() : output;
		commandList->beginMarker("Tonemap");
		DrawFullscreen(commandList, GetFullscreenPipeline(m_TonemapPipelines, m_TonemapPixelShader, m_TonemapBindingLayout, tonemapTarget->getFramebufferInfo()),
			tonemapTarget, resolved ? m_TemporalTonemapSets[m_HistoryIndex].Get() : m_TonemapBindingSet.Get(), &tonemap, sizeof(tonemap));
		commandList->endMarker();
		if (fxaa)
		{
			commandList->beginMarker("FXAA");
			DrawFullscreen(commandList, GetFullscreenPipeline(m_FXAAPipelines, m_FXAAPixelShader, m_FXAABindingLayout, output->getFramebufferInfo()), output, m_FXAABindingSet, nullptr, 0);
			commandList->endMarker();
		}
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
