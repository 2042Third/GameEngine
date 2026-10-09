#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <unordered_map>
#include <utility>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace Strata
{

	class ShaderLibrary;

	// Render state exposed to ImGui draw callbacks while rendering, through
	// static_cast<ImGuiRenderState*>(ImGui::GetPlatformIO().Renderer_RenderState).
	struct ImGuiRenderState
	{
		nvrhi::ICommandList* CommandList = nullptr;
		bool NearestFiltering = false; // Set by the standard DrawCallback_SetSamplerNearest/Linear callbacks
	};

	// Dear ImGui renderer backend on NVRHI. Supports ImGui's dynamic texture protocol (font atlas creation and
	// updates) and user textures: pass an nvrhi::ITexture* as the ImTextureID, e.g.
	// ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), size). User textures must stay
	// alive until the frame's draw data has been rendered; the renderer itself keeps them alive for a few frames after
	// their last use (cached binding sets).
	class ImGuiRenderer
	{
	public:
		ImGuiRenderer() = default;
		~ImGuiRenderer();

		bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders);
		void Shutdown();

		// Records all draw commands into commandList, rendering into the framebuffer (any color format).
		void Render(nvrhi::ICommandList* commandList, ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer);
	private:
		void UpdateTexture(nvrhi::ICommandList* commandList, ImTextureData* texture);
		void DestroyTexture(ImTextureData* texture);
		nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture, bool nearestFiltering);
		bool ReserveBuffers(size_t vertexCount, size_t indexCount);
	private:
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::BindingLayoutHandle m_BindingLayout;
		nvrhi::SamplerHandle m_LinearSampler;
		nvrhi::SamplerHandle m_NearestSampler;

		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_Pipelines;

		// Keyed by texture pointer with the low bit marking nearest filtering. A binding set holds a reference to its
		// texture, so a cached pointer can never be reused by another texture; entries unused for a few frames are
		// dropped, releasing the texture.
		struct CachedBindingSet
		{
			nvrhi::BindingSetHandle BindingSet;
			uint64_t LastUsedFrame = 0;
		};
		std::unordered_map<uintptr_t, CachedBindingSet> m_BindingSets;
		uint64_t m_FrameCounter = 0;

		nvrhi::BufferHandle m_VertexBuffer;
		nvrhi::BufferHandle m_IndexBuffer;
		size_t m_VertexCapacity = 0;
		size_t m_IndexCapacity = 0;
		std::vector<uint8_t> m_VertexStaging;
		std::vector<uint8_t> m_IndexStaging;
	};

}
