#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

// The lighting debug views (SceneRenderOptions::LightingView). The forward pass shades with the view (MaterialParams.y);
// then, in the GPU.Overlays stage, Effects/LightingViewStats.comp counts the frame's HDR target into histograms that come back
// through a ring of readbacks frames later (GetLightingViewStats), and for the value and luminance views
// Overlay/LightingView.frag replaces the finished image with the view's colors, first among the overlays. Nothing is made
// before the first frame of a view; the pipelines then stay, the statistics buffer and readbacks go with the targets.

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_StatsGroupSize = 16;

		uint32_t PackDisplayColor(const glm::vec4& color)
		{
			const auto channel = [](float value, uint32_t shift)
			{
				const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
				return static_cast<uint32_t>(std::lround(clamped * 255.0f)) << shift;
			};
			return channel(color.r, 0) | channel(color.g, 8) | channel(color.b, 16) | channel(color.a, 24);
		}

		float GetLuminance(const glm::vec3& color)
		{
			return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
		}

		// The value below which `fraction` of a histogram's counts lie, linear within the bin it falls in: bin `index` covers
		// [first + index * width, first + (index + 1) * width). NaN for an empty histogram.
		float GetPercentile(std::span<const uint32_t> bins, float fraction, float first, float width)
		{
			uint64_t total = 0;
			for (uint32_t count : bins)
				total += count;
			if (total == 0)
				return std::numeric_limits<float>::quiet_NaN();
			const double target = static_cast<double>(fraction) * static_cast<double>(total);
			uint64_t below = 0;
			for (size_t index = 0; index < bins.size(); index++)
			{
				if (bins[index] == 0)
					continue;
				if (static_cast<double>(below + bins[index]) >= target)
				{
					const double within = std::clamp((target - static_cast<double>(below)) / static_cast<double>(bins[index]), 0.0, 1.0);
					return first + (static_cast<float>(index) + static_cast<float>(within)) * width;
				}
				below += bins[index];
			}
			return first + static_cast<float>(bins.size()) * width;
		}

	}

	bool SceneRenderer::IsLightingValueView(SceneLightingView view)
	{
		return view == SceneLightingView::AmbientOcclusion || view == SceneLightingView::Albedo;
	}

	void SceneRenderer::BeginLightingViewFrame(SceneLightingView view)
	{
		if (view == m_LightingView)
			return;
		// The temporal history must not blend a view's image with the lit one (or another view's).
		ResetHistory();
		const bool wasValueView = IsLightingValueView(m_LightingView);
		const bool valueView = IsLightingValueView(view);
		// Readbacks of the exposure recorded on either side of the switch metered other values: none is followed.
		if (wasValueView != valueView)
			m_PreExposureEpoch++;
		// The lit image returns at once, without adapting from the values the view metered.
		if (wasValueView && !valueView)
			JumpExposureAdaptation();
		m_LightingView = view;
		m_LightingViewChosen = m_RenderCount + 1;
		m_LightingViewStats.reset();
	}

	bool SceneRenderer::RecordLightingView(nvrhi::ICommandList* commandList, const SceneRenderOptions& options, const RenderData::FrameConstants& frame,
		const nvrhi::Viewport& viewport)
	{
		const SceneLightingView view = options.LightingView;
		if (!m_LightingStatsPipeline)
		{
			nvrhi::BindingLayoutDesc statsLayout;
			statsLayout.visibility = nvrhi::ShaderType::Compute;
			statsLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Texture_SRV(1),
				nvrhi::BindingLayoutItem::Texture_SRV(3),
				nvrhi::BindingLayoutItem::Sampler(8),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::LightingViewParameters))
			};
			m_LightingStatsLayout = m_Device->createBindingLayout(statsLayout);
			ST_CORE_VERIFY(m_LightingStatsLayout, "SceneRenderer: failed to create the lighting view's statistics layout");
			m_LightingStatsPipeline = CreateComputePipeline("Effects/LightingViewStats.comp", m_LightingStatsLayout);
			ST_CORE_VERIFY(m_LightingStatsPipeline, "SceneRenderer: failed to create the lighting view's statistics pipeline");

			nvrhi::BindingLayoutDesc overlayLayout;
			overlayLayout.visibility = nvrhi::ShaderType::Pixel;
			overlayLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Texture_SRV(1),
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::LightingViewParameters))
			};
			m_LightingOverlayLayout = m_Device->createBindingLayout(overlayLayout);
			nvrhi::IShader* pixelShader = Renderer::GetShaderLibrary().Get("Overlay/LightingView.frag");
			ST_CORE_VERIFY(pixelShader && m_LightingOverlayLayout, "SceneRenderer: the lighting view shader is missing");
			// Replaces the image without blending; it reads the depth itself, so no depth attachment is tested.
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = pixelShader;
			desc.bindingLayouts = { m_LightingOverlayLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			m_LightingOverlayPipeline = m_Device->createGraphicsPipeline(desc, m_OutputFramebuffer->getFramebufferInfo());
			ST_CORE_VERIFY(m_LightingOverlayPipeline, "SceneRenderer: failed to create the lighting view pipeline");
		}

		if (!m_LightingStatsBuffer)
		{
			m_LightingStatsBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
				.setByteSize(sizeof(RenderData::LightingViewStatsData))
				.setStructStride(sizeof(uint32_t))
				.setCanHaveUAVs(true)
				.setInitialState(nvrhi::ResourceStates::UnorderedAccess)
				.setKeepInitialState(true)
				.setDebugName(m_Specification.DebugName + ".LightingViewStats"));
			for (uint32_t index = 0; index < c_LightingViewReadbacks; index++)
			{
				m_LightingViewReadbacks[index].Readback = CreateScope<BufferReadback>(m_Device, sizeof(RenderData::LightingViewStatsData),
					fmt::format("{}.LightingViewStatsReadback{}", m_Specification.DebugName, index));
			}
			m_LightingViewReadbackIndex = 0;
			const bool readbacks = std::all_of(m_LightingViewReadbacks.begin(), m_LightingViewReadbacks.end(),
				[](const LightingViewReadback& entry) { return entry.Readback->IsValid(); });
			if (!m_LightingStatsBuffer || !readbacks)
			{
				std::vector<nvrhi::ResourceHandle> released;
				ReleaseLightingViewResources(released);
				Renderer::ReleaseDeferred(std::move(released));
				m_Errors.Report("failed to create the lighting view's statistics buffer");
				return false;
			}
		}

		nvrhi::ITexture* irradiance = GetIrradianceMap() ? GetIrradianceMap() : Renderer::GetBlackCubeTexture();
		if (!m_LightingStatsBindingSet || m_LightingStatsIrradiance != irradiance)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
				nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(3, irradiance),
				nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_LightingStatsBuffer),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::LightingViewParameters))
			};
			m_LightingStatsBindingSet = m_Device->createBindingSet(desc, m_LightingStatsLayout);
			m_LightingStatsIrradiance = m_LightingStatsBindingSet ? irradiance : nullptr;
		}
		if (!m_LightingOverlayBindingSet)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
				nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::LightingViewParameters))
			};
			m_LightingOverlayBindingSet = m_Device->createBindingSet(desc, m_LightingOverlayLayout);
		}
		if (!m_LightingStatsBindingSet || !m_LightingOverlayBindingSet)
		{
			m_Errors.Report("failed to create the lighting view's bindings");
			return false;
		}

		RenderData::LightingViewParameters parameters = {};
		parameters.View = static_cast<uint32_t>(view);
		parameters.InversePreExposure = 1.0f / m_PreExposure;
		parameters.LuminanceFirstEV100 = std::isfinite(options.LightingViewColors.LuminanceFirstEV100) ? options.LightingViewColors.LuminanceFirstEV100 : 0.0f;
		for (uint32_t index = 0; index < RenderData::c_LightingViewLuminanceColors; index++)
			parameters.LuminanceColors[index] = PackDisplayColor(options.LightingViewColors.Luminance[index]);
		parameters.AlbedoLow = PackDisplayColor(options.LightingViewColors.AlbedoLow);
		parameters.AlbedoHigh = PackDisplayColor(options.LightingViewColors.AlbedoHigh);
		parameters.Background = PackDisplayColor(options.LightingViewColors.Background);

		// The statistics: a readback whose earlier copy is still under way is left to finish, and the frame measures nothing.
		LightingViewReadback& readback = m_LightingViewReadbacks[m_LightingViewReadbackIndex];
		if (!readback.Readback->IsPending() || readback.Readback->IsReady())
		{
			commandList->clearBufferUInt(m_LightingStatsBuffer, 0);
			nvrhi::ComputeState state;
			state.pipeline = m_LightingStatsPipeline;
			state.bindings = { m_LightingStatsBindingSet };
			commandList->setComputeState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch((m_ViewportSize.x + c_StatsGroupSize - 1) / c_StatsGroupSize, (m_ViewportSize.y + c_StatsGroupSize - 1) / c_StatsGroupSize, 1);
			readback.Readback->Record(commandList, m_LightingStatsBuffer, 0, sizeof(RenderData::LightingViewStatsData));
			readback.Render = m_RenderCount + 1;
			readback.View = view;
			// The directional light at the ground (its color is multiplied by the atmosphere's transmittance) on a surface facing
			// up, before shadows.
			readback.SunIlluminance = frame.DirectionalLightDirection.w > 0.0f
				? std::max(GetLuminance(glm::vec3(frame.DirectionalLightColor)), 0.0f) * std::max(-frame.DirectionalLightDirection.y, 0.0f)
				: 0.0f;
			m_LightingViewReadbackRecorded = true;
		}

		if (view == SceneLightingView::AmbientOcclusion || view == SceneLightingView::Albedo || view == SceneLightingView::Luminance)
		{
			nvrhi::GraphicsState state;
			state.pipeline = m_LightingOverlayPipeline;
			state.framebuffer = m_OutputFramebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { m_LightingOverlayBindingSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
		return true;
	}

	void SceneRenderer::SubmitLightingViewReadback()
	{
		if (!m_LightingViewReadbackRecorded)
			return;
		m_LightingViewReadbacks[m_LightingViewReadbackIndex].Readback->Submit();
		m_LightingViewReadbackIndex = (m_LightingViewReadbackIndex + 1) % c_LightingViewReadbacks;
		m_LightingViewReadbackRecorded = false;
	}

	void SceneRenderer::ResolveLightingViewStats(bool wait)
	{
		if (m_LightingView == SceneLightingView::None)
		{
			m_LightingViewStats.reset();
			return;
		}
		// The newest completed readback of the current view since it was chosen, newer than the statistics known.
		LightingViewReadback* newest = nullptr;
		for (LightingViewReadback& entry : m_LightingViewReadbacks)
		{
			if (!entry.Readback || !entry.Readback->IsPending() || entry.View != m_LightingView || entry.Render < m_LightingViewChosen)
				continue;
			if (m_LightingViewStats && entry.Render <= m_LightingViewStats->Render)
				continue;
			if (wait)
				entry.Readback->Wait();
			if (entry.Readback->IsReady() && (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		if (!newest)
			return;
		RenderData::LightingViewStatsData data = {};
		if (!newest->Readback->Read(&data, sizeof(data)))
			return;

		LightingViewStats stats;
		stats.View = newest->View;
		stats.Render = newest->Render;
		const uint64_t pixels = static_cast<uint64_t>(data.SurfacePixels) + data.BackgroundPixels;
		stats.SurfaceShare = pixels > 0 ? static_cast<float>(static_cast<double>(data.SurfacePixels) / static_cast<double>(pixels)) : 0.0f;
		const float evWidth = 1.0f / RenderData::c_LightingViewBinsPerEV;
		const float valueWidth = 1.0f / static_cast<float>(RenderData::c_LightingViewValueBins);
		if (IsLightingValueView(stats.View))
		{
			stats.ValueMedian = GetPercentile(data.Value, 0.5f, 0.0f, valueWidth);
			stats.ValueP05 = GetPercentile(data.Value, 0.05f, 0.0f, valueWidth);
			if (stats.View == SceneLightingView::Albedo && data.SurfacePixels > 0)
			{
				stats.BelowRangeShare = static_cast<float>(static_cast<double>(data.BelowRange) / data.SurfacePixels);
				stats.AboveRangeShare = static_cast<float>(static_cast<double>(data.AboveRange) / data.SurfacePixels);
			}
		}
		else
		{
			stats.SurfaceEV100 = glm::vec3(GetPercentile(data.SurfaceEV, 0.05f, RenderData::c_LightingViewMinEV100, evWidth),
				GetPercentile(data.SurfaceEV, 0.5f, RenderData::c_LightingViewMinEV100, evWidth),
				GetPercentile(data.SurfaceEV, 0.95f, RenderData::c_LightingViewMinEV100, evWidth));
			stats.BackgroundEV100 = GetPercentile(data.BackgroundEV, 0.5f, RenderData::c_LightingViewMinEV100, evWidth);
		}
		const float sky = glm::pi<float>() * GetLuminance(glm::vec3(data.SkyIrradiance));
		stats.SunIlluminance = newest->SunIlluminance;
		stats.SkyIlluminance = std::isfinite(sky) ? std::max(sky, 0.0f) : 0.0f;
		stats.SunToSkyRatio = stats.SunIlluminance > 0.0f && stats.SkyIlluminance > 0.0f ? stats.SunIlluminance / stats.SkyIlluminance : 0.0f;
		m_LightingViewStats = stats;
	}

	void SceneRenderer::ReleaseLightingViewResources(std::vector<nvrhi::ResourceHandle>& released)
	{
		auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		release(m_LightingStatsBuffer);
		release(m_LightingStatsBindingSet);
		release(m_LightingOverlayBindingSet);
		m_LightingStatsIrradiance = nullptr;
		for (LightingViewReadback& entry : m_LightingViewReadbacks)
			entry = LightingViewReadback {};
		m_LightingViewReadbackIndex = 0;
		m_LightingViewReadbackRecorded = false;
		m_LightingViewStats.reset();
	}

}
