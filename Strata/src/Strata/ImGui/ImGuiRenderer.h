#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/TextureFormat.h"

#include <nvrhi/nvrhi.h>

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

struct ImDrawCmd;
struct ImDrawData;
struct ImDrawList;
struct ImTextureData;

namespace Strata
{

	class ShaderLibrary;
	struct TextureSpecification;

	// How ImGui draws show their textures. Textures are sampled as they are stored; for the layouts the engine cooks that
	// would look wrong as they are, these show what the texture holds (what materials read from it).
	enum class ImGuiTextureDisplay : uint32_t
	{
		Direct = 0, // As sampled: UI textures, and images that hold display colors (the viewport's)
		EncodeSRGB, // sRGB textures: sampling decodes them to linear values, encoded again for the display
		Grayscale,  // One channel that stands for gray (TextureSpecification::Grayscale, BC4/R8): (r, r, r, 1)
		NormalXY    // Two-channel normal maps (BC5/RG8): X and Y, with Z reconstructed, shown as normal maps are (n * 0.5 + 0.5)
	};

	// The display that shows a texture of this specification as its content.
	ImGuiTextureDisplay GetImGuiTextureDisplay(const TextureSpecification& specification);
	ImGuiTextureDisplay GetImGuiTextureDisplay(TextureFormat format, bool grayscale);

	// Adds a draw callback to the list that makes its following draws show their textures in `display`, until another such
	// callback or ImGui's DrawCallback_ResetRenderState. Without the engine's ImGui renderer (UI tests without a GPU) it
	// changes nothing.
	void AddImGuiTextureDisplay(ImDrawList* drawList, ImGuiTextureDisplay display);
	// The display a draw command added by AddImGuiTextureDisplay sets (tests read draw lists with it); nullopt for other
	// commands.
	std::optional<ImGuiTextureDisplay> FindImGuiTextureDisplay(const ImDrawCmd& command);

	// Render state exposed to ImGui draw callbacks while rendering, through
	// static_cast<ImGuiRenderState*>(ImGui::GetPlatformIO().Renderer_RenderState).
	struct ImGuiRenderState
	{
		nvrhi::ICommandList* CommandList = nullptr;
		bool NearestFiltering = false; // Set by the standard DrawCallback_SetSamplerNearest/Linear callbacks
		ImGuiTextureDisplay TextureDisplay = ImGuiTextureDisplay::Direct; // Set by AddImGuiTextureDisplay's callbacks
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

		// Records all draw commands into commandList, rendering into the framebuffer (any color format). Execute the command
		// list before the next call: the draws read their vertices from memory the renderer writes again once the GPU has
		// finished the work submitted before that call.
		void Render(nvrhi::ICommandList* commandList, ImDrawData* drawData, nvrhi::IFramebuffer* framebuffer);
		// How many sets of vertex and index buffers the renderer has (FrameBuffers): one per frame the GPU was still
		// drawing when another began. For tests.
		size_t GetFrameBufferSetCount() const { return m_FrameBuffers.size(); }
	private:
		// Vertex and index buffers that the CPU writes and the GPU reads in place: mapped once, the draw lists are copied
		// straight into them, with no staging copy and no GPU copy (a frame's UI is a few hundred kilobytes). A set is
		// written again only once the GPU is done with the frame that drew from it (Done).
		struct FrameBuffers
		{
			nvrhi::BufferHandle Vertices;
			nvrhi::BufferHandle Indices;
			uint8_t* MappedVertices = nullptr;
			uint8_t* MappedIndices = nullptr;
			size_t VertexCapacity = 0; // In vertices
			size_t IndexCapacity = 0;  // In indices
			nvrhi::EventQueryHandle Done; // Set once the frame that drew from the set was submitted
			bool Pending = false;          // Done was set and may not have completed yet
			uint64_t LastUsedFrame = 0;
		};

		void UpdateTexture(nvrhi::ICommandList* commandList, ImTextureData* texture);
		void DestroyTexture(ImTextureData* texture);
		nvrhi::IGraphicsPipeline* GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo);
		nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture, bool nearestFiltering);
		// A set the GPU is done with, holding at least these counts; null if a buffer could not be created.
		FrameBuffers* AcquireFrameBuffers(size_t vertexCount, size_t indexCount);
		bool ReserveFrameBuffers(FrameBuffers& buffers, size_t vertexCount, size_t indexCount);
		void ReleaseFrameBuffers(FrameBuffers& buffers);
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

		std::vector<FrameBuffers> m_FrameBuffers;
		// The set the previous Render drew from: its Done query is set at the next Render, once its draws were submitted.
		std::optional<size_t> m_DrawnFrameBuffers;
	};

}
