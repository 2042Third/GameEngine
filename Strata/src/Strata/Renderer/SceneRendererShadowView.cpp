#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <cstring>
#include <string>

// The shadows debug view (SceneRenderOptions::ShowShadowView): Debug/ShadowViewStats.comp counts the frame's pixels by cascade
// and screen pixels per shadow texel into a buffer read back frames later (GetShadowViewStats), and Overlay/ShadowView.frag
// colors them over the finished image, both from Include/ShadowView.slang so they see the same pixels. Both run in the
// GPU.Overlays stage, with the overlays' frame constants (the camera's own matrices in TAA frames, over the jittered depth:
// an error of under half a pixel in the reconstructed position, immaterial to a ratio binned in eighths of a stop). Nothing
// exists until a frame shows the view.

namespace Strata
{

	bool SceneRenderer::DrawShadowView(nvrhi::ICommandList* commandList, const SceneRenderOptions& options, const nvrhi::Viewport& viewport)
	{
		if (!m_ShadowViewPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(17), // The cascades (Include/Scene.slang)
				nvrhi::BindingLayoutItem::Texture_SRV(22),          // The depth
				nvrhi::BindingLayoutItem::Texture_SRV(23),          // The sun's shadow mask, or white
				// Slot 1: register b0 belongs to the frame constants.
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ShadowViewParameters))
			};
			m_ShadowViewLayout = m_Device->createBindingLayout(layout);
			nvrhi::IShader* pixelShader = Renderer::GetShaderLibrary().Get("Overlay/ShadowView.frag");
			ST_CORE_VERIFY(pixelShader && m_ShadowViewLayout, "SceneRenderer: the shadow view shader is missing");
			// Blended over the image as the motion view is, without the depth attachment: it reads the depth.
			nvrhi::BlendState::RenderTarget blend;
			blend.setBlendEnable(true)
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = pixelShader;
			desc.bindingLayouts = { m_ShadowViewLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.blendState.setRenderTarget(0, blend);
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			m_ShadowViewPipeline = m_Device->createGraphicsPipeline(desc, m_OutputFramebuffer->getFramebufferInfo());

			nvrhi::BindingLayoutDesc countLayout;
			countLayout.visibility = nvrhi::ShaderType::Compute;
			countLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(17),
				nvrhi::BindingLayoutItem::Texture_SRV(22),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)
			};
			m_ShadowViewCountLayout = m_Device->createBindingLayout(countLayout);
			ST_CORE_VERIFY(m_ShadowViewPipeline && m_ShadowViewCountLayout, "SceneRenderer: failed to create the shadow view pipeline");
			m_ShadowViewCountPipeline = CreateComputePipeline("Debug/ShadowViewStats.comp", m_ShadowViewCountLayout);
			ST_CORE_VERIFY(m_ShadowViewCountPipeline, "SceneRenderer: failed to create the shadow view's count pipeline");
		}
		if (!m_ShadowViewCountBuffer)
		{
			nvrhi::BufferHandle buffer = m_Device->createBuffer(nvrhi::BufferDesc()
				.setByteSize(sizeof(RenderData::ShadowViewCounts))
				.setCanHaveUAVs(true)
				.setStructStride(sizeof(uint32_t))
				.setInitialState(nvrhi::ResourceStates::UnorderedAccess)
				.setKeepInitialState(true)
				.setDebugName(m_Specification.DebugName + ".ShadowViewCounts"));
			bool readbacks = true;
			for (ShadowViewReadback& entry : m_ShadowViewReadbacks)
			{
				entry = {};
				entry.Readback = CreateScope<BufferReadback>(m_Device, sizeof(RenderData::ShadowViewCounts), m_Specification.DebugName + ".ShadowViewReadback");
				readbacks = readbacks && entry.Readback->IsValid();
			}
			if (!buffer || !readbacks)
			{
				for (ShadowViewReadback& entry : m_ShadowViewReadbacks)
					entry = {};
				m_Errors.Report("failed to create the shadows debug view's statistics buffers");
				return false;
			}
			m_ShadowViewCountBuffer = std::move(buffer);
			m_ShadowViewCountBindingSet = nullptr; // Names the buffer
		}
		// The view darkens its colors by the sun's shadow: the frame's mask, made by the first frame with a shadowed sun (white
		// before; without a shadowed sun the view reads no pixel of it).
		nvrhi::ITexture* mask = GetBoundScreenShadows();
		if (!m_ShadowViewBindingSet || !m_ShadowViewCountBindingSet || m_ShadowViewBoundMask != mask)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_CascadeBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, mask),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ShadowViewParameters))
			};
			m_ShadowViewBindingSet = m_Device->createBindingSet(desc, m_ShadowViewLayout);
			m_ShadowViewBoundMask = mask;
			nvrhi::BindingSetDesc countDesc;
			countDesc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_CascadeBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ShadowViewCountBuffer)
			};
			m_ShadowViewCountBindingSet = m_Device->createBindingSet(countDesc, m_ShadowViewCountLayout);
			if (!m_ShadowViewBindingSet || !m_ShadowViewCountBindingSet)
			{
				m_ShadowViewBindingSet = nullptr;
				m_ShadowViewCountBindingSet = nullptr;
				m_ShadowViewBoundMask = nullptr;
				m_Errors.Report("failed to create the shadows debug view's bindings");
				return false;
			}
		}

		// The counts: a readback whose earlier copy is still under way is left to finish, and the frame is not counted.
		ShadowViewReadback& readback = m_ShadowViewReadbacks[m_ShadowViewReadbackIndex];
		if (!readback.Readback->IsPending() || readback.Readback->IsReady())
		{
			commandList->clearBufferUInt(m_ShadowViewCountBuffer, 0);
			nvrhi::ComputeState countState;
			countState.pipeline = m_ShadowViewCountPipeline;
			countState.bindings = { m_ShadowViewCountBindingSet };
			commandList->setComputeState(countState);
			commandList->dispatch((m_ViewportSize.x + 15) / 16, (m_ViewportSize.y + 15) / 16, 1);
			readback.Readback->Record(commandList, m_ShadowViewCountBuffer, 0, sizeof(RenderData::ShadowViewCounts));
			readback.Render = m_RenderCount + 1;
			readback.ViewportSize = m_ViewportSize;
			readback.ShadowMapSize = m_ShadowMapSize;
			readback.CascadeCount = m_Stats.CascadeCount;
			readback.Cascades = m_Stats.Cascades;
			m_ShadowViewReadbackRecorded = true;
		}

		RenderData::ShadowViewParameters parameters = {};
		for (size_t color = 0; color < options.ShadowViewColors.size(); color++)
			parameters.Colors[color] = options.ShadowViewColors[color];
		nvrhi::GraphicsState state;
		state.pipeline = m_ShadowViewPipeline;
		state.framebuffer = m_OutputFramebuffer;
		state.viewport.addViewportAndScissorRect(viewport);
		state.bindings = { m_ShadowViewBindingSet };
		commandList->setGraphicsState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		return true;
	}

	void SceneRenderer::SubmitShadowViewReadback()
	{
		if (!m_ShadowViewReadbackRecorded)
			return;
		m_ShadowViewReadbacks[m_ShadowViewReadbackIndex].Readback->Submit();
		m_ShadowViewReadbackIndex = (m_ShadowViewReadbackIndex + 1) % c_GPURecordCount;
		m_ShadowViewReadbackRecorded = false;
	}

	void SceneRenderer::ResolveShadowViewStats()
	{
		// The newest completed copy of a frame newer than the statistics held.
		ShadowViewReadback* newest = nullptr;
		for (ShadowViewReadback& entry : m_ShadowViewReadbacks)
		{
			if (entry.Readback && entry.Readback->IsPending() && entry.Render > m_ShadowViewStats.Render && entry.Readback->IsReady()
				&& (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		if (!newest)
			return;
		RenderData::ShadowViewCounts counts = {};
		if (!newest->Readback->Read(&counts, sizeof(counts)))
			return;
		ShadowViewStats stats;
		stats.Render = newest->Render;
		stats.ViewportSize = newest->ViewportSize;
		stats.ShadowMapSize = newest->ShadowMapSize;
		stats.SurfacePixels = counts.SurfacePixels;
		stats.SkyPixels = counts.SkyPixels;
		stats.UncoveredPixels = counts.UncoveredPixels;
		stats.CascadeCount = newest->CascadeCount;
		stats.Cascades = newest->Cascades;
		for (uint32_t cascade = 0; cascade < RenderData::c_MaxCascades; cascade++)
			std::copy(std::begin(counts.Bins[cascade]), std::end(counts.Bins[cascade]), stats.Bins[cascade].begin());
		ShadowView::SummarizeAll(stats);
		m_ShadowViewStats = stats;
	}

}
