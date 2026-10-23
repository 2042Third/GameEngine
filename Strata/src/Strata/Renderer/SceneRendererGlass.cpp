#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <string>

// Glass (transmissive materials, MaterialProperties::Transmission; DG "Clear coat and glass"): the transparent pass draws it
// first, unblended, writing depth, entity ids and motion (Scene/ForwardGlass.frag), and it reads what lies behind it from a
// copy of the frame's HDR image made just before (the opaque surfaces and the sky), at half resolution with mips that rough
// glass reads blurred (Effects/SceneColorCopy.comp, in GPU.Transparent).

namespace Strata
{

	namespace
	{

		constexpr nvrhi::Format c_SceneColorFormat = nvrhi::Format::R11G11B10_FLOAT;
		// Without the extended storage formats (GraphicsDeviceInfo::SupportsExtendedStorageFormats) B10G11R11 cannot be written by
		// a compute shader.
		constexpr nvrhi::Format c_SceneColorFallbackFormat = nvrhi::Format::RGBA16_FLOAT;
		// Levels of the copy: from half the viewport's size down to 1/64 of it, the blur of the roughest glass.
		constexpr uint32_t c_MaxSceneColorLevels = 6;
		constexpr uint32_t c_SceneColorGroupSize = 8;

	}

	nvrhi::ITexture* SceneRenderer::GetBoundSceneColor() const
	{
		return m_SceneColorTexture ? m_SceneColorTexture.Get() : Renderer::GetBlackTexture();
	}

	bool SceneRenderer::EnsureSceneColorResources()
	{
		if (!m_SceneColorPipeline)
		{
			const bool extended = m_Specification.ExtendedStorageFormats && Renderer::GetGraphicsDevice().GetInfo().SupportsExtendedStorageFormats;
			m_SceneColorFormat = extended ? c_SceneColorFormat : c_SceneColorFallbackFormat;
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Texture_UAV(0)
			};
			m_SceneColorLayout = m_Device->createBindingLayout(layout);
			nvrhi::ComputePipelineDesc desc;
			desc.CS = Renderer::GetShaderLibrary().Get(extended ? "Effects/SceneColorCopy.comp" : "Effects/SceneColorCopyFallback.comp");
			desc.bindingLayouts = { m_SceneColorLayout };
			ST_CORE_VERIFY(desc.CS && m_SceneColorLayout, "SceneRenderer: the scene-color copy shader is missing");
			m_SceneColorPipeline = m_Device->createComputePipeline(desc);
			ST_CORE_VERIFY(m_SceneColorPipeline, "SceneRenderer: failed to create the scene-color copy pipeline");
		}
		if (m_SceneColorTexture)
			return true;

		// Made after the frame's render targets, with which it goes (ReleaseRenderTargets).
		ST_CORE_ASSERT(m_HDRTexture, "The scene-color copy reads the HDR target");
		nvrhi::TextureDesc desc;
		desc.width = std::max(1u, (m_ViewportSize.x + 1) / 2);
		desc.height = std::max(1u, (m_ViewportSize.y + 1) / 2);
		uint32_t levels = 1;
		while (levels < c_MaxSceneColorLevels && std::min(desc.width, desc.height) >> levels > 0)
			levels++;
		desc.mipLevels = levels;
		desc.format = m_SceneColorFormat;
		desc.isUAV = true;
		desc.debugName = m_Specification.DebugName + ".SceneColor";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = m_Device->createTexture(desc);
		std::vector<nvrhi::BindingSetHandle> sets;
		if (texture)
		{
			// Level 0 from the HDR target, each further one from the level above it.
			for (uint32_t level = 0; level < levels; level++)
			{
				nvrhi::BindingSetDesc set;
				set.bindings = {
					level == 0 ? nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture)
						: nvrhi::BindingSetItem::Texture_SRV(0, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level - 1, 1, 0, 1)),
					nvrhi::BindingSetItem::Texture_UAV(0, texture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level, 1, 0, 1))
				};
				nvrhi::BindingSetHandle handle = m_Device->createBindingSet(set, m_SceneColorLayout);
				if (!handle)
					break;
				sets.push_back(std::move(handle));
			}
		}
		if (!texture || sets.size() != levels)
		{
			// What was made goes with this scope (no command list used it yet): the next frame that draws glass tries again.
			m_Errors.Report(fmt::format("failed to create the scene-color copy for a {}x{} viewport; glass shows black behind it", m_ViewportSize.x,
				m_ViewportSize.y));
			return false;
		}
		m_SceneColorTexture = std::move(texture);
		m_SceneColorSets = std::move(sets);
		return true;
	}

	void SceneRenderer::CopySceneColor(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		const nvrhi::TextureDesc& desc = m_SceneColorTexture->getDesc();
		nvrhi::ComputeState state;
		state.pipeline = m_SceneColorPipeline;
		for (uint32_t level = 0; level < static_cast<uint32_t>(m_SceneColorSets.size()); level++)
		{
			state.bindings = { m_SceneColorSets[level] };
			commandList->setComputeState(state);
			const uint32_t width = std::max(1u, desc.width >> level);
			const uint32_t height = std::max(1u, desc.height >> level);
			commandList->dispatch((width + c_SceneColorGroupSize - 1) / c_SceneColorGroupSize, (height + c_SceneColorGroupSize - 1) / c_SceneColorGroupSize, 1);
		}
	}

}
