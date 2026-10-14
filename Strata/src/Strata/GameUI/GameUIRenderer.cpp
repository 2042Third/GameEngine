#include "stpch.h"
#include "Strata/GameUI/GameUIRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlasCache.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureFormat.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// Changed ranges this close together are uploaded as one.
		constexpr uint32_t c_RangeMergeGap = 16;

		glm::uvec2 PackGlyph(const GlyphInfo& glyph)
		{
			if (!glyph.Visible)
				return glm::uvec2(0u);
			return glm::uvec2(glyph.AtlasPosition.x | (glyph.AtlasPosition.y << 16), glyph.AtlasSize.x | (glyph.AtlasSize.y << 8) | (glyph.Page << 16));
		}

		bool IsPending(AssetManagerBase* assets, AssetHandle handle)
		{
			if (!assets || !assets->IsHandleValid(handle))
				return false;
			const AssetState state = assets->GetAssetState(handle);
			return state != AssetState::Failed && state != AssetState::Ready;
		}

		float GetContentScale()
		{
			IGameWindow* window = EngineServices::Get<IGameWindow>();
			const float scale = window ? window->GetState().ContentScale : 1.0f;
			return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
		}

	}

	void RegisterGameUIRenderer()
	{
		const bool registered = SceneOverlayRegistry::Register("GameUI", [](const SceneOverlayPassContext& context) -> Scope<ISceneOverlayPass>
		{
			return CreateScope<GameUIRenderer>(context);
		});
		ST_CORE_VERIFY(registered, "The game UI could not register its overlay pass");
	}

	GameUIRenderer::GameUIRenderer(const SceneOverlayPassContext& context)
		: m_DebugName(context.DebugName), m_Errors("GameUIRenderer '" + context.DebugName + "'")
	{
		ST_CORE_VERIFY(Renderer::IsInitialized(), "GameUIRenderer requires an initialized renderer");
		m_Device = Renderer::GetDevice();
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		m_VertexShader = shaders.Get("GameUI/UIQuad.vert");
		m_PixelShader = shaders.Get("GameUI/UIQuad.frag");
		ST_CORE_VERIFY(m_VertexShader && m_PixelShader, "GameUIRenderer shaders are missing");

		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.bindings = {
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Texture_SRV(6),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::UIDrawParameters))
		};
		m_BindingLayout = m_Device->createBindingLayout(layout);
		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_Sampler = m_Device->createSampler(samplerDesc);
		nvrhi::TextureDesc emptyDesc;
		emptyDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
		emptyDesc.width = 1;
		emptyDesc.height = 1;
		emptyDesc.arraySize = 1;
		emptyDesc.format = nvrhi::Format::R8_UNORM;
		emptyDesc.debugName = m_DebugName + ".UIEmptyAtlas";
		emptyDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		emptyDesc.keepInitialState = true;
		m_EmptyAtlas = m_Device->createTexture(emptyDesc);
		ST_CORE_VERIFY(m_BindingLayout && m_Sampler && m_EmptyAtlas, "GameUIRenderer: failed to create GPU resources");
	}

	GameUIRenderer::~GameUIRenderer() = default;

	bool GameUIRenderer::HasScreenContent(Scene& scene)
	{
		return GameUIScene::HasCanvases(scene) && GameUIScene::Get(scene).GetRootCanvasCount() > 0;
	}

	bool GameUIRenderer::EnsureBuffer(nvrhi::BufferHandle& buffer, uint64_t bytes, uint32_t stride, const char* name)
	{
		if (buffer && buffer->getDesc().byteSize >= bytes)
			return false;
		uint64_t capacity = buffer ? buffer->getDesc().byteSize : static_cast<uint64_t>(stride) * 256;
		while (capacity < bytes)
			capacity *= 2;
		nvrhi::BufferDesc desc;
		desc.byteSize = capacity;
		desc.structStride = stride;
		desc.debugName = m_DebugName + "." + name;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		// A replaced buffer stays alive while command lists that use it are in flight (they hold references).
		buffer = m_Device->createBuffer(desc);
		m_BindingSets.clear();
		return true;
	}

	bool GameUIRenderer::Prepare(const SceneOverlayFrame& frame)
	{
		m_Runs.clear();
		for (auto& [atlases, set] : m_BindingSets)
			set.second = false;
		Scene* scene = frame.OverlayScene;
		// A scene without canvases costs this check.
		if (!scene || !GameUIScene::HasCanvases(*scene))
			return false;
		GameUIScene& ui = GameUIScene::Get(*scene);
		SceneOverlayStats& stats = *frame.Stats;
		if (!frame.DrawScreenSpace)
		{
			// Hidden (editor views without the game UI): counted, not laid out.
			stats.HiddenCanvases += ui.GetRootCanvasCount();
			return false;
		}
		if (frame.ViewportSize.x == 0 || frame.ViewportSize.y == 0 || !frame.CommandList || !frame.Atlases)
			return false;

		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.UI");
		m_ViewportSize = glm::vec2(frame.ViewportSize);
		const UIScreen screen { m_ViewportSize, GetContentScale() };
		const std::span<const UICanvasView> views = ui.EnsureLayout(screen);
		if (views.empty())
			return false;

		nvrhi::ICommandList* commandList = frame.CommandList;
		if (!m_EmptyAtlasWritten)
		{
			const uint8_t zero = 0;
			commandList->writeTexture(m_EmptyAtlas, 0, 0, &zero, 1);
			m_EmptyAtlasWritten = true;
		}
		AssetManagerBase* assets = scene->GetAssetManager();
		if (!UploadQuads(ui, views, commandList, stats))
			return false;
		float maxScale = 0.0f;
		for (const UICanvasView& view : views)
			maxScale = std::max(maxScale, view.Scale);
		UpdateTextures(ui, frame, assets, maxScale);
		UpdateGlyphs(ui, frame, assets);
		if (!m_QuadBuffer || !m_TextureSlotBuffer || !m_GlyphBuffer)
			return false;
		BuildRuns(ui, views);

		for (const UICanvasView& view : views)
			stats.Elements += ui.GetElementCount(view);
		stats.Canvases += static_cast<uint32_t>(views.size());
		stats.DrawCalls += static_cast<uint32_t>(m_Runs.size());
		for (const UploadedCanvas& canvas : m_Uploaded)
			stats.Quads += canvas.Count;
		m_Errors.Clear();
		return !m_Runs.empty();
	}

	bool GameUIRenderer::UploadQuads(GameUIScene& ui, std::span<const UICanvasView> views, nvrhi::ICommandList* commandList, SceneOverlayStats& stats)
	{
		// The quads of every canvas view, in draw order, one after the other in the buffer.
		uint64_t total = 0;
		for (const UICanvasView& view : views)
			total += ui.EnsureQuads(view).size();
		const bool recreated = EnsureBuffer(m_QuadBuffer, std::max<uint64_t>(total, 1) * sizeof(RenderData::UIQuad), sizeof(RenderData::UIQuad), "UIQuads");
		if (!m_QuadBuffer)
		{
			m_Errors.Report(fmt::format("failed to allocate {} UI quads", total));
			return false;
		}
		const bool sameScene = !recreated && m_SceneId == ui.GetId();
		if (!sameScene)
			std::fill(m_TextureDensity.begin(), m_TextureDensity.end(), 0.0f);
		m_SceneId = ui.GetId();

		auto noteDensity = [&](const RenderData::UIQuad* quads, uint32_t count)
		{
			// The most canvas units one texture coordinate unit spans, per texture (for mip streaming).
			for (uint32_t index = 0; index < count; index++)
			{
				const RenderData::UIQuad& quad = quads[index];
				if ((quad.Flags & RenderData::c_UIQuadKindMask) != RenderData::c_UIQuadImage || quad.Resource == 0)
					continue;
				const uint32_t texture = quad.Resource - 1;
				if (texture >= m_TextureDensity.size())
					m_TextureDensity.resize(texture + 1, 0.0f);
				const float du = std::abs(quad.UV.z - quad.UV.x);
				const float dv = std::abs(quad.UV.w - quad.UV.y);
				const float density = std::max(du > 0.0f ? (quad.Rect.z - quad.Rect.x) / du : 0.0f, dv > 0.0f ? (quad.Rect.w - quad.Rect.y) / dv : 0.0f);
				if (std::isfinite(density))
					m_TextureDensity[texture] = std::max(m_TextureDensity[texture], density);
			}
		};

		std::vector<UploadedCanvas> uploaded;
		uploaded.reserve(views.size());
		uint32_t offset = 0;
		for (size_t index = 0; index < views.size(); index++)
		{
			const UICanvasView& view = views[index];
			const std::vector<RenderData::UIQuad>& quads = ui.EnsureQuads(view);
			const uint32_t count = static_cast<uint32_t>(quads.size());
			const UploadedCanvas previous = sameScene && index < m_Uploaded.size() ? m_Uploaded[index] : UploadedCanvas {};
			bool whole = previous.QuadArrayId != ui.GetQuadArrayId(view) || previous.Offset != offset || previous.Count != count;
			if (!whole && ui.GetQuadVersion(view) != previous.QuadVersion)
			{
				m_RangeScratch.clear();
				whole = !ui.GetQuadChanges(view, previous.QuadVersion, m_RangeScratch);
				if (!whole)
				{
					std::sort(m_RangeScratch.begin(), m_RangeScratch.end(), [](const UIQuadRange& a, const UIQuadRange& b) { return a.First < b.First; });
					size_t merged = 0;
					for (const UIQuadRange& range : m_RangeScratch)
					{
						if (merged > 0 && range.First <= m_RangeScratch[merged - 1].First + m_RangeScratch[merged - 1].Count + c_RangeMergeGap)
						{
							UIQuadRange& last = m_RangeScratch[merged - 1];
							last.Count = std::max(last.First + last.Count, range.First + range.Count) - last.First;
						}
						else
						{
							m_RangeScratch[merged++] = range;
						}
					}
					m_RangeScratch.resize(merged);
					for (const UIQuadRange& range : m_RangeScratch)
					{
						const uint32_t first = std::min(range.First, count);
						const uint32_t rangeCount = std::min(range.Count, count - first);
						if (rangeCount == 0)
							continue;
						const uint64_t bytes = static_cast<uint64_t>(rangeCount) * sizeof(RenderData::UIQuad);
						commandList->writeBuffer(m_QuadBuffer, quads.data() + first, bytes, static_cast<uint64_t>(offset + first) * sizeof(RenderData::UIQuad));
						stats.UploadedBytes += bytes;
						noteDensity(quads.data() + first, rangeCount);
					}
				}
			}
			if (whole && count > 0)
			{
				const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(RenderData::UIQuad);
				commandList->writeBuffer(m_QuadBuffer, quads.data(), bytes, static_cast<uint64_t>(offset) * sizeof(RenderData::UIQuad));
				stats.UploadedBytes += bytes;
				noteDensity(quads.data(), count);
			}
			uploaded.push_back(UploadedCanvas { ui.GetQuadArrayId(view), ui.GetQuadVersion(view), offset, count });
			offset += count;
		}
		m_Uploaded = std::move(uploaded);
		return true;
	}

	void GameUIRenderer::UpdateTextures(GameUIScene& ui, const SceneOverlayFrame& frame, AssetManagerBase* assets, float maxScale)
	{
		const std::span<const AssetHandle> textures = ui.GetTextures();
		const std::span<const uint32_t> uses = ui.GetTextureUses();
		bool changed = m_TextureSlots.size() != textures.size();
		m_TextureSlots.resize(textures.size(), BindlessTextureTable::c_WhiteSlot);
		m_TextureDensity.resize(std::max(m_TextureDensity.size(), textures.size()), 0.0f);
		for (size_t index = 0; index < textures.size(); index++)
		{
			if (uses[index] == 0)
				continue;
			// Requested once per frame, at High priority (UI is what players read); white while it loads.
			uint32_t entry = BindlessTextureTable::c_WhiteSlot;
			const Ref<Texture> texture = assets ? assets->GetAsset<Texture>(textures[index], AssetPriority::High) : nullptr;
			if (texture && texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot)
			{
				const TextureSpecification& specification = texture->GetSpecification();
				entry = texture->GetBindlessSlot() & 0xFFFFFFu;
				if (specification.Grayscale)
					entry |= RenderData::c_UITextureGrayscale;
				if (GetTextureFormatInfo(specification.Format).IsSRGB)
					entry |= RenderData::c_UITextureSRGB;
				if (frame.VisibleTextures && m_TextureDensity[index] > 0.0f)
				{
					// What one texture coordinate unit spans on screen decides the level the texture streams to.
					const float uvPixels = m_TextureDensity[index] * maxScale;
					frame.VisibleTextures->push_back(VisibleTextureUse { textures[index], std::max(texture->GetWidth(), texture->GetHeight()), texture->GetMipCount(),
						uvPixels, 1.0f, uvPixels });
				}
			}
			else if (IsPending(assets, textures[index]))
			{
				frame.Stats->PendingAssets++;
			}
			if (m_TextureSlots[index] != entry)
			{
				m_TextureSlots[index] = entry;
				changed = true;
			}
		}
		const bool recreated = EnsureBuffer(m_TextureSlotBuffer, std::max<size_t>(m_TextureSlots.size(), 1) * sizeof(uint32_t), sizeof(uint32_t), "UITextureSlots");
		if (!m_TextureSlotBuffer)
		{
			m_Errors.Report("failed to allocate the UI texture slots");
			return;
		}
		if ((changed || recreated) && !m_TextureSlots.empty())
		{
			frame.CommandList->writeBuffer(m_TextureSlotBuffer, m_TextureSlots.data(), m_TextureSlots.size() * sizeof(uint32_t));
			frame.Stats->UploadedBytes += m_TextureSlots.size() * sizeof(uint32_t);
		}
	}

	void GameUIRenderer::UpdateGlyphs(GameUIScene& ui, const SceneOverlayFrame& frame, AssetManagerBase* assets)
	{
		const std::span<const AssetHandle> fonts = ui.GetFonts();
		m_FontRegions.resize(fonts.size());
		for (FontRegion& region : m_FontRegions)
			region.Texture = nullptr;
		auto markDirty = [this](uint32_t begin, uint32_t end)
		{
			if (m_GlyphDirtyEnd > m_GlyphDirtyBegin)
			{
				m_GlyphDirtyBegin = std::min(m_GlyphDirtyBegin, begin);
				m_GlyphDirtyEnd = std::max(m_GlyphDirtyEnd, end);
			}
			else
			{
				m_GlyphDirtyBegin = begin;
				m_GlyphDirtyEnd = end;
			}
		};

		for (uint32_t fontIndex = 0; fontIndex < fonts.size(); fontIndex++)
		{
			ui.GetFontGlyphs(fontIndex, m_GlyphScratch);
			if (m_GlyphScratch.empty())
				continue;
			// Requested once per frame; the default font for index 0 (texts without a font, or whose font loads).
			Ref<Font> font;
			if (fontIndex == 0 || !fonts[fontIndex].IsValid())
				font = Font::GetDefault();
			else if (assets)
				font = assets->GetAsset<Font>(fonts[fontIndex], AssetPriority::High);
			if (!font)
			{
				if (IsPending(assets, fonts[fontIndex]))
					frame.Stats->PendingAssets++;
				continue; // Its glyphs draw nothing until it is loaded again (its texts are reshaped then)
			}
			FontAtlas* atlas = frame.Atlases->Get(font);
			if (!atlas)
				continue;

			// The font's region of the glyph table, filled again for a new atlas.
			FontRegion& region = m_FontRegions[fontIndex];
			const uint32_t glyphCount = font->GetGlyphCount();
			if (region.AtlasId != atlas->GetId() || region.Size < glyphCount)
			{
				if (region.Size < glyphCount)
				{
					region.Base = static_cast<uint32_t>(m_Glyphs.size());
					region.Size = glyphCount;
					m_Glyphs.resize(m_Glyphs.size() + glyphCount, glm::uvec2(0u));
				}
				std::fill(m_Glyphs.begin() + region.Base, m_Glyphs.begin() + region.Base + region.Size, glm::uvec2(0u));
				markDirty(region.Base, region.Base + region.Size);
				region.AtlasId = atlas->GetId();
			}

			// Each distinct glyph the UI shows is asked for every frame (cost per glyph, not per quad): that rasterizes it
			// within the frame's budget and keeps it from being evicted while it is on screen.
			atlas->SetRasterBudget(frame.Atlases->GetBudget());
			for (const uint32_t glyph : m_GlyphScratch)
			{
				if (glyph >= region.Size)
					continue;
				const GlyphInfo& info = atlas->GetGlyphByIndex(glyph);
				if (info.Pending)
					frame.Stats->PendingGlyphs++;
				const glm::uvec2 entry = PackGlyph(info);
				glm::uvec2& stored = m_Glyphs[region.Base + glyph];
				if (stored != entry)
				{
					stored = entry;
					markDirty(region.Base + glyph, region.Base + glyph + 1);
				}
			}
			frame.Atlases->SetBudget(atlas->GetRasterBudget());
			if (!atlas->Upload(m_Device, frame.CommandList))
			{
				m_Errors.Report("failed to create a glyph atlas texture");
				continue;
			}
			region.Texture = atlas->GetTexture();
		}

		const bool recreated = EnsureBuffer(m_GlyphBuffer, std::max<size_t>(m_Glyphs.size(), 1) * sizeof(glm::uvec2), sizeof(glm::uvec2), "UIGlyphs");
		if (!m_GlyphBuffer)
		{
			m_Errors.Report("failed to allocate the UI glyph table");
			return;
		}
		if (recreated && !m_Glyphs.empty())
		{
			m_GlyphDirtyBegin = 0;
			m_GlyphDirtyEnd = static_cast<uint32_t>(m_Glyphs.size());
		}
		if (m_GlyphDirtyEnd > m_GlyphDirtyBegin)
		{
			const uint64_t bytes = static_cast<uint64_t>(m_GlyphDirtyEnd - m_GlyphDirtyBegin) * sizeof(glm::uvec2);
			frame.CommandList->writeBuffer(m_GlyphBuffer, m_Glyphs.data() + m_GlyphDirtyBegin, bytes, static_cast<uint64_t>(m_GlyphDirtyBegin) * sizeof(glm::uvec2));
			frame.Stats->UploadedBytes += bytes;
			m_GlyphDirtyBegin = 0;
			m_GlyphDirtyEnd = 0;
		}
	}

	void GameUIRenderer::BuildRuns(GameUIScene& ui, std::span<const UICanvasView> views)
	{
		// Binds a font to a run's next atlas slot: true when it is bound (or cannot draw now, so needs no slot: still loading,
		// no atlas; its glyphs draw nothing), false when the run's slots are full.
		auto bind = [this](DrawRun& run, uint32_t& bound, uint32_t font) -> bool
		{
			if (font >= m_FontRegions.size() || !m_FontRegions[font].Texture)
				return true;
			for (uint32_t slot = 0; slot < bound; slot++)
			{
				if (run.Fonts[slot] == font)
					return true;
			}
			if (bound == RenderData::c_MaxBoundAtlases)
				return false;
			run.Fonts[bound] = font;
			run.GlyphBases[bound] = m_FontRegions[font].Base;
			run.Atlases[bound] = m_FontRegions[font].Texture;
			bound++;
			return true;
		};

		for (size_t index = 0; index < views.size(); index++)
		{
			const UICanvasView& view = views[index];
			const UploadedCanvas& uploaded = m_Uploaded[index];
			if (uploaded.Count == 0)
				continue;
			DrawRun empty;
			empty.Scale = view.Scale;
			empty.PixelSnap = view.PixelSnap;
			empty.Atlases.fill(m_EmptyAtlas.Get());

			// Up to four fonts: the whole canvas in one draw.
			DrawRun run = empty;
			uint32_t bound = 0;
			ui.GetCanvasFonts(view, m_FontScratch);
			bool fits = true;
			for (const uint32_t font : m_FontScratch)
				fits = fits && bind(run, bound, font);
			if (fits)
			{
				run.FirstQuad = uploaded.Offset;
				run.QuadCount = uploaded.Count;
				m_Runs.push_back(run);
				continue;
			}

			// More fonts than a draw binds: a new run starts at each glyph whose font the current one cannot bind (rare: the
			// canvas's quads are scanned).
			const std::vector<RenderData::UIQuad>& quads = ui.EnsureQuads(view);
			run = empty;
			bound = 0;
			uint32_t runStart = 0;
			for (uint32_t quadIndex = 0; quadIndex < quads.size(); quadIndex++)
			{
				const RenderData::UIQuad& quad = quads[quadIndex];
				if ((quad.Flags & RenderData::c_UIQuadKindMask) != RenderData::c_UIQuadGlyph)
					continue;
				const uint32_t font = (quad.Flags >> RenderData::c_UIQuadFontShift) & RenderData::c_UIQuadFontMask;
				if (bind(run, bound, font))
					continue;
				run.FirstQuad = uploaded.Offset + runStart;
				run.QuadCount = quadIndex - runStart;
				m_Runs.push_back(run);
				run = empty;
				bound = 0;
				runStart = quadIndex;
				bind(run, bound, font);
			}
			run.FirstQuad = uploaded.Offset + runStart;
			run.QuadCount = uploaded.Count - runStart;
			m_Runs.push_back(run);
		}
	}

	nvrhi::IGraphicsPipeline* GameUIRenderer::GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo)
	{
		for (const auto& [info, pipeline] : m_Pipelines)
		{
			if (info == framebufferInfo)
				return pipeline;
		}
		// Blended in display values over the finished image, like every overlay.
		nvrhi::BlendState::RenderTarget blend;
		blend.setBlendEnable(true)
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.VS = m_VertexShader;
		desc.PS = m_PixelShader;
		desc.bindingLayouts = { m_BindingLayout, Renderer::GetBindlessTextures().GetLayout() };
		desc.renderState.rasterState.setCullNone();
		desc.renderState.blendState.setRenderTarget(0, blend);
		desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		ST_CORE_VERIFY(pipeline, "GameUIRenderer: failed to create the UI pipeline");
		m_Pipelines.emplace_back(framebufferInfo, pipeline);
		return pipeline;
	}

	nvrhi::IBindingSet* GameUIRenderer::GetBindingSet(const std::array<nvrhi::ITexture*, RenderData::c_MaxBoundAtlases>& atlases)
	{
		auto it = m_BindingSets.find(atlases);
		if (it == m_BindingSets.end())
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_QuadBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_TextureSlotBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_GlyphBuffer),
				nvrhi::BindingSetItem::Texture_SRV(3, atlases[0]),
				nvrhi::BindingSetItem::Texture_SRV(4, atlases[1]),
				nvrhi::BindingSetItem::Texture_SRV(5, atlases[2]),
				nvrhi::BindingSetItem::Texture_SRV(6, atlases[3]),
				nvrhi::BindingSetItem::Sampler(0, m_Sampler),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::UIDrawParameters))
			};
			nvrhi::BindingSetHandle set = m_Device->createBindingSet(desc, m_BindingLayout);
			if (!set)
				return nullptr;
			it = m_BindingSets.emplace(atlases, std::make_pair(set, true)).first;
		}
		it->second.second = true;
		return it->second.first;
	}

	void GameUIRenderer::Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
	{
		if (m_Runs.empty())
			return;
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.UI");
		nvrhi::GraphicsState state;
		state.pipeline = GetPipeline(framebuffer->getFramebufferInfo());
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(m_ViewportSize.x, m_ViewportSize.y));
		for (const DrawRun& run : m_Runs)
		{
			nvrhi::IBindingSet* set = GetBindingSet(run.Atlases);
			if (!set)
			{
				m_Errors.Report("failed to create the UI binding set");
				break;
			}
			state.bindings = { set, Renderer::GetBindlessTextures().GetTable() };
			commandList->setGraphicsState(state);
			RenderData::UIDrawParameters parameters;
			parameters.ViewportSize = m_ViewportSize;
			parameters.Scale = run.Scale;
			parameters.PixelSnap = run.PixelSnap ? 1u : 0u;
			parameters.FirstQuad = run.FirstQuad;
			parameters.Fonts = run.Fonts;
			parameters.GlyphBases = run.GlyphBases;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(run.QuadCount).setStartInstanceLocation(run.FirstQuad));
		}
		// Binding sets of atlases no draw used this frame are let go (atlases grow and change).
		for (auto it = m_BindingSets.begin(); it != m_BindingSets.end();)
			it = it->second.second ? std::next(it) : m_BindingSets.erase(it);
	}

}
