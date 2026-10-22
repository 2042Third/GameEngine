#include "stpch.h"
#include "Strata/GameUI/GameUIRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlasCache.h"
#include "Strata/Renderer/GPUProfiler.h"
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

	namespace
	{

		// The UI pass's GPU time (its world-space and screen-space draws), a stage of its own inside the renderer's overlays.
		FrameStageID GetUIGPUStage()
		{
			static const FrameStageID s_Stage = FrameProfiler::Get().RegisterStage("GPU.UI", false, FrameStageKind::GPU);
			return s_Stage;
		}

	}

	void RegisterGameUIRenderer()
	{
		// Registered with the module, after the renderer's stages: its perf log column exists before a log writes its header.
		GetUIGPUStage();
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
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),
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
		// Canvases placed in the scene need its camera: they are no screen content.
		if (!GameUIScene::HasCanvases(scene))
			return false;
		GameUIScene& ui = GameUIScene::Get(scene);
		return ui.GetRootCanvasCount() > ui.GetWorldCanvasCount();
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

	void GameUIRenderer::ReleaseResources()
	{
		if (m_BindingSets.empty() && !m_QuadBuffer && !m_TextureSlotBuffer && !m_GlyphBuffer && !m_ClipBuffer && !m_TransformBuffer)
			return;
		// Command lists in flight hold references to what they use. New buffers are filled whole (EnsureBuffer reports them).
		m_BindingSets.clear();
		m_QuadBuffer = nullptr;
		m_TextureSlotBuffer = nullptr;
		m_GlyphBuffer = nullptr;
		m_ClipBuffer = nullptr;
		m_TransformBuffer = nullptr;
		m_Uploaded.clear();
		m_UploadedTransforms.clear();
		m_SceneId = 0;
	}

	bool GameUIRenderer::Prepare(const SceneOverlayFrame& frame)
	{
		m_Runs.clear();
		m_GPUTimer = frame.GPUTimer;
		m_JitteredDepth = frame.JitteredDepth;
		// Binding sets no draw of the previous frame used are let go, whether or not it drew (atlases grow and change, and a
		// frame that draws no UI uses none): they hold the atlases' textures and the buffers.
		for (auto it = m_BindingSets.begin(); it != m_BindingSets.end();)
		{
			if (!it->second.second)
			{
				it = m_BindingSets.erase(it);
				continue;
			}
			it->second.second = false;
			++it;
		}
		Scene* scene = frame.OverlayScene;
		// A scene without canvases costs this check, and holds nothing of the UI's (a menu's atlases and quads included).
		if (!scene || !GameUIScene::HasCanvases(*scene))
		{
			ReleaseResources();
			return false;
		}
		GameUIScene& ui = GameUIScene::Get(*scene);
		SceneOverlayStats& stats = *frame.Stats;
		// World-space canvases are part of the scene: drawn in every view of it that has its camera.
		const uint32_t worldCanvases = ui.GetWorldCanvasCount();
		if (!frame.DrawScreenSpace)
		{
			// Hidden screen-space canvases (editor views without the game UI): counted, not laid out.
			stats.HiddenCanvases += ui.GetRootCanvasCount() - worldCanvases;
			if (worldCanvases == 0 || !frame.HasCamera)
				return false;
		}
		if (frame.ViewportSize.x == 0 || frame.ViewportSize.y == 0 || !frame.CommandList || !frame.Atlases)
			return false;

		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.UI");
		m_ViewportSize = glm::vec2(frame.ViewportSize);
		m_ScreenZoom = frame.ScreenView.GetZoom();
		m_ScreenOrigin = frame.ScreenView.GetOffset();
		const UIScreen screen { frame.ScreenView.GetScreenSize(frame.ViewportSize), GetContentScale() };
		const std::span<const UICanvasView> views = ui.EnsureLayout(screen, frame.DrawScreenSpace);
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
		if (!UploadQuads(ui, views, commandList, stats) || !UploadClips(ui, views, commandList, stats) || !UploadTransforms(ui, views, frame, stats))
			return false;
		// Textures stream to the most pixels a texture coordinate unit of theirs spans: on the screen the canvases' scale, in
		// the scene what the camera makes of a canvas unit where each canvas is.
		float maxScale = 0.0f;
		for (size_t index = 0; index < views.size(); index++)
		{
			const UICanvasView& view = views[index];
			if (!view.WorldSpace)
				maxScale = std::max(maxScale, view.Scale * m_ScreenZoom);
			else if (m_ViewTransforms[index] != c_NoTransform)
				maxScale = std::max(maxScale, GetProjectedUnitPixels(m_Transforms[m_ViewTransforms[index]], m_ViewportSize, view.UnitSize * 0.5f));
		}
		UpdateTextures(ui, frame, assets, maxScale);
		UpdateGlyphs(ui, frame, assets);
		if (!m_QuadBuffer || !m_TextureSlotBuffer || !m_GlyphBuffer || !m_ClipBuffer || !m_TransformBuffer)
			return false;
		BuildRuns(ui, views);

		for (size_t index = 0; index < views.size(); index++)
		{
			// Canvases in the scene count only where they are drawn (a view with the scene's camera).
			if (views[index].WorldSpace && m_ViewTransforms[index] == c_NoTransform)
				continue;
			stats.Elements += ui.GetElementCount(views[index]);
			stats.Canvases++;
		}
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
			UploadedCanvas& canvas = uploaded.emplace_back();
			canvas.QuadArrayId = ui.GetQuadArrayId(view);
			canvas.QuadVersion = ui.GetQuadVersion(view);
			canvas.Offset = offset;
			canvas.Count = count;
			if (sameScene && index < m_Uploaded.size())
			{
				canvas.ClipLayoutId = m_Uploaded[index].ClipLayoutId;
				canvas.ClipVersion = m_Uploaded[index].ClipVersion;
				canvas.ClipOffset = m_Uploaded[index].ClipOffset;
				canvas.ClipCount = m_Uploaded[index].ClipCount;
			}
			offset += count;
		}
		m_Uploaded = std::move(uploaded);
		return true;
	}

	bool GameUIRenderer::UploadTransforms(GameUIScene& ui, std::span<const UICanvasView> views, const SceneOverlayFrame& frame, SceneOverlayStats& stats)
	{
		// A world-space canvas's transform (canvas units to clip space) follows the camera and the canvas's entity: computed
		// every frame, uploaded when it changed.
		m_ViewTransforms.assign(views.size(), c_NoTransform);
		m_Transforms.clear();
		for (size_t index = 0; index < views.size(); index++)
		{
			if (!views[index].WorldSpace || !frame.HasCamera)
				continue;
			m_ViewTransforms[index] = static_cast<uint32_t>(m_Transforms.size());
			// Relative to the camera (the subtraction in double), as the scene's depth the canvas is tested against was drawn.
			glm::mat4 relative = ui.GetCanvasWorldMatrix(views[index]);
			relative[3] = glm::vec4(glm::vec3(glm::dvec3(glm::vec3(relative[3])) - glm::dvec3(frame.CameraPosition)), relative[3].w);
			m_Transforms.push_back(frame.ViewProjection * relative);
		}
		const bool recreated = EnsureBuffer(m_TransformBuffer, std::max<uint64_t>(m_Transforms.size(), 1) * sizeof(glm::mat4), sizeof(glm::mat4), "UITransforms");
		if (!m_TransformBuffer)
		{
			m_Errors.Report(fmt::format("failed to allocate {} UI canvas transforms", m_Transforms.size()));
			return false;
		}
		if (!m_Transforms.empty() && (recreated || m_Transforms != m_UploadedTransforms))
		{
			const uint64_t bytes = m_Transforms.size() * sizeof(glm::mat4);
			frame.CommandList->writeBuffer(m_TransformBuffer, m_Transforms.data(), bytes);
			stats.UploadedBytes += bytes;
			m_UploadedTransforms = m_Transforms;
		}
		return true;
	}

	float GameUIRenderer::GetProjectedUnitPixels(const glm::mat4& transform, const glm::vec2& viewport, const glm::vec2& point)
	{
		// The pixels between a point of a canvas and its neighbours one unit right and down, once projected (0 behind the
		// camera).
		auto project = [&](const glm::vec2& at, glm::vec2& outPixel)
		{
			const glm::vec4 clip = transform * glm::vec4(at, 0.0f, 1.0f);
			if (!(clip.w > 1e-6f))
				return false;
			outPixel = glm::vec2(clip) / clip.w * 0.5f * viewport;
			return true;
		};
		glm::vec2 center;
		glm::vec2 right;
		glm::vec2 down;
		if (!project(point, center) || !project(point + glm::vec2(1.0f, 0.0f), right) || !project(point + glm::vec2(0.0f, 1.0f), down))
			return 0.0f;
		const float pixels = std::max(glm::length(right - center), glm::length(down - center));
		return std::isfinite(pixels) ? pixels : 0.0f;
	}

	bool GameUIRenderer::UploadClips(GameUIScene& ui, std::span<const UICanvasView> views, nvrhi::ICommandList* commandList, SceneOverlayStats& stats)
	{
		// The clip rects of every canvas view, one after the other (a buffer of one rect when no canvas has a mask).
		uint64_t total = 0;
		for (const UICanvasView& view : views)
			total += ui.GetClipRects(view).size();
		const bool recreated = EnsureBuffer(m_ClipBuffer, std::max<uint64_t>(total, 1) * sizeof(RenderData::UIClipRect), sizeof(RenderData::UIClipRect), "UIClips");
		if (!m_ClipBuffer)
		{
			m_Errors.Report(fmt::format("failed to allocate {} UI clip rects", total));
			return false;
		}
		uint32_t offset = 0;
		for (size_t index = 0; index < views.size(); index++)
		{
			const UICanvasView& view = views[index];
			UploadedCanvas& canvas = m_Uploaded[index];
			const std::span<const RenderData::UIClipRect> clips = ui.GetClipRects(view);
			const uint32_t count = static_cast<uint32_t>(clips.size());
			const bool current = !recreated && canvas.ClipLayoutId == ui.GetLayoutId(view) && canvas.ClipVersion == ui.GetClipVersion(view)
				&& canvas.ClipOffset == offset && canvas.ClipCount == count;
			if (!current && count > 0)
			{
				const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(RenderData::UIClipRect);
				commandList->writeBuffer(m_ClipBuffer, clips.data(), bytes, static_cast<uint64_t>(offset) * sizeof(RenderData::UIClipRect));
				stats.UploadedBytes += bytes;
			}
			canvas.ClipLayoutId = ui.GetLayoutId(view);
			canvas.ClipVersion = ui.GetClipVersion(view);
			canvas.ClipOffset = offset;
			canvas.ClipCount = count;
			offset += count;
		}
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
			// A canvas in the scene draws only with the scene's camera.
			if (uploaded.Count == 0 || (view.WorldSpace && m_ViewTransforms[index] == c_NoTransform))
				continue;
			DrawRun empty;
			empty.Scale = view.Scale * m_ScreenZoom;
			empty.PixelSnap = view.PixelSnap;
			empty.World = view.WorldSpace;
			empty.Transform = view.WorldSpace ? m_ViewTransforms[index] : 0;
			empty.FirstClip = uploaded.ClipOffset;
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

	nvrhi::IGraphicsPipeline* GameUIRenderer::GetPipeline(const nvrhi::FramebufferInfo& framebufferInfo, bool world, bool jitteredDepth)
	{
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>>& pipelines = !world ? m_Pipelines
			: (jitteredDepth ? m_JitteredWorldPipelines : m_WorldPipelines);
		for (const auto& [info, pipeline] : pipelines)
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
		// A canvas in the scene hides behind what is in front of it (reversed-Z), like world text; it never writes depth.
		desc.renderState.depthStencilState.setDepthTestEnable(world).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
		if (world && jitteredDepth)
			SetJitteredDepthBias(desc.renderState.rasterState);
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		ST_CORE_VERIFY(pipeline, "GameUIRenderer: failed to create the UI pipeline");
		pipelines.emplace_back(framebufferInfo, pipeline);
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
				nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_ClipBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_TransformBuffer),
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

	void GameUIRenderer::DrawWorld(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
	{
		DrawRuns(commandList, framebuffer, true);
	}

	void GameUIRenderer::Draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
	{
		DrawRuns(commandList, framebuffer, false);
	}

	void GameUIRenderer::DrawRuns(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, bool world)
	{
		if (std::none_of(m_Runs.begin(), m_Runs.end(), [world](const DrawRun& run) { return run.World == world; }))
			return;
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.UI");
		// The draws' GPU time is GPU.UI's; the renderer's timer goes back to its stage afterwards.
		const FrameStageID previousStage = m_GPUTimer ? m_GPUTimer->GetStage() : c_InvalidFrameStage;
		if (m_GPUTimer)
			m_GPUTimer->Next(GetUIGPUStage());
		nvrhi::GraphicsState state;
		state.pipeline = GetPipeline(framebuffer->getFramebufferInfo(), world, world && m_JitteredDepth);
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(m_ViewportSize.x, m_ViewportSize.y));
		for (const DrawRun& run : m_Runs)
		{
			if (run.World != world)
				continue;
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
			parameters.Origin = m_ScreenOrigin;
			parameters.Scale = run.Scale;
			parameters.Flags = (run.PixelSnap ? RenderData::c_UIDrawPixelSnap : 0u)
				| (run.World ? RenderData::c_UIDrawWorld | (run.Transform << RenderData::c_UIDrawTransformShift) : 0u);
			parameters.FirstQuad = run.FirstQuad;
			parameters.FirstClip = run.FirstClip;
			parameters.Fonts = run.Fonts;
			parameters.GlyphBases = run.GlyphBases;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(6).setInstanceCount(run.QuadCount).setStartInstanceLocation(run.FirstQuad));
		}
		if (m_GPUTimer)
			m_GPUTimer->Next(previousStage);
	}

}
