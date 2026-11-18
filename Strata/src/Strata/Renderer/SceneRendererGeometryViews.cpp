#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

// The debug views of what the frame drew and left out, drawn into the output texture in the GPU.Overlays stage with the
// overlays' frame constants (the camera's own matrices in TAA frames, over the jittered depth with its bias), after the image's
// tints and before the other overlays:
// - the wireframe view (SceneRenderOptions::ShowWireframe) replays the frame's draws with Pass::Wireframe pipelines, whose
//   rasterizer fills lines: the GPU-driven path's view buckets (DrawGPUView, the arguments its culling wrote) or the CPU's
//   opaque batches, then the transparent batches both paths draw on the CPU; the mesh vertex shaders (skinned ones for skinned
//   instances, the pipelines' winding for mirrored ones) and Scene/Wireframe.frag, which tests the level-of-detail fade's
//   mask as the prepass does;
// - the culled view (SceneRenderOptions::ShowCulled) draws a box per entry of the frame's culled list, which Scene/Cull.comp's
//   counting passes write on the GPU-driven path (u_Cull.CulledView) and the CPU's extraction on the CPU path
//   (m_CulledProxies), from the scene copy's proxy records (Overlay/CulledView.vert).
// Nothing of either exists until a frame shows it, and frames without them record nothing of them.

namespace Strata
{

	namespace
	{

		uint32_t PackDisplayColor(const glm::vec4& color)
		{
			const auto channel = [](float value, uint32_t shift)
			{
				const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
				return static_cast<uint32_t>(std::lround(clamped * 255.0f)) << shift;
			};
			return channel(color.r, 0) | channel(color.g, 8) | channel(color.b, 16) | channel(color.a, 24);
		}

		// Overlay/CulledView.vert's push constants.
		struct CulledViewParameters
		{
			uint32_t Color = 0;  // RGBA8, sRGB, packed (red in the low byte)
			uint32_t Reason = 0; // The entries drawn: RenderData::c_CulledBySize or c_CulledByOcclusion
			float MinPixels = 0.0f; // Boxes smaller on screen are drawn this wide about their center (0: as they are)
			uint32_t Padding = 0;
		};
		static_assert(sizeof(CulledViewParameters) == 16);

	}

	bool SceneRenderer::DrawWireframeView(nvrhi::ICommandList* commandList, const SceneRenderOptions& options)
	{
		if (!Renderer::GetGraphicsDevice().GetInfo().SupportsWireframe)
		{
			m_Errors.Report("the wireframe view needs the device's non-solid fill mode (fillModeNonSolid), which this device lacks: the image is "
				"drawn without its lines");
			return false;
		}
		if (!m_WireframePushBindingSet)
		{
			nvrhi::ShaderHandle pixelShader = Renderer::GetShaderLibrary().Get("Scene/Wireframe.frag");
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			// Slot 1: register b0 belongs to the frame constants of the scene layout.
			layout.bindings = { nvrhi::BindingLayoutItem::PushConstants(1, sizeof(uint32_t)) };
			nvrhi::BindingLayoutHandle pushLayout = m_Device->createBindingLayout(layout);
			nvrhi::BindingSetDesc set;
			set.bindings = { nvrhi::BindingSetItem::PushConstants(1, sizeof(uint32_t)) };
			nvrhi::BindingSetHandle pushSet = pushLayout ? m_Device->createBindingSet(set, pushLayout) : nullptr;
			if (!pixelShader || !pushSet)
			{
				m_Errors.Report("failed to create the wireframe view's shader or bindings");
				return false;
			}
			m_WireframePixelShader = std::move(pixelShader);
			m_WireframePushLayout = std::move(pushLayout);
			m_WireframePushBindingSet = std::move(pushSet);
		}
		m_WireframeColor = PackDisplayColor(options.WireframeColor);

		// The statistics count the frame's own draws: the view's replay leaves them as they were.
		const auto drawCommands = m_Stats.DrawCommands;
		if (m_GPUDrivenFrame)
			DrawGPUView(commandList, Pass::Wireframe, 0, m_CullConstants.ViewBucketCount);
		nvrhi::GraphicsState state;
		state.framebuffer = m_OverlayFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable(), m_WireframePushBindingSet };
		const auto getKey = [](const SceneDrawBatch& batch) { return PipelineKey { Pass::Wireframe, batch.DoubleSided, batch.Mirrored, batch.Skinned }; };
		const auto setColor = [this, commandList](const SceneDrawBatch&) { commandList->setPushConstants(&m_WireframeColor, sizeof(m_WireframeColor)); };
		// The CPU's opaque batches (none on the GPU-driven path), then the transparent ones, at their places in the draw arguments.
		if (!m_GPUDrivenFrame)
			DrawBuckets(commandList, state, m_OpaqueBatches, 0, getKey, nullptr, setColor);
		DrawBuckets(commandList, state, m_TransparentBatches, static_cast<uint32_t>(m_OpaqueBatches.size()), getKey, nullptr, setColor);
		m_Stats.DrawCommands = drawCommands;
		return true;
	}

	bool SceneRenderer::DrawCulledView(nvrhi::ICommandList* commandList, const SceneRenderOptions& options)
	{
		// The proxies' records: none without proxies, and then nothing was culled.
		nvrhi::IBuffer* proxies = m_SceneGPU ? m_SceneGPU->GetProxyBuffer() : nullptr;
		if (!proxies)
			return true;
		uint32_t entries = 0;
		if (m_GPUDrivenFrame)
		{
			// Without proxies to cull the culling recorded nothing (RunGPUCulling).
			if (!m_GPUCullFrame || !m_CulledViewArguments || !m_CulledViewList)
				return true;
		}
		else
		{
			entries = static_cast<uint32_t>(m_CulledProxies.size());
			if (entries == 0)
				return true;
			bool recreated = false;
			if (!EnsureBufferCapacity(m_CulledViewList, entries, sizeof(uint32_t), "CulledViewList", recreated, BufferKind::StructuredUAV))
				return false; // Reported
			commandList->writeBuffer(m_CulledViewList, m_CulledProxies.data(), m_CulledProxies.size() * sizeof(uint32_t));
		}

		if (!m_CulledViewPipeline)
		{
			ShaderLibrary& shaders = Renderer::GetShaderLibrary();
			nvrhi::ShaderHandle vertexShader = shaders.Get("Overlay/CulledView.vert");
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Vertex;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22), // The scene copy's proxy records (registers Include/Scene.slang leaves free)
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23), // The culled list
				// Slot 1: register b0 belongs to the frame constants.
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(CulledViewParameters))
			};
			nvrhi::BindingLayoutHandle bindingLayout = m_Device->createBindingLayout(layout);
			nvrhi::GraphicsPipelineHandle pipeline;
			if (vertexShader && bindingLayout)
			{
				nvrhi::GraphicsPipelineDesc desc;
				desc.primType = nvrhi::PrimitiveType::LineList;
				desc.VS = vertexShader;
				desc.PS = m_DebugLinePixelShader;
				desc.bindingLayouts = { bindingLayout };
				desc.renderState.rasterState.setCullNone();
				// Over everything: the boxes of what lies behind other surfaces are what the view shows.
				desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
				pipeline = m_Device->createGraphicsPipeline(desc, m_OverlayFramebuffer->getFramebufferInfo());
			}
			if (!pipeline)
			{
				m_Errors.Report("failed to create the culled view's pipeline");
				return false;
			}
			m_CulledViewLayout = std::move(bindingLayout);
			m_CulledViewPipeline = std::move(pipeline);
		}
		if (!m_CulledViewBindingSet || m_CulledViewBoundProxies != proxies || m_CulledViewBoundList != m_CulledViewList.Get())
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(22, proxies),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_CulledViewList),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(CulledViewParameters))
			};
			m_CulledViewBindingSet = m_Device->createBindingSet(desc, m_CulledViewLayout);
			if (!m_CulledViewBindingSet)
			{
				m_CulledViewBoundProxies = nullptr;
				m_CulledViewBoundList = nullptr;
				m_Errors.Report("failed to create the culled view's bindings");
				return false;
			}
			m_CulledViewBoundProxies = proxies;
			m_CulledViewBoundList = m_CulledViewList.Get();
		}

		nvrhi::GraphicsState state;
		state.pipeline = m_CulledViewPipeline;
		state.framebuffer = m_OverlayFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_CulledViewBindingSet };
		state.indirectParams = m_GPUDrivenFrame ? m_CulledViewArguments.Get() : nullptr;
		// A draw per reason: crossing lines of one color do not depend on the order the entries were recorded in.
		const uint32_t reasons[] = { RenderData::c_CulledBySize, RenderData::c_CulledByOcclusion };
		for (size_t index = 0; index < std::size(reasons); index++)
		{
			CulledViewParameters parameters;
			parameters.Color = PackDisplayColor(glm::vec4(glm::vec3(options.CulledColors[index]), 1.0f));
			parameters.Reason = reasons[index];
			parameters.MinPixels = reasons[index] == RenderData::c_CulledBySize ? RenderData::c_CulledViewMinPixels : 0.0f;
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			if (m_GPUDrivenFrame)
				commandList->drawIndirect(0, 1);
			else
				commandList->draw(nvrhi::DrawArguments().setVertexCount(24).setInstanceCount(entries));
		}
		return true;
	}

}
