#include "stpch.h"
#include "Strata/Renderer/TextRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace Strata
{

	// A glyph may overdraw the frame budget by its own cost: keep that within a few budgets.
	static_assert(FontAtlas::c_MaxGlyphRasterCost <= 4 * TextRenderer::c_FrameRasterBudget.Cost);

	TextRenderer::TextRenderer(const std::string& debugName, nvrhi::IBuffer* frameConstants)
		: m_DebugName(debugName), m_Errors("TextRenderer '" + debugName + "'"), m_FrameConstants(frameConstants)
	{
		ST_CORE_VERIFY(Renderer::IsInitialized() && frameConstants, "TextRenderer requires an initialized renderer and frame constants");
		m_Device = Renderer::GetDevice();
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		m_VertexShader = shaders.Get("Overlay/Text.vert");
		m_PixelShader = shaders.Get("Overlay/Text.frag");
		ST_CORE_VERIFY(m_VertexShader && m_PixelShader, "TextRenderer shaders are missing");

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
				.setOffset(offsetof(TextVertex, Position)).setElementStride(sizeof(TextVertex)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
				.setOffset(offsetof(TextVertex, TexCoord)).setElementStride(sizeof(TextVertex)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setBufferIndex(0)
				.setOffset(offsetof(TextVertex, Color)).setElementStride(sizeof(TextVertex))
		};
		m_InputLayout = m_Device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_VertexShader);

		// Slot 1 for the push constants: register b0 belongs to the frame constants.
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(15),
			nvrhi::BindingLayoutItem::Sampler(15),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::TextParameters))
		};
		m_BindingLayout = m_Device->createBindingLayout(layout);
		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_Sampler = m_Device->createSampler(samplerDesc);
		ST_CORE_VERIFY(m_InputLayout && m_BindingLayout && m_Sampler, "TextRenderer: failed to create GPU resources");
	}

	TextRenderer::~TextRenderer() = default;

	TextRenderer::CachedAtlas* TextRenderer::GetAtlas(const Ref<Font>& font)
	{
		auto it = m_Atlases.find(font.get());
		if (it == m_Atlases.end())
		{
			std::string error;
			CachedAtlas cached;
			cached.Atlas = FontAtlas::Create(font, &error);
			if (!cached.Atlas)
				m_Errors.Report(fmt::format("cannot use a font: {}", error)); // Cached as unusable, so not retried every frame
			it = m_Atlases.emplace(font.get(), std::move(cached)).first;
		}
		it->second.Used = true;
		return it->second.Atlas ? &it->second : nullptr;
	}

	void TextRenderer::AppendQuads(CachedAtlas& atlas, const glm::mat4& transform, const glm::vec4& color, bool screenSpace)
	{
		std::vector<DrawRange>& ranges = screenSpace ? m_ScreenRanges : m_WorldRanges;
		const uint32_t firstVertex = static_cast<uint32_t>(m_Vertices.size());
		const uint32_t packedColor = DebugDraw::PackColor(color);
		for (const TextGlyphQuad& quad : m_Layout.Quads)
		{
			// Texture coordinates stay in texels of the glyph's page (the layer of the atlas texture array).
			const glm::vec2 texelMin(quad.AtlasPosition);
			const glm::vec2 texelMax = texelMin + glm::vec2(quad.AtlasSize);
			const float page = static_cast<float>(quad.Page);
			auto vertex = [&](float x, float y, float u, float v)
			{
				m_Vertices.push_back(TextVertex { glm::vec3(transform * glm::vec4(x, y, 0.0f, 1.0f)), glm::vec3(u, v, page), packedColor });
			};
			// Em space is +Y up; atlas rows go down.
			vertex(quad.Min.x, quad.Min.y, texelMin.x, texelMax.y);
			vertex(quad.Max.x, quad.Min.y, texelMax.x, texelMax.y);
			vertex(quad.Max.x, quad.Max.y, texelMax.x, texelMin.y);
			vertex(quad.Min.x, quad.Min.y, texelMin.x, texelMax.y);
			vertex(quad.Max.x, quad.Max.y, texelMax.x, texelMin.y);
			vertex(quad.Min.x, quad.Max.y, texelMin.x, texelMin.y);
		}

		const uint32_t vertexCount = static_cast<uint32_t>(m_Vertices.size()) - firstVertex;
		if (!ranges.empty() && ranges.back().Atlas == &atlas && ranges.back().FirstVertex + ranges.back().VertexCount == firstVertex)
			ranges.back().VertexCount += vertexCount;
		else
			ranges.push_back(DrawRange { &atlas, firstVertex, vertexCount });
	}

	bool TextRenderer::Prepare(Scene& scene, const glm::uvec2& viewportSize, bool drawScreenSpace, nvrhi::ICommandList* commandList, TextRenderStats& outStats)
	{
		m_Vertices.clear();
		m_WorldRanges.clear();
		m_ScreenRanges.clear();
		m_ViewportSize = glm::max(glm::vec2(viewportSize), glm::vec2(1.0f));
		for (auto& [font, cached] : m_Atlases)
		{
			cached.Used = false;
			if (cached.Atlas)
				cached.Atlas->BeginFrame();
		}
		// One budget for every font: each atlas receives what the previous ones left.
		GlyphRasterBudget budget = c_FrameRasterBudget;

		for (auto [entity, text, world] : scene.GetRegistry().view<TextComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || text.Text.empty() || !(text.FontSize > 0.0f) || !std::isfinite(text.FontSize) || !(text.Color.a > 0.0f))
				continue;
			if (text.ScreenSpace && !drawScreenSpace)
			{
				outStats.HiddenScreenSpaceTexts++;
				continue;
			}

			// The default font stands in while the text's own font loads (or when it cannot be loaded).
			Ref<Font> font;
			if (text.Font.IsValid())
			{
				font = AssetManager::GetAsset<Font>(text.Font, AssetPriority::High);
				const AssetState state = AssetManager::GetAssetState(text.Font);
				if (!font && AssetManager::IsHandleValid(text.Font) && state != AssetState::Failed && state != AssetState::Ready)
					outStats.PendingFonts++;
			}
			CachedAtlas* atlas = GetAtlas(font ? font : Font::GetDefault());
			if (!atlas)
				continue;
			atlas->Atlas->SetRasterBudget(budget);
			LayoutText(*atlas->Atlas, text.Text, text.Alignment, m_Layout);
			budget = atlas->Atlas->GetRasterBudget();
			outStats.PendingGlyphs += m_Layout.PendingGlyphs;
			if (m_Layout.Quads.empty())
				continue;

			const float size = text.FontSize;
			glm::mat4 transform;
			if (text.ScreenSpace)
			{
				// The anchor is also the pivot: (0, 0) puts the block's top-left corner on it, (1, 1) its bottom-right
				// (horizontally the alignment decides). Whole pixels keep the glyphs crisp.
				const glm::vec2 anchor = text.ScreenAnchor * m_ViewportSize + text.ScreenOffset;
				const float blockHeight = (m_Layout.Max.y - m_Layout.Min.y) * size;
				const glm::vec2 origin(std::round(anchor.x), std::round(anchor.y - text.ScreenAnchor.y * blockHeight));
				transform = glm::translate(glm::mat4(1.0f), glm::vec3(origin, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(size, -size, 1.0f));
			}
			else
			{
				// In the entity's XY plane (facing +Z), vertically centered on its origin, FontSize world units per em.
				transform = world.Matrix * glm::scale(glm::mat4(1.0f), glm::vec3(size)) * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -m_Layout.Min.y * 0.5f, 0.0f));
			}
			AppendQuads(*atlas, transform, text.Color, text.ScreenSpace);
			outStats.Texts++;
			if (text.ScreenSpace)
				outStats.ScreenSpaceTexts++;
			outStats.Glyphs += static_cast<uint32_t>(m_Layout.Quads.size());
		}
		outStats.RasterizedGlyphs += c_FrameRasterBudget.Glyphs - budget.Glyphs;

		// Atlases live as long as their font asset: once only the cache still holds a font, it is gone from the scene's
		// asset manager (unloaded or replaced by a reload) and its atlas is dropped.
		for (auto it = m_Atlases.begin(); it != m_Atlases.end();)
		{
			const bool orphaned = !it->second.Used && (!it->second.Atlas || it->second.Atlas->GetFont().use_count() == 1) && it->first != Font::GetDefault().get();
			it = orphaned ? m_Atlases.erase(it) : std::next(it);
		}
		if (m_Vertices.empty())
			return false;

		for (const std::vector<DrawRange>* ranges : { &m_WorldRanges, &m_ScreenRanges })
		{
			for (const DrawRange& range : *ranges)
			{
				CachedAtlas& cached = *range.Atlas;
				if (!cached.Atlas->Upload(m_Device, commandList))
				{
					m_Errors.Report("failed to create a glyph atlas texture");
					return false;
				}
				if (cached.BoundTexture != cached.Atlas->GetTexture())
				{
					nvrhi::BindingSetDesc desc;
					desc.bindings = {
						nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstants),
						nvrhi::BindingSetItem::Texture_SRV(15, cached.Atlas->GetTexture()),
						nvrhi::BindingSetItem::Sampler(15, m_Sampler),
						nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::TextParameters))
					};
					cached.BindingSet = m_Device->createBindingSet(desc, m_BindingLayout);
					if (!cached.BindingSet)
					{
						m_Errors.Report("failed to create a glyph atlas binding set");
						return false;
					}
					cached.BoundTexture = cached.Atlas->GetTexture();
				}
			}
		}

		const uint64_t requiredBytes = static_cast<uint64_t>(m_Vertices.size()) * sizeof(TextVertex);
		if (!m_VertexBuffer || m_VertexBuffer->getDesc().byteSize < requiredBytes)
		{
			uint64_t capacity = m_VertexBuffer ? m_VertexBuffer->getDesc().byteSize : 6 * 256 * sizeof(TextVertex);
			while (capacity < requiredBytes)
				capacity *= 2;
			nvrhi::BufferDesc desc;
			desc.byteSize = capacity;
			desc.isVertexBuffer = true;
			desc.debugName = m_DebugName + ".TextVertices";
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
			if (!buffer)
			{
				m_Errors.Report(fmt::format("failed to allocate {} bytes of text vertices", capacity));
				return false;
			}
			m_VertexBuffer = buffer;
		}
		commandList->writeBuffer(m_VertexBuffer, m_Vertices.data(), requiredBytes);
		m_Errors.Clear();
		return true;
	}

	nvrhi::IGraphicsPipeline* TextRenderer::GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo, bool screenSpace)
	{
		for (const auto& [info, pipelines] : m_Pipelines)
		{
			if (info == framebufferInfo)
				return screenSpace ? pipelines.second : pipelines.first;
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
		desc.renderState.rasterState.setCullNone(); // World-space text reads mirrored from behind
		desc.renderState.blendState.setRenderTarget(0, blend);
		// World-space text is hidden behind scene geometry (reversed-Z) without changing the depth.
		desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
		nvrhi::GraphicsPipelineHandle world = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		desc.renderState.depthStencilState.setDepthTestEnable(false);
		nvrhi::GraphicsPipelineHandle screen = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		ST_CORE_VERIFY(world && screen, "TextRenderer: failed to create the text pipelines");
		m_Pipelines.emplace_back(framebufferInfo, std::pair { world, screen });
		return screenSpace ? screen.Get() : world.Get();
	}

	void TextRenderer::Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, bool screenSpace)
	{
		const std::vector<DrawRange>& ranges = screenSpace ? m_ScreenRanges : m_WorldRanges;
		if (ranges.empty())
			return;

		const RenderData::TextParameters parameters = { m_ViewportSize, screenSpace ? 1u : 0u, 0u };
		nvrhi::GraphicsState state;
		state.pipeline = GetPipeline(framebuffer->getFramebufferInfo(), screenSpace);
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(m_ViewportSize.x, m_ViewportSize.y));
		state.vertexBuffers = { nvrhi::VertexBufferBinding { m_VertexBuffer, 0, 0 } };
		for (const DrawRange& range : ranges)
		{
			state.bindings = { range.Atlas->BindingSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(range.VertexCount).setStartVertexLocation(range.FirstVertex));
		}
	}

}
