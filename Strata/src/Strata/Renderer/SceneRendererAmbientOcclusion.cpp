#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Ambient occlusion (PostProcessComponent::AmbientOcclusion): GTAO from the depth prepass's depth and normals into a raw
// texture (Effects/GTAO.comp: the quality's slices and steps, turned by a fixed 4x4 pattern), then the denoiser that needs no
// history into the texture the forward pass reads (Effects/AODenoise.comp: the pattern resolved, then blurred within edges;
// scene SRV 11), in the GPU.AO stage after the prepass. Both are R16_FLOAT where the device stores it (R32_FLOAT otherwise)
// and of the viewport's size; their passes take their parameters by push constants (RenderData::AOParameters).

namespace Strata
{

	namespace
	{

		// Ambient occlusion lies in [0, 1]: half precision holds it, where the device can store r16f images (an optional
		// feature); r32f storage needs none.
		constexpr nvrhi::Format c_AOFormat = nvrhi::Format::R16_FLOAT;
		constexpr nvrhi::Format c_AOFallbackFormat = nvrhi::Format::R32_FLOAT;

		uint32_t DispatchGroups(uint32_t size)
		{
			return (size + 7) / 8;
		}

	}

	void SceneRenderer::CreateAmbientOcclusionResources()
	{
		nvrhi::BindingLayoutDesc aoLayout;
		aoLayout.visibility = nvrhi::ShaderType::Compute;
		aoLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(8),
			nvrhi::BindingLayoutItem::Texture_SRV(9),
			nvrhi::BindingLayoutItem::Texture_SRV(10),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::AOParameters))
		};
		m_AOBindingLayout = m_Device->createBindingLayout(aoLayout);

		const bool halfAO = m_Specification.ExtendedStorageFormats && Renderer::GetGraphicsDevice().GetInfo().SupportsExtendedStorageFormats;
		m_AOFormat = halfAO ? c_AOFormat : c_AOFallbackFormat;
		m_GTAOPipeline = CreateComputePipeline(halfAO ? "Effects/GTAOHalf.comp" : "Effects/GTAO.comp", m_AOBindingLayout);
		m_AODenoisePipeline = CreateComputePipeline(halfAO ? "Effects/AODenoiseHalf.comp" : "Effects/AODenoise.comp", m_AOBindingLayout);
		ST_CORE_VERIFY(m_AOBindingLayout && m_GTAOPipeline && m_AODenoisePipeline, "SceneRenderer: failed to create the ambient occlusion resources");
	}

	bool SceneRenderer::CreateAmbientOcclusionTargets(uint32_t width, uint32_t height)
	{
		const std::string& name = m_Specification.DebugName;
		auto createStorage = [&](const std::string& debugName)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = m_AOFormat;
			desc.isUAV = true;
			desc.debugName = debugName;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return m_Device->createTexture(desc);
		};
		m_AORawTexture = createStorage(name + ".AORaw");
		m_AOTexture = createStorage(name + ".AO");
		if (!m_AORawTexture || !m_AOTexture)
			return false;

		auto createAOSet = [&](nvrhi::ITexture* input, nvrhi::ITexture* output)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(8, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(9, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(10, input),
				nvrhi::BindingSetItem::Texture_UAV(0, output),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::AOParameters))
			};
			return m_Device->createBindingSet(desc, m_AOBindingLayout);
		};
		// The GTAO pass reads nothing through slot 10; it binds the denoised texture there, which it does not write.
		m_GTAOBindingSet = createAOSet(m_AOTexture, m_AORawTexture);
		m_AODenoiseBindingSet = createAOSet(m_AORawTexture, m_AOTexture);
		return m_GTAOBindingSet && m_AODenoiseBindingSet;
	}

	void SceneRenderer::ReleaseAmbientOcclusionTargets(std::vector<nvrhi::ResourceHandle>& released)
	{
		auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		release(m_AORawTexture);
		release(m_AOTexture);
		release(m_GTAOBindingSet);
		release(m_AODenoiseBindingSet);
	}

	void SceneRenderer::RenderAmbientOcclusion(nvrhi::ICommandList* commandList, const PostProcessComponent& settings)
	{
		const AmbientOcclusionSamples samples = RendererQuality::GetAmbientOcclusionSamples(m_Specification.Quality.AmbientOcclusionQuality);
		const float height = static_cast<float>(m_ViewportSize.y);
		RenderData::AOParameters parameters = {};
		parameters.Radius = std::isfinite(settings.AmbientOcclusionRadius) ? std::clamp(settings.AmbientOcclusionRadius, 0.01f, 10.0f) : 1.0f;
		parameters.Intensity = std::isfinite(settings.AmbientOcclusionIntensity) ? std::clamp(settings.AmbientOcclusionIntensity, 0.0f, 8.0f) : 1.0f;
		parameters.SliceCount = samples.Slices;
		parameters.StepsPerSide = samples.StepsPerSide;
		parameters.MinRadiusPixels = std::max(RenderData::c_AOMinRadiusFraction * height, RenderData::c_AOMinRadiusPixels);
		parameters.MaxRadiusPixels = std::max(RenderData::c_AOMaxRadiusFraction * height, parameters.MinRadiusPixels);
		// The relaxation per step that keeps the relaxation per meter of 0.1 per step at 6 steps.
		parameters.ThinOccluderDecay = 1.0f - std::pow(0.9f, 6.0f / static_cast<float>(samples.StepsPerSide));
		m_Stats.AmbientOcclusionSamples = samples.Slices * samples.StepsPerSide * 2;
		const uint32_t groupsX = DispatchGroups(m_ViewportSize.x);
		const uint32_t groupsY = DispatchGroups(m_ViewportSize.y);

		nvrhi::ComputeState state;
		state.pipeline = m_GTAOPipeline;
		state.bindings = { m_GTAOBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(groupsX, groupsY, 1);

		state.pipeline = m_AODenoisePipeline;
		state.bindings = { m_AODenoiseBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(groupsX, groupsY, 1);
	}

}
