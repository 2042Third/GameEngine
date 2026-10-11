#include "stpch.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/Texture.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace Strata
{

	namespace
	{

		struct PushConstants
		{
			float Scale[2];
			float Translate[2];
			uint32_t TextureDisplay; // ImGuiTextureDisplay (ImGui.frag)
		};

		// Backend storage attached to ImGui-managed textures (font atlas pages).
		struct ManagedTexture
		{
			nvrhi::TextureHandle Texture;
		};

		// Binding sets unused for this many frames are released (more than any number of frames in flight).
		constexpr uint64_t c_BindingSetRetentionFrames = 8;
		// Sets of vertex and index buffers at most (more than any number of frames in flight), and their smallest sizes.
		constexpr size_t c_MaxFrameBufferSets = 8;
		constexpr size_t c_MinVertexCapacity = 4096;
		constexpr size_t c_MinIndexCapacity = 8192;

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
			{
				state->NearestFiltering = false;
				state->TextureDisplay = ImGuiTextureDisplay::Direct;
			}
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

		// The callbacks of AddImGuiTextureDisplay point their user data at the display they set, one of these: the commands
		// are told apart by the address of their data, never by the callback's (the linker may merge identical functions).
		constexpr ImGuiTextureDisplay c_TextureDisplays[] = { ImGuiTextureDisplay::Direct, ImGuiTextureDisplay::EncodeSRGB, ImGuiTextureDisplay::Grayscale,
			ImGuiTextureDisplay::NormalXY };

		void DrawCallbackSetTextureDisplay(const ImDrawList*, const ImDrawCmd* command)
		{
			ImGuiRenderState* state = GetRenderState();
			if (state && command->UserCallbackData)
				state->TextureDisplay = *static_cast<const ImGuiTextureDisplay*>(command->UserCallbackData);
		}

	}

	ImGuiTextureDisplay GetImGuiTextureDisplay(TextureFormat format, bool grayscale)
	{
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		if (grayscale && info.Channels == 1)
			return ImGuiTextureDisplay::Grayscale;
		if (info.Channels == 2)
			return ImGuiTextureDisplay::NormalXY;
		return info.IsSRGB ? ImGuiTextureDisplay::EncodeSRGB : ImGuiTextureDisplay::Direct;
	}

	ImGuiTextureDisplay GetImGuiTextureDisplay(const TextureSpecification& specification)
	{
		return GetImGuiTextureDisplay(specification.Format, specification.Grayscale);
	}

	void AddImGuiTextureDisplay(ImDrawList* drawList, ImGuiTextureDisplay display)
	{
		const size_t index = std::min<size_t>(static_cast<size_t>(display), std::size(c_TextureDisplays) - 1);
		// ImGui keeps the pointer (a user data size of 0) and never writes through it.
		drawList->AddCallback(DrawCallbackSetTextureDisplay, const_cast<ImGuiTextureDisplay*>(&c_TextureDisplays[index]));
	}

	std::optional<ImGuiTextureDisplay> FindImGuiTextureDisplay(const ImDrawCmd& command)
	{
		if (!command.UserCallback)
			return std::nullopt;
		for (const ImGuiTextureDisplay& display : c_TextureDisplays)
		{
			if (command.UserCallbackData == &display)
				return display;
		}
		return std::nullopt;
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
		// Command lists in flight keep the buffers they draw from alive (NVRHI references them).
		for (FrameBuffers& buffers : m_FrameBuffers)
			ReleaseFrameBuffers(buffers);
		m_FrameBuffers.clear();
		m_DrawnFrameBuffers.reset();
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

	ImGuiRenderer::FrameBuffers* ImGuiRenderer::AcquireFrameBuffers(size_t vertexCount, size_t indexCount)
	{
		// The first set the GPU is done with. While every set is in use (frames in flight), another one, up to a bound: past
		// it the GPU is that many frames behind, and waiting for its oldest frame beats taking more memory.
		FrameBuffers* acquired = nullptr;
		for (FrameBuffers& buffers : m_FrameBuffers)
		{
			if (!buffers.Pending || m_Device->pollEventQuery(buffers.Done))
			{
				acquired = &buffers;
				break;
			}
		}
		if (!acquired && m_FrameBuffers.size() < c_MaxFrameBufferSets)
		{
			FrameBuffers& added = m_FrameBuffers.emplace_back();
			added.Done = m_Device->createEventQuery();
			acquired = &added;
		}
		if (!acquired)
		{
			acquired = &*std::min_element(m_FrameBuffers.begin(), m_FrameBuffers.end(),
				[](const FrameBuffers& a, const FrameBuffers& b) { return a.LastUsedFrame < b.LastUsedFrame; });
			m_Device->waitEventQuery(acquired->Done);
		}
		acquired->Pending = false;
		acquired->LastUsedFrame = m_FrameCounter;
		return ReserveFrameBuffers(*acquired, vertexCount, indexCount) ? acquired : nullptr;
	}

	bool ImGuiRenderer::ReserveFrameBuffers(FrameBuffers& buffers, size_t vertexCount, size_t indexCount)
	{
		// The GPU is done with the set: its buffers can be replaced. They grow by half again, so a growing UI replaces them
		// rarely.
		if (vertexCount > buffers.VertexCapacity || !buffers.MappedVertices)
		{
			if (buffers.Vertices && buffers.MappedVertices)
				m_Device->unmapBuffer(buffers.Vertices);
			buffers.MappedVertices = nullptr;
			buffers.VertexCapacity = std::max<size_t>(vertexCount + vertexCount / 2, c_MinVertexCapacity);
			nvrhi::BufferDesc desc;
			desc.byteSize = buffers.VertexCapacity * sizeof(ImDrawVert);
			desc.debugName = "ImGuiVertexBuffer";
			desc.isVertexBuffer = true;
			desc.cpuAccess = nvrhi::CpuAccessMode::Write;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			buffers.Vertices = m_Device->createBuffer(desc);
			if (buffers.Vertices)
				buffers.MappedVertices = static_cast<uint8_t*>(m_Device->mapBuffer(buffers.Vertices, nvrhi::CpuAccessMode::Write));
		}

		if (indexCount > buffers.IndexCapacity || !buffers.MappedIndices)
		{
			if (buffers.Indices && buffers.MappedIndices)
				m_Device->unmapBuffer(buffers.Indices);
			buffers.MappedIndices = nullptr;
			buffers.IndexCapacity = std::max<size_t>(indexCount + indexCount / 2, c_MinIndexCapacity);
			nvrhi::BufferDesc desc;
			desc.byteSize = buffers.IndexCapacity * sizeof(ImDrawIdx);
			desc.debugName = "ImGuiIndexBuffer";
			desc.isIndexBuffer = true;
			desc.cpuAccess = nvrhi::CpuAccessMode::Write;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			buffers.Indices = m_Device->createBuffer(desc);
			if (buffers.Indices)
				buffers.MappedIndices = static_cast<uint8_t*>(m_Device->mapBuffer(buffers.Indices, nvrhi::CpuAccessMode::Write));
		}

		if (!buffers.MappedVertices || !buffers.MappedIndices)
		{
			// A failed creation (out of memory) is tried again next frame.
			ReleaseFrameBuffers(buffers);
			return false;
		}
		return true;
	}

	void ImGuiRenderer::ReleaseFrameBuffers(FrameBuffers& buffers)
	{
		if (buffers.Vertices && buffers.MappedVertices)
			m_Device->unmapBuffer(buffers.Vertices);
		if (buffers.Indices && buffers.MappedIndices)
			m_Device->unmapBuffer(buffers.Indices);
		buffers.Vertices = nullptr;
		buffers.Indices = nullptr;
		buffers.MappedVertices = nullptr;
		buffers.MappedIndices = nullptr;
		buffers.VertexCapacity = 0;
		buffers.IndexCapacity = 0;
	}

	void ImGuiRenderer::Render(nvrhi::ICommandList* commandList, ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer)
	{
		ST_PROFILE_FUNCTION();
		if (!drawData || !m_Device)
			return;

		m_FrameCounter++;
		std::erase_if(m_BindingSets, [this](const auto& entry) { return m_FrameCounter - entry.second.LastUsedFrame > c_BindingSetRetentionFrames; });

		// The previous call's draws were submitted since (see Render): the GPU is done with their buffers once it finished
		// everything submitted so far.
		if (m_DrawnFrameBuffers)
		{
			FrameBuffers& drawn = m_FrameBuffers[*m_DrawnFrameBuffers];
			m_Device->resetEventQuery(drawn.Done);
			m_Device->setEventQuery(drawn.Done, nvrhi::CommandQueue::Graphics);
			drawn.Pending = true;
			m_DrawnFrameBuffers.reset();
		}

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

		FrameBuffers* buffers = AcquireFrameBuffers(static_cast<size_t>(drawData->TotalVtxCount), static_cast<size_t>(drawData->TotalIdxCount));
		if (!buffers)
			return;
		size_t vertexOffset = 0;
		size_t indexOffset = 0;
		for (const ImDrawList* drawList : drawData->CmdLists)
		{
			const size_t vertexBytes = static_cast<size_t>(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
			const size_t indexBytes = static_cast<size_t>(drawList->IdxBuffer.Size) * sizeof(ImDrawIdx);
			std::memcpy(buffers->MappedVertices + vertexOffset, drawList->VtxBuffer.Data, vertexBytes);
			std::memcpy(buffers->MappedIndices + indexOffset, drawList->IdxBuffer.Data, indexBytes);
			vertexOffset += vertexBytes;
			indexOffset += indexBytes;
		}
		m_DrawnFrameBuffers = static_cast<size_t>(buffers - m_FrameBuffers.data());

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
		state.vertexBuffers = { nvrhi::VertexBufferBinding { buffers->Vertices, 0, 0 } };
		state.indexBuffer = nvrhi::IndexBufferBinding { buffers->Indices, sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT, 0 };

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
				pushConstants.TextureDisplay = static_cast<uint32_t>(renderState.TextureDisplay);
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
