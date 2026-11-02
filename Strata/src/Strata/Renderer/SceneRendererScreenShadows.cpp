#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// The sun's screen-space shadow mask (Docs/DeveloperGuide.md, "Sun shadows"): after the depth prepass, a fullscreen pass
// evaluates the cascades' filter (Include/Shadows.slang) at every opaque pixel from the prepass's depth and geometric normal
// (Effects/ScreenShadows.frag) into target A, with each pixel's blur radius, and an edge-aware separable blur runs A -> B
// -> A (Effects/ScreenShadowsBlur.frag) where the disk filtered, taking its grain without crossing silhouettes. The forward
// pass's opaque variants read A's red (SRV 28); transparent surfaces filter per pixel. With contact shadows
// (RendererQualitySettings::ContactShadows steps over DirectionalLightComponent::ContactShadowLength), the evaluation writes B
// and a pass of their own (Effects/ContactShadows.frag) marches toward the sun from B into A, for what the cascades are too
// coarse to show, before the blur. Fragment passes: comparison sampling in compute is unverified through MoltenVK. The
// GPU.ScreenShadows stage; nothing exists or runs without a shadowed sun.

namespace Strata
{

	bool SceneRenderer::EnsureScreenShadowResources()
	{
		if (!m_ScreenShadowTextures[0])
		{
			std::array<nvrhi::TextureHandle, 2> textures;
			std::array<nvrhi::FramebufferHandle, 2> framebuffers;
			for (size_t index = 0; index < textures.size(); index++)
			{
				textures[index] = CreateRenderTarget(m_Device, m_ViewportSize.x, m_ViewportSize.y, nvrhi::Format::RG8_UNORM,
					m_Specification.DebugName + (index == 0 ? ".ScreenShadowsA" : ".ScreenShadowsB"), false);
				framebuffers[index] = textures[index] ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(textures[index])) : nullptr;
				if (!textures[index] || !framebuffers[index])
					return false;
			}
			m_ScreenShadowTextures = std::move(textures);
			m_ScreenShadowFramebuffers = std::move(framebuffers);
			m_ScreenShadowSet = nullptr; // Name the new targets
			m_ScreenShadowBlurSets = {};
			m_ContactShadowSet = nullptr;
		}
		if (!m_ScreenShadowPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(7),           // The cascades' shadow map
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(17), // The cascades
				nvrhi::BindingLayoutItem::Texture_SRV(22),          // The prepass's depth
				nvrhi::BindingLayoutItem::Texture_SRV(23),          // Its normals
				nvrhi::BindingLayoutItem::Texture_SRV(32),          // The cascades' depth ranges
				nvrhi::BindingLayoutItem::Texture_SRV(33),          // The blue-noise tile
				nvrhi::BindingLayoutItem::Sampler(9)
			};
			m_ScreenShadowLayout = m_Device->createBindingLayout(layout);
			nvrhi::BindingLayoutDesc contactLayout;
			contactLayout.visibility = nvrhi::ShaderType::Pixel;
			contactLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(22),
				nvrhi::BindingLayoutItem::Texture_SRV(23),
				nvrhi::BindingLayoutItem::Texture_SRV(24), // The evaluated mask
				nvrhi::BindingLayoutItem::Texture_SRV(33),
				// Slot 1: register b0 belongs to the frame constants.
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ScreenShadowParameters))
			};
			m_ContactShadowLayout = m_Device->createBindingLayout(contactLayout);
			nvrhi::BindingLayoutDesc blurLayout;
			blurLayout.visibility = nvrhi::ShaderType::Pixel;
			blurLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(22),
				nvrhi::BindingLayoutItem::Texture_SRV(23),
				nvrhi::BindingLayoutItem::Texture_SRV(24), // The pass's source
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ScreenShadowBlurParameters))
			};
			m_ScreenShadowBlurLayout = m_Device->createBindingLayout(blurLayout);
			ShaderLibrary& shaders = Renderer::GetShaderLibrary();
			nvrhi::IShader* evaluate = shaders.Get("Effects/ScreenShadows.frag");
			nvrhi::IShader* blur = shaders.Get("Effects/ScreenShadowsBlur.frag");
			nvrhi::IShader* contact = shaders.Get("Effects/ContactShadows.frag");
			ST_CORE_VERIFY(evaluate && blur && contact && m_ScreenShadowLayout && m_ScreenShadowBlurLayout && m_ContactShadowLayout,
				"SceneRenderer: the screen-space shadow shaders are missing");
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = evaluate;
			desc.bindingLayouts = { m_ScreenShadowLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			const nvrhi::FramebufferInfo& info = m_ScreenShadowFramebuffers[0]->getFramebufferInfo();
			m_ScreenShadowPipeline = m_Device->createGraphicsPipeline(desc, info);
			desc.PS = blur;
			desc.bindingLayouts = { m_ScreenShadowBlurLayout };
			m_ScreenShadowBlurPipeline = m_Device->createGraphicsPipeline(desc, info);
			desc.PS = contact;
			desc.bindingLayouts = { m_ContactShadowLayout };
			m_ContactShadowPipeline = m_Device->createGraphicsPipeline(desc, info);
			ST_CORE_VERIFY(m_ScreenShadowPipeline && m_ScreenShadowBlurPipeline && m_ContactShadowPipeline,
				"SceneRenderer: failed to create the screen-space shadow pipelines");
		}
		// The evaluation names the shadow map and its ranges, which the cascades' release (a quality change) replaces.
		if (!m_ScreenShadowSet || m_ScreenShadowBoundMap != m_ShadowMap.Get() || m_ScreenShadowBoundRanges != m_ShadowRanges.Get())
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(7, m_ShadowMap),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_CascadeBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(32, m_ShadowRanges),
				nvrhi::BindingSetItem::Texture_SRV(33, Renderer::GetBlueNoiseTexture()),
				nvrhi::BindingSetItem::Sampler(9, m_ShadowCompareSampler)
			};
			m_ScreenShadowSet = m_Device->createBindingSet(desc, m_ScreenShadowLayout);
			if (!m_ScreenShadowSet)
				return false;
			m_ScreenShadowBoundMap = m_ShadowMap.Get();
			m_ScreenShadowBoundRanges = m_ShadowRanges.Get();
		}
		if (!m_ContactShadowSet)
		{
			// The contact shadows read the evaluation from B and write A.
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(24, m_ScreenShadowTextures[1]),
				nvrhi::BindingSetItem::Texture_SRV(33, Renderer::GetBlueNoiseTexture()),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ScreenShadowParameters))
			};
			m_ContactShadowSet = m_Device->createBindingSet(desc, m_ContactShadowLayout);
			if (!m_ContactShadowSet)
				return false;
		}
		for (size_t pass = 0; pass < m_ScreenShadowBlurSets.size(); pass++)
		{
			if (m_ScreenShadowBlurSets[pass])
				continue;
			// The horizontal pass reads A into B, the vertical one B into A.
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(24, m_ScreenShadowTextures[pass]),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ScreenShadowBlurParameters))
			};
			m_ScreenShadowBlurSets[pass] = m_Device->createBindingSet(desc, m_ScreenShadowBlurLayout);
			if (!m_ScreenShadowBlurSets[pass])
				return false;
		}
		return true;
	}

	void SceneRenderer::ReleaseScreenShadowTargets(std::vector<nvrhi::ResourceHandle>& released)
	{
		for (size_t index = 0; index < m_ScreenShadowTextures.size(); index++)
		{
			if (m_ScreenShadowTextures[index])
				released.emplace_back(m_ScreenShadowTextures[index].Get());
			if (m_ScreenShadowFramebuffers[index])
				released.emplace_back(m_ScreenShadowFramebuffers[index].Get());
			if (m_ScreenShadowBlurSets[index])
				released.emplace_back(m_ScreenShadowBlurSets[index].Get());
			m_ScreenShadowTextures[index] = nullptr;
			m_ScreenShadowFramebuffers[index] = nullptr;
			m_ScreenShadowBlurSets[index] = nullptr;
		}
		if (m_ScreenShadowSet)
			released.emplace_back(m_ScreenShadowSet.Get());
		if (m_ContactShadowSet)
			released.emplace_back(m_ContactShadowSet.Get());
		m_ScreenShadowSet = nullptr;
		m_ContactShadowSet = nullptr;
		m_ScreenShadowBoundMap = nullptr;
		m_ScreenShadowBoundRanges = nullptr;
	}

	void SceneRenderer::RenderScreenShadows(nvrhi::ICommandList* commandList, const RenderData::FrameConstants& frame)
	{
		const nvrhi::Viewport viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y));
		// Contact shadows (the quality's steps, the light's length): their reach on screen at most 5% of the image's height, faded
		// out from half the shadow distance (the last split) to all of it.
		RenderData::ScreenShadowParameters parameters = {};
		const uint32_t steps = m_Specification.Quality.ContactShadows;
		if (steps > 0 && m_ShadowLight.ContactLength > 0.0f)
		{
			// Called only for frames with a shadowed sun: ShadowParams.w is their cascade count, the last split the shadow distance.
			const int cascades = std::clamp(static_cast<int>(frame.ShadowParams.w), 1, static_cast<int>(RenderData::c_MaxCascades));
			const float shadowDistance = frame.CascadeSplits[cascades - 1];
			parameters.ContactLength = m_ShadowLight.ContactLength;
			parameters.ContactSteps = steps;
			parameters.ContactMaxPixels = c_ContactShadowMaxScreenShare * static_cast<float>(m_ViewportSize.y);
			parameters.ContactFadeStart = 0.5f * shadowDistance;
			parameters.ContactFadeEnd = shadowDistance;
			parameters.ContactThickness = c_ContactShadowThickness;
			parameters.ContactThicknessScale = c_ContactShadowThicknessScale;
		}
		// The evaluation into A, or into B for the contact shadows to read.
		const bool contacts = parameters.ContactSteps > 0;
		nvrhi::GraphicsState state;
		state.pipeline = m_ScreenShadowPipeline;
		state.framebuffer = m_ScreenShadowFramebuffers[contacts ? 1 : 0];
		state.viewport.addViewportAndScissorRect(viewport);
		state.bindings = { m_ScreenShadowSet };
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		if (contacts)
		{
			nvrhi::GraphicsState contactState;
			contactState.pipeline = m_ContactShadowPipeline;
			contactState.framebuffer = m_ScreenShadowFramebuffers[0];
			contactState.viewport.addViewportAndScissorRect(viewport);
			contactState.bindings = { m_ContactShadowSet };
			commandList->setGraphicsState(contactState);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		for (size_t pass = 0; pass < m_ScreenShadowBlurSets.size(); pass++)
		{
			RenderData::ScreenShadowBlurParameters blur = {};
			blur.DirectionX = pass == 0 ? 1 : 0;
			blur.DirectionY = pass == 0 ? 0 : 1;
			blur.PlaneTolerance = c_ScreenShadowPlaneTolerance;
			nvrhi::GraphicsState blurState;
			blurState.pipeline = m_ScreenShadowBlurPipeline;
			blurState.framebuffer = m_ScreenShadowFramebuffers[pass == 0 ? 1 : 0];
			blurState.viewport.addViewportAndScissorRect(viewport);
			blurState.bindings = { m_ScreenShadowBlurSets[pass] };
			commandList->setGraphicsState(blurState);
			commandList->setPushConstants(&blur, sizeof(blur));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}
	}

}
