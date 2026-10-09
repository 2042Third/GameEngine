#include "stpch.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include "Strata/Renderer/ShaderLibrary.h"

#include <imgui.h>

namespace Strata
{

	namespace
	{

		struct PushConstants
		{
			float Scale[2];
			float Translate[2];
		};

		// Backend storage attached to ImGui-managed textures (font atlas pages).
		struct ManagedTexture
		{
			nvrhi::TextureHandle Texture;
		};

		// Binding sets unused for this many frames are released (more than any number of frames in flight).
		constexpr uint64_t c_BindingSetRetentionFrames = 8;

		ImTextureID ToTextureID(nvrhi::ITexture* texture)
		{
			return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
		}

		ImGuiRenderState* GetRenderState()
		{
			return static_cast<ImGuiRenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
		}

		// Standard ImGui draw callbacks. Like any user callback they are called by the render loop and act on the
		// exposed render state; they are never identified by address (the linker may merge identical functions).
		// Pipeline state is re-specified for every draw, so a reset only has to restore the default sampler.
		void DrawCallbackResetRenderState(const ImDrawList*, const ImDrawCmd*)
		{
			if (ImGuiRenderState* state = GetRenderState())
				state->NearestFiltering = false;
		}

		void DrawCallbackSetSamplerLinear(const ImDrawList*, const ImDrawCmd*)
		{
			if (ImGuiRenderState* state = GetRenderState())
				state->NearestFiltering = false;
		}

		void DrawCallbackSetSamplerNearest(const ImDrawList*, const ImDrawCmd*)
		{
			if (ImGuiRenderState* state = GetRenderState())
				state->NearestFiltering = true;
		}

		size_t AlignTo4(size_t value)
		{
			return (value + 3) & ~static_cast<size_t>(3);
		}

	}

	ImGuiRenderer::~ImGuiRenderer()
	{
		Shutdown();
	}

	bool ImGuiRenderer::Init(nvrhi::IDevice* device, ShaderLibrary& shaders)
	{
		m_VertexShader = shaders.Get("ImGui.vert");
		m_PixelShader = shaders.Get("ImGui.frag");
		if (!m_VertexShader || !m_PixelShader)
		{
			m_VertexShader = nullptr;
			m_PixelShader = nullptr;
			return false;
		}
		m_Device = device;

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, pos)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, uv)).setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert, col)).setElementStride(sizeof(ImDrawVert))
		};
		m_InputLayout = m_Device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_VertexShader);

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PushConstants)),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0)
		};
		m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_LinearSampler = m_Device->createSampler(samplerDesc);
		samplerDesc.setAllFilters(false);
		m_NearestSampler = m_Device->createSampler(samplerDesc);

		ImGuiIO& io = ImGui::GetIO();
		io.BackendRendererName = "Strata NVRHI";
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.DrawCallback_ResetRenderState = DrawCallbackResetRenderState;
		platformIO.DrawCallback_SetSamplerLinear = DrawCallbackSetSamplerLinear;
		platformIO.DrawCallback_SetSamplerNearest = DrawCallbackSetSamplerNearest;

		if (!m_InputLayout || !m_BindingLayout || !m_LinearSampler || !m_NearestSampler)
		{
			Shutdown();
			return false;
		}
		return true;
	}

	void ImGuiRenderer::Shutdown()
	{
		if (!m_Device)
			return;

		if (ImGui::GetCurrentContext())
		{
			for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
			{
				if (texture->RefCount == 1)
					DestroyTexture(texture);
			}

			ImGuiIO& io = ImGui::GetIO();
			io.BackendRendererName = nullptr;
			io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);

			ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			platformIO.DrawCallback_ResetRenderState = nullptr;
			platformIO.DrawCallback_SetSamplerLinear = nullptr;
			platformIO.DrawCallback_SetSamplerNearest = nullptr;
		}

		m_BindingSets.clear();
		m_Pipelines.clear();
		m_VertexBuffer = nullptr;
		m_IndexBuffer = nullptr;
		m_VertexCapacity = 0;
		m_IndexCapacity = 0;
		m_InputLayout = nullptr;
		m_BindingLayout = nullptr;
		m_LinearSampler = nullptr;
		m_NearestSampler = nullptr;
		m_VertexShader = nullptr;
		m_PixelShader = nullptr;
		m_Device = nullptr;
	}

	void ImGuiRenderer::UpdateTexture(nvrhi::ICommandList* commandList, ImTextureData* texture)
	{
		if (texture->Status == ImTextureStatus_WantDestroy)
		{
			// NVRHI keeps the texture alive for command lists that still reference it.
			DestroyTexture(texture);
			return;
		}

		if (texture->Status != ImTextureStatus_WantCreate && texture->Status != ImTextureStatus_WantUpdates)
			return;

		ST_CORE_ASSERT(texture->Format == ImTextureFormat_RGBA32, "ImGui textures are expected in RGBA32");
		auto* managed = static_cast<ManagedTexture*>(texture->BackendUserData);
		if (texture->Status == ImTextureStatus_WantCreate)
		{
			if (!managed)
			{
				managed = new ManagedTexture();
				texture->BackendUserData = managed;
			}

			nvrhi::TextureDesc desc;
			desc.width = static_cast<uint32_t>(texture->Width);
			desc.height = static_cast<uint32_t>(texture->Height);
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "ImGuiTexture";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			managed->Texture = m_Device->createTexture(desc);
			texture->SetTexID(ToTextureID(managed->Texture));
		}

		// Updates rewrite the whole texture: atlas updates are rare (new glyphs) and NVRHI uploads whole mips.
		if (managed && managed->Texture)
			commandList->writeTexture(managed->Texture, 0, 0, texture->GetPixels(), static_cast<size_t>(texture->GetPitch()));
		texture->SetStatus(ImTextureStatus_OK);
	}

	void ImGuiRenderer::DestroyTexture(ImTextureData* texture)
	{
		delete static_cast<ManagedTexture*>(texture->BackendUserData);
		texture->BackendUserData = nullptr;
		texture->SetTexID(ImTextureID_Invalid);
		texture->SetStatus(ImTextureStatus_Destroyed);
	}

	nvrhi::IGraphicsPipeline* ImGuiRenderer::GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo)
	{
		for (const auto& [info, pipeline] : m_Pipelines)
		{
			if (info == framebufferInfo)
				return pipeline;
		}

		nvrhi::BlendState::RenderTarget blend;
		blend.setBlendEnable(true)
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.inputLayout = m_InputLayout;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.bindingLayouts = { m_BindingLayout };
		desc.renderState.rasterState.setCullNone().setScissorEnable(true);
		desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
		desc.renderState.blendState.setRenderTarget(0, blend);

		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		m_Pipelines.emplace_back(framebufferInfo, pipeline);
		return pipeline;
	}

	nvrhi::IBindingSet* ImGuiRenderer::GetBindingSet(nvrhi::ITexture* texture, bool nearestFiltering)
	{
		const uintptr_t key = reinterpret_cast<uintptr_t>(texture) | (nearestFiltering ? 1u : 0u);
		auto it = m_BindingSets.find(key);
		if (it != m_BindingSets.end())
		{
			it->second.LastUsedFrame = m_FrameCounter;
			return it->second.BindingSet;
		}

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::PushConstants(0, sizeof(PushConstants)),
			nvrhi::BindingSetItem::Texture_SRV(0, texture),
			nvrhi::BindingSetItem::Sampler(0, nearestFiltering ? m_NearestSampler : m_LinearSampler)
		};
		nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(desc, m_BindingLayout);
		m_BindingSets.emplace(key, CachedBindingSet { bindingSet, m_FrameCounter });
		return bindingSet;
	}

	bool ImGuiRenderer::ReserveBuffers(size_t vertexCount, size_t indexCount)
	{
		if (vertexCount > m_VertexCapacity || !m_VertexBuffer)
		{
			m_VertexCapacity = std::max<size_t>(vertexCount + vertexCount / 2, 4096);
			nvrhi::BufferDesc desc;
			desc.byteSize = m_VertexCapacity * sizeof(ImDrawVert);
			desc.debugName = "ImGuiVertexBuffer";
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			m_VertexBuffer = m_Device->createBuffer(desc);
		}

		if (indexCount > m_IndexCapacity || !m_IndexBuffer)
		{
			m_IndexCapacity = std::max<size_t>(indexCount + indexCount / 2, 8192);
			nvrhi::BufferDesc desc;
			// Buffer updates are done in multiples of 4 bytes (vkCmdUpdateBuffer), which 16-bit indices may not be.
			desc.byteSize = AlignTo4(m_IndexCapacity * sizeof(ImDrawIdx));
			desc.debugName = "ImGuiIndexBuffer";
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			m_IndexBuffer = m_Device->createBuffer(desc);
		}

		return m_VertexBuffer && m_IndexBuffer;
	}

	void ImGuiRenderer::Render(nvrhi::ICommandList* commandList, ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer)
	{
		ST_PROFILE_FUNCTION();
		if (!drawData || !m_Device)
			return;

		m_FrameCounter++;
		std::erase_if(m_BindingSets, [this](const auto& entry) { return m_FrameCounter - entry.second.LastUsedFrame > c_BindingSetRetentionFrames; });

		if (drawData->Textures)
		{
			for (ImTextureData* texture : *drawData->Textures)
			{
				if (texture->Status != ImTextureStatus_OK)
					UpdateTexture(commandList, texture);
			}
		}

		const float framebufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float framebufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (framebufferWidth <= 0.0f || framebufferHeight <= 0.0f || drawData->TotalVtxCount == 0)
			return;

		if (!ReserveBuffers(static_cast<size_t>(drawData->TotalVtxCount), static_cast<size_t>(drawData->TotalIdxCount)))
			return;

		// Sizes are padded to 4 bytes: NVRHI uploads small buffers with vkCmdUpdateBuffer, which reads whole dwords.
		m_VertexStaging.resize(AlignTo4(static_cast<size_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert)));
		m_IndexStaging.resize(AlignTo4(static_cast<size_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx)));
		size_t vertexOffset = 0;
		size_t indexOffset = 0;
		for (const ImDrawList* drawList : drawData->CmdLists)
		{
			const size_t vertexBytes = static_cast<size_t>(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
			const size_t indexBytes = static_cast<size_t>(drawList->IdxBuffer.Size) * sizeof(ImDrawIdx);
			std::memcpy(m_VertexStaging.data() + vertexOffset, drawList->VtxBuffer.Data, vertexBytes);
			std::memcpy(m_IndexStaging.data() + indexOffset, drawList->IdxBuffer.Data, indexBytes);
			vertexOffset += vertexBytes;
			indexOffset += indexBytes;
		}
		commandList->writeBuffer(m_VertexBuffer, m_VertexStaging.data(), m_VertexStaging.size());
		commandList->writeBuffer(m_IndexBuffer, m_IndexStaging.data(), m_IndexStaging.size());

		// Pixel coordinates (y down, origin at DisplayPos) to NDC (+Y up).
		PushConstants pushConstants;
		pushConstants.Scale[0] = 2.0f / drawData->DisplaySize.x;
		pushConstants.Scale[1] = -2.0f / drawData->DisplaySize.y;
		pushConstants.Translate[0] = -1.0f - drawData->DisplayPos.x * pushConstants.Scale[0];
		pushConstants.Translate[1] = 1.0f + drawData->DisplayPos.y * 2.0f / drawData->DisplaySize.y;

		nvrhi::GraphicsState state;
		state.pipeline = GetPipeline(framebuffer->getFramebufferInfo());
		state.framebuffer = framebuffer;
		state.viewport.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight));
		state.viewport.addScissorRect(nvrhi::Rect(0, 0, 0, 0));
		state.vertexBuffers = { nvrhi::VertexBufferBinding { m_VertexBuffer, 0, 0 } };
		state.indexBuffer = nvrhi::IndexBufferBinding { m_IndexBuffer, sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT, 0 };

		const ImVec2 clipOffset = drawData->DisplayPos;
		const ImVec2 clipScale = drawData->FramebufferScale;
		uint32_t globalVertexOffset = 0;
		uint32_t globalIndexOffset = 0;

		ImGuiRenderState renderState;
		renderState.CommandList = commandList;
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Renderer_RenderState = &renderState;
		for (const ImDrawList* drawList : drawData->CmdLists)
		{
			for (const ImDrawCmd& command : drawList->CmdBuffer)
			{
				if (command.UserCallback)
				{
					command.UserCallback(drawList, &command);
					continue;
				}

				const float clipMinX = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0f);
				const float clipMinY = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0f);
				const float clipMaxX = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, framebufferWidth);
				const float clipMaxY = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, framebufferHeight);
				if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
					continue;

				auto* texture = reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(command.GetTexID()));
				if (!texture)
					continue;

				state.bindings = { GetBindingSet(texture, renderState.NearestFiltering) };
				state.viewport.scissorRects[0] = nvrhi::Rect(static_cast<int>(clipMinX), static_cast<int>(clipMaxX), static_cast<int>(clipMinY), static_cast<int>(clipMaxY));
				commandList->setGraphicsState(state);
				commandList->setPushConstants(&pushConstants, sizeof(pushConstants));

				nvrhi::DrawArguments arguments;
				arguments.vertexCount = command.ElemCount;
				arguments.startIndexLocation = command.IdxOffset + globalIndexOffset;
				arguments.startVertexLocation = command.VtxOffset + globalVertexOffset;
				commandList->drawIndexed(arguments);
			}
			globalIndexOffset += static_cast<uint32_t>(drawList->IdxBuffer.Size);
			globalVertexOffset += static_cast<uint32_t>(drawList->VtxBuffer.Size);
		}
		platformIO.Renderer_RenderState = nullptr;
	}

}
