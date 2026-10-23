#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <string>

// Screen-space reflections (PostProcessComponent::ScreenSpaceReflections; DG "Reflections"): with temporal anti-aliasing the
// prepass also writes the surface target (Scene/DepthPrepassSurface.frag), a compute pass traces the reflections at half
// resolution against the frame's depth and fetches what the previous frame's resolved image showed where the rays hit
// (Effects/ScreenSpaceReflections.comp, in GPU.Reflections), and the forward pass mixes them over the probes' and the sky's
// specular light (Scene/Forward.frag, SRV 18).

namespace Strata
{

	namespace
	{

		// The normal-mapped view-space normal (octahedral), the roughness and the metallic factor of the prepass's surfaces.
		constexpr nvrhi::Format c_SurfaceFormat = nvrhi::Format::RGBA16_FLOAT;
		// The reflected luminance (pre-exposed, within half floats as the HDR target is) and the confidence.
		constexpr nvrhi::Format c_ScreenReflectionFormat = nvrhi::Format::RGBA16_FLOAT;

	}

	nvrhi::ITexture* SceneRenderer::GetBoundScreenReflections() const
	{
		return m_ScreenReflectionTexture ? m_ScreenReflectionTexture.Get() : Renderer::GetBlackTexture();
	}

	bool SceneRenderer::EnsureScreenReflectionResources()
	{
		if (!m_ScreenReflectionPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(22),
				nvrhi::BindingLayoutItem::Texture_SRV(23),
				nvrhi::BindingLayoutItem::Texture_SRV(24),
				nvrhi::BindingLayoutItem::Texture_SRV(25),
				nvrhi::BindingLayoutItem::Texture_SRV(26),
				nvrhi::BindingLayoutItem::Sampler(8),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ScreenReflectionParameters))
			};
			m_ScreenReflectionLayout = m_Device->createBindingLayout(layout);
			ShaderLibrary& shaders = Renderer::GetShaderLibrary();
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shaders.Get("Effects/ScreenSpaceReflections.comp");
			desc.bindingLayouts = { m_ScreenReflectionLayout };
			m_PrepassSurfacePixelShader = shaders.Get(m_Specification.EntityIDs ? "Scene/DepthPrepassSurface.frag" : "Scene/DepthPrepassNoEntityIDsSurface.frag");
			ST_CORE_VERIFY(desc.CS && m_ScreenReflectionLayout && m_PrepassSurfacePixelShader, "SceneRenderer: the screen-space reflection shaders are missing");
			m_ScreenReflectionPipeline = m_Device->createComputePipeline(desc);
			ST_CORE_VERIFY(m_ScreenReflectionPipeline, "SceneRenderer: failed to create the screen-space reflection pipeline");
		}
		if (m_ScreenReflectionSets[0])
			return true;

		// Made after the motion and temporal resources of the frame (the velocity target and the history), which go with the
		// other targets as these do.
		ST_CORE_ASSERT(m_VelocityTexture && m_HistoryTextures[0] && m_HistoryTextures[1], "Screen-space reflections need the TAA's resources");
		const uint32_t width = m_ViewportSize.x;
		const uint32_t height = m_ViewportSize.y;
		nvrhi::TextureDesc surfaceDesc;
		surfaceDesc.width = width;
		surfaceDesc.height = height;
		surfaceDesc.format = c_SurfaceFormat;
		surfaceDesc.isRenderTarget = true;
		surfaceDesc.debugName = m_Specification.DebugName + ".Surface";
		surfaceDesc.initialState = nvrhi::ResourceStates::RenderTarget;
		surfaceDesc.keepInitialState = true;
		nvrhi::TextureHandle surface = m_Device->createTexture(surfaceDesc);
		nvrhi::TextureDesc reflectionDesc;
		reflectionDesc.width = std::max(1u, (width + 1) / 2);
		reflectionDesc.height = std::max(1u, (height + 1) / 2);
		reflectionDesc.format = c_ScreenReflectionFormat;
		reflectionDesc.isUAV = true;
		reflectionDesc.debugName = m_Specification.DebugName + ".ScreenReflections";
		reflectionDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		reflectionDesc.keepInitialState = true;
		nvrhi::TextureHandle reflections = m_Device->createTexture(reflectionDesc);

		nvrhi::FramebufferHandle framebuffer;
		std::array<nvrhi::BindingSetHandle, 2> sets;
		if (surface && reflections)
		{
			nvrhi::FramebufferDesc desc;
			desc.addColorAttachment(m_NormalTexture);
			if (m_EntityIDTexture)
				desc.addColorAttachment(m_EntityIDTexture);
			desc.addColorAttachment(m_VelocityTexture);
			desc.addColorAttachment(surface);
			desc.setDepthAttachment(m_DepthTexture);
			framebuffer = m_Device->createFramebuffer(desc);
			// By the history texture the trace reads: the one holding the newest image when the frame starts.
			for (uint32_t index = 0; index < 2; index++)
			{
				nvrhi::BindingSetDesc set;
				set.bindings = {
					nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
					nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
					nvrhi::BindingSetItem::Texture_SRV(23, surface),
					nvrhi::BindingSetItem::Texture_SRV(24, m_VelocityTexture),
					nvrhi::BindingSetItem::Texture_SRV(25, m_HistoryTextures[index]),
					nvrhi::BindingSetItem::Texture_SRV(26, m_NormalTexture),
					nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
					nvrhi::BindingSetItem::Texture_UAV(0, reflections),
					nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ScreenReflectionParameters))
				};
				sets[index] = m_Device->createBindingSet(set, m_ScreenReflectionLayout);
			}
		}
		if (!framebuffer || !sets[0] || !sets[1])
		{
			// What was made goes with this scope (no command list used it yet): the next frame that wants them tries again.
			m_Errors.Report(fmt::format("failed to create the screen-space reflections' targets for a {}x{} viewport; rendering without them", width,
				height));
			return false;
		}
		m_SurfaceTexture = std::move(surface);
		m_ScreenReflectionTexture = std::move(reflections);
		m_SurfacePrepassFramebuffer = std::move(framebuffer);
		m_ScreenReflectionSets = std::move(sets);
		return true;
	}

	void SceneRenderer::TraceScreenReflections(nvrhi::ICommandList* commandList, const PostProcessComponent& settings)
	{
		ST_PROFILE_FUNCTION();
		const auto finiteIn = [](float value, float fallback) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback; };
		RenderData::ScreenReflectionParameters parameters;
		parameters.Intensity = finiteIn(settings.ScreenSpaceReflectionsIntensity, 0.0f);
		parameters.MaxRoughness = finiteIn(settings.ScreenSpaceReflectionsMaxRoughness, 0.0f);
		// The trace reads the history's newest image, the previous frame's, at its own pre-exposure.
		parameters.HistoryScale = m_PreExposure / m_HistoryPreExposure;
		parameters.Frame = m_TemporalFrames;

		nvrhi::ComputeState state;
		state.pipeline = m_ScreenReflectionPipeline;
		state.bindings = { m_ScreenReflectionSets[m_HistoryIndex] };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		const nvrhi::TextureDesc& desc = m_ScreenReflectionTexture->getDesc();
		commandList->dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1);
	}

}
