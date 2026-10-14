#include "stpch.h"
#include "Strata/GameUI/GameUIScene.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/GameUI/GameUISceneData.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <cmath>

// The quads of a canvas's layout: each element writes its image (one quad, or nine when sliced) and its text (a quad per
// glyph) into a slot of its own, in element (draw) order. Slots have room to spare for texts, so a text that changes
// rewrites its slot alone; one that outgrows it, or a change of the elements, rebuilds the array.
namespace Strata
{

	namespace
	{

		uint32_t PackColor(const glm::vec4& color)
		{
			return DebugDraw::PackColor(glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f)));
		}

		// Room for a text's glyphs: a quarter more, at least four, so a counter or a label that changes a little keeps its slot.
		uint32_t GetTextCapacity(uint32_t glyphs)
		{
			return glyphs + std::max(4u, (glyphs + 3) / 4);
		}

		RenderData::UIQuad HiddenQuad()
		{
			return RenderData::UIQuad();
		}

	}

	uint32_t GameUIScene::GetTextureIndex(AssetHandle texture)
	{
		auto it = m_TextureIndices.find(texture);
		if (it != m_TextureIndices.end())
			return it->second;
		const uint32_t index = static_cast<uint32_t>(m_Textures.size());
		m_Textures.push_back(texture);
		m_TextureUses.push_back(0);
		m_TextureIndices.emplace(texture, index);
		m_WatchedAssets.insert(texture);
		m_ResourceVersion++;
		return index;
	}

	uint32_t GameUIScene::GetFontIndex(AssetHandle font)
	{
		auto it = m_FontIndices.find(font);
		if (it != m_FontIndices.end())
			return it->second;
		if (m_Fonts.size() >= RenderData::c_MaxUIFonts)
			return RenderData::c_MaxUIFonts; // No room: the caller draws the text with nothing
		const uint32_t index = static_cast<uint32_t>(m_Fonts.size());
		m_Fonts.push_back(font);
		m_FontIndices.emplace(font, index);
		m_FontGlyphUses.emplace_back();
		m_ResourceVersion++;
		return index;
	}

	void GameUIScene::CountQuadUses(Layout& layout, const RenderData::UIQuad& quad, int delta)
	{
		const uint32_t kind = quad.Flags & RenderData::c_UIQuadKindMask;
		if (kind == RenderData::c_UIQuadGlyph)
		{
			const uint32_t font = (quad.Flags >> RenderData::c_UIQuadFontShift) & RenderData::c_UIQuadFontMask;
			if (font >= m_FontGlyphUses.size())
				return;
			std::unordered_map<uint32_t, uint32_t>& glyphs = m_FontGlyphUses[font];
			if (delta > 0)
			{
				if (glyphs[quad.Resource]++ == 0)
					m_ResourceVersion++;
				layout.FontUses[font]++;
			}
			else
			{
				auto glyph = glyphs.find(quad.Resource);
				if (glyph != glyphs.end() && --glyph->second == 0)
				{
					glyphs.erase(glyph);
					m_ResourceVersion++;
				}
				auto fontUse = layout.FontUses.find(font);
				if (fontUse != layout.FontUses.end() && --fontUse->second == 0)
					layout.FontUses.erase(fontUse);
			}
		}
		else if (kind == RenderData::c_UIQuadImage && quad.Resource > 0 && quad.Resource - 1 < m_TextureUses.size())
		{
			uint32_t& uses = m_TextureUses[quad.Resource - 1];
			if (delta > 0)
			{
				if (uses++ == 0)
					m_ResourceVersion++;
			}
			else if (uses > 0 && --uses == 0)
			{
				m_ResourceVersion++;
			}
		}
	}

	void GameUIScene::GetCanvasFonts(const UICanvasView& view, std::vector<uint32_t>& outFonts) const
	{
		outFonts.clear();
		for (const auto& [font, uses] : m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].FontUses)
			outFonts.push_back(font);
		std::sort(outFonts.begin(), outFonts.end());
	}

	void GameUIScene::GetFontGlyphs(uint32_t font, std::vector<uint32_t>& outGlyphs) const
	{
		outGlyphs.clear();
		if (font >= m_FontGlyphUses.size())
			return;
		outGlyphs.reserve(m_FontGlyphUses[font].size());
		for (const auto& [glyph, uses] : m_FontGlyphUses[font])
			outGlyphs.push_back(glyph);
		std::sort(outGlyphs.begin(), outGlyphs.end());
	}

	void GameUIScene::ReleaseLayoutGlyphs(Layout& layout)
	{
		for (const RenderData::UIQuad& quad : layout.Quads)
			CountQuadUses(layout, quad, -1);
		layout.FontUses.clear();
	}

	void GameUIScene::RecordQuadChange(Layout& layout, uint32_t first, uint32_t count)
	{
		layout.QuadVersion++;
		// Adjacent ranges of one update merge.
		if (!layout.QuadChanges.empty() && layout.QuadChanges.back().Version + 1 == layout.QuadVersion
			&& layout.QuadChanges.back().Range.First + layout.QuadChanges.back().Range.Count == first)
		{
			layout.QuadChanges.back().Range.Count += count;
			layout.QuadChanges.back().Version = layout.QuadVersion;
			return;
		}
		layout.QuadChanges.push_back({ layout.QuadVersion, { first, count } });
		while (layout.QuadChanges.size() > c_MaxQuadChanges)
		{
			layout.QuadChangesFloor = layout.QuadChanges.front().Version;
			layout.QuadChanges.pop_front();
		}
	}

	const std::vector<RenderData::UIQuad>& GameUIScene::EnsureQuads(const UICanvasView& view)
	{
		Canvas& canvas = m_Canvases[view.CanvasIndex];
		Layout& layout = canvas.Layouts[view.LayoutIndex];
		BuildQuads(canvas, layout);
		return layout.Quads;
	}

	uint64_t GameUIScene::GetQuadArrayId(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].QuadArrayId;
	}

	uint64_t GameUIScene::GetQuadVersion(const UICanvasView& view) const
	{
		return m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex].QuadVersion;
	}

	bool GameUIScene::GetQuadChanges(const UICanvasView& view, uint64_t sinceVersion, std::vector<UIQuadRange>& outRanges) const
	{
		const Layout& layout = m_Canvases[view.CanvasIndex].Layouts[view.LayoutIndex];
		if (sinceVersion < layout.QuadChangesFloor)
			return false;
		for (const Layout::QuadChange& change : layout.QuadChanges)
		{
			if (change.Version > sinceVersion)
				outRanges.push_back(change.Range);
		}
		return true;
	}

	void GameUIScene::BuildQuads(Canvas& canvas, Layout& layout)
	{
		const uint32_t count = static_cast<uint32_t>(canvas.Elements.size());
		std::vector<RenderData::UIQuad>& scratch = m_QuadScratch;
		if (!layout.QuadsRepack && layout.QuadFirst.size() == count)
		{
			if (layout.QuadsDirty.empty())
				return;
			ST_PROFILE_FUNCTION();
			// Each changed element rewrites its slot; one that outgrew it rebuilds the array.
			for (const uint32_t element : layout.QuadsDirty)
			{
				layout.QuadsDirtyMarks[element] = 0;
				if (layout.QuadsRepack)
					continue;
				scratch.clear();
				const uint32_t written = WriteElementQuads(canvas, layout, element, scratch);
				if (written > layout.QuadCapacity[element])
				{
					layout.QuadsRepack = true;
					continue;
				}
				RenderData::UIQuad* slot = layout.Quads.data() + layout.QuadFirst[element];
				for (uint32_t index = 0; index < layout.QuadCapacity[element]; index++)
				{
					RenderData::UIQuad& quad = slot[index];
					CountQuadUses(layout, quad, -1);
					quad = index < written ? scratch[index] : HiddenQuad();
					CountQuadUses(layout, quad, 1);
				}
				m_Stats.QuadsBuilt += written;
				RecordQuadChange(layout, layout.QuadFirst[element], layout.QuadCapacity[element]);
			}
			layout.QuadsDirty.clear();
			if (!layout.QuadsRepack)
				return;
		}

		ST_PROFILE_SCOPE("GameUIScene::RebuildQuads");
		// The whole array: every element's quads, each in a slot sized for what it writes now (with room for texts).
		ReleaseLayoutGlyphs(layout);
		layout.Quads.clear();
		layout.QuadFirst.assign(count, 0);
		layout.QuadCapacity.assign(count, 0);
		for (uint32_t element = 0; element < count; element++)
		{
			scratch.clear();
			const uint32_t written = WriteElementQuads(canvas, layout, element, scratch);
			const entt::entity entity = canvas.Elements[element].Entity;
			uint32_t capacity = written;
			if (m_Registry->valid(entity) && m_Registry->all_of<UITextComponent>(entity))
			{
				uint32_t glyphs = 0;
				for (const RenderData::UIQuad& quad : scratch)
					glyphs += (quad.Flags & RenderData::c_UIQuadKindMask) == RenderData::c_UIQuadGlyph ? 1 : 0;
				capacity = written - glyphs + GetTextCapacity(glyphs);
			}
			layout.QuadFirst[element] = static_cast<uint32_t>(layout.Quads.size());
			layout.QuadCapacity[element] = capacity;
			for (const RenderData::UIQuad& quad : scratch)
				CountQuadUses(layout, quad, 1);
			layout.Quads.insert(layout.Quads.end(), scratch.begin(), scratch.end());
			layout.Quads.resize(layout.Quads.size() + (capacity - written), HiddenQuad());
			m_Stats.QuadsBuilt += written;
		}
		layout.QuadsRepack = false;
		layout.QuadsDirty.clear();
		layout.QuadsDirtyMarks.assign(count, 0);
		layout.QuadArrayId = m_NextQuadArrayId++;
		layout.QuadVersion++;
		layout.QuadChanges.clear();
		layout.QuadChangesFloor = layout.QuadVersion;
	}

	uint32_t GameUIScene::WriteElementQuads(Canvas& canvas, Layout& layout, uint32_t element, std::vector<RenderData::UIQuad>& out)
	{
		const size_t first = out.size();
		const entt::entity entity = canvas.Elements[element].Entity;
		if (!m_Registry->valid(entity) || !IsVisible(canvas, element))
			return 0;
		const UIBox box = layout.Boxes[element];
		AssetManagerBase* assets = m_Scene->GetAssetManager();

		if (const UIImageComponent* image = m_Registry->try_get<UIImageComponent>(entity))
		{
			// The state of the selectable that tints this image (at rest: its normal color, or disabled).
			glm::vec4 color = image->Color;
			AssetHandle texture = image->Texture;
			const uint32_t tintFrom = canvas.Elements[element].TintFrom;
			if (tintFrom != c_NoElement)
			{
				if (const UISelectableComponent* selectable = m_Registry->try_get<UISelectableComponent>(canvas.Elements[tintFrom].Entity))
				{
					const UIVisualState state = GetVisualState(canvas.Elements[tintFrom].Entity, *selectable);
					if (selectable->Transition == UISelectableTransition::ColorTint)
						color *= GetTintColor(canvas.Elements[tintFrom].Entity, *selectable, state);
					else if (selectable->Transition == UISelectableTransition::ImageSwap)
					{
						const AssetHandle swapped = state == UIVisualState::Hover ? selectable->HoverImage
							: state == UIVisualState::Pressed ? selectable->PressedImage
							: state == UIVisualState::Focused ? selectable->FocusedImage
							: state == UIVisualState::Disabled ? selectable->DisabledImage : UUID::Null();
						if (swapped.IsValid())
							texture = swapped;
					}
				}
			}

			const bool border = image->BorderWidth > 0.0f && image->BorderColor.a > 0.0f;
			if (color.a > 0.0f || border)
			{
				RenderData::UIQuad quad;
				quad.Flags = RenderData::c_UIQuadImage;
				quad.Color = PackColor(color);
				quad.Resource = texture.IsValid() ? GetTextureIndex(texture) + 1 : 0;
				glm::vec4 uv = image->UVRect;
				glm::vec2 min = box.Min;
				glm::vec2 max = box.GetMax();
				// The texture's size, once loaded (it is watched: loading it rebuilds these quads).
				glm::vec2 textureSize(0.0f);
				if (texture.IsValid() && assets && (image->PreserveAspect || image->Type == UIImageType::Sliced))
				{
					if (const Ref<Texture> loaded = assets->PeekLoadedAsset<Texture>(texture))
						textureSize = glm::vec2(static_cast<float>(loaded->GetWidth()), static_cast<float>(loaded->GetHeight()));
				}

				if (image->Type == UIImageType::Sliced && textureSize.x > 0.0f && textureSize.y > 0.0f)
				{
					// Nine pieces: the borders keep their size (SliceScale canvas units per texel), the edges and the center stretch.
					const glm::vec4 borders = glm::max(image->SliceBorders, glm::vec4(0.0f));
					glm::vec4 units = borders * std::max(image->SliceScale, 0.0f);
					// Borders wider than the rect shrink together.
					const glm::vec2 size = box.Size;
					const float horizontal = units.x + units.z;
					const float vertical = units.y + units.w;
					if (horizontal > size.x && horizontal > 0.0f)
					{
						units.x *= size.x / horizontal;
						units.z *= size.x / horizontal;
					}
					if (vertical > size.y && vertical > 0.0f)
					{
						units.y *= size.y / vertical;
						units.w *= size.y / vertical;
					}
					const float xs[4] = { min.x, min.x + units.x, max.x - units.z, max.x };
					const float ys[4] = { min.y, min.y + units.y, max.y - units.w, max.y };
					const glm::vec2 uvSize(uv.z - uv.x, uv.w - uv.y);
					const float us[4] = { uv.x, uv.x + borders.x / textureSize.x * glm::sign(uvSize.x), uv.z - borders.z / textureSize.x * glm::sign(uvSize.x), uv.z };
					const float vs[4] = { uv.y, uv.y + borders.y / textureSize.y * glm::sign(uvSize.y), uv.w - borders.w / textureSize.y * glm::sign(uvSize.y), uv.w };
					for (int row = 0; row < 3; row++)
					{
						for (int column = 0; column < 3; column++)
						{
							if (!(xs[column + 1] > xs[column]) || !(ys[row + 1] > ys[row]))
								continue;
							RenderData::UIQuad piece = quad;
							piece.Rect = glm::vec4(xs[column], ys[row], xs[column + 1], ys[row + 1]);
							piece.UV = glm::vec4(us[column], vs[row], us[column + 1], vs[row + 1]);
							out.push_back(piece);
						}
					}
				}
				else
				{
					if (image->PreserveAspect && textureSize.x > 0.0f && textureSize.y > 0.0f)
					{
						// The shown part's aspect, centered in the rect.
						const float aspect = (textureSize.x * std::abs(uv.z - uv.x)) / std::max(textureSize.y * std::abs(uv.w - uv.y), 1e-6f);
						const glm::vec2 size = box.Size;
						glm::vec2 fitted = size;
						if (size.x > size.y * aspect)
							fitted.x = size.y * aspect;
						else
							fitted.y = size.x / std::max(aspect, 1e-6f);
						min = box.Min + (size - fitted) * 0.5f;
						max = min + fitted;
					}
					if (image->Type == UIImageType::Filled)
					{
						// Cut to the fill amount along the fill direction, the texture with it.
						const float fill = std::clamp(image->FillAmount, 0.0f, 1.0f);
						switch (image->FillDirection)
						{
						case UIFillDirection::LeftToRight:
							max.x = min.x + (max.x - min.x) * fill;
							uv.z = uv.x + (uv.z - uv.x) * fill;
							break;
						case UIFillDirection::RightToLeft:
							min.x = max.x - (max.x - min.x) * fill;
							uv.x = uv.z - (uv.z - uv.x) * fill;
							break;
						case UIFillDirection::TopToBottom:
							max.y = min.y + (max.y - min.y) * fill;
							uv.w = uv.y + (uv.w - uv.y) * fill;
							break;
						case UIFillDirection::BottomToTop:
							min.y = max.y - (max.y - min.y) * fill;
							uv.y = uv.w - (uv.w - uv.y) * fill;
							break;
						}
					}
					if (max.x > min.x && max.y > min.y)
					{
						quad.Rect = glm::vec4(min, max);
						quad.UV = uv;
						quad.CornerRadius = std::max(image->CornerRadius, 0.0f);
						quad.BorderWidth = border ? image->BorderWidth : 0.0f;
						quad.BorderColor = PackColor(image->BorderColor);
						out.push_back(quad);
					}
				}
			}
		}

		if (const UITextComponent* text = m_Registry->try_get<UITextComponent>(entity))
		{
			const bool outline = text->OutlineWidth > 0.0f && text->OutlineColor.a > 0.0f;
			if (!text->Text.empty() && (text->Color.a > 0.0f || outline))
			{
				const UIRectComponent* rect = m_Registry->try_get<UIRectComponent>(entity);
				const ShapedText& shaped = ShapeElementText(layout, element, *text, GetShapeOptions(*text, rect, box));
				AssetHandle shapedFont;
				const FontFace* face = GetTextFace(*text, shapedFont);
				const uint32_t font = GetFontIndex(shapedFont);
				if (font < RenderData::c_MaxUIFonts && shapedFont == layout.Texts[element].ShapedFont)
				{
					const float size = std::clamp(text->FontSize, 1.0f, 1024.0f);
					const float originX = text->Alignment == TextAlignment::Left ? box.Min.x : (text->Alignment == TextAlignment::Center ? box.GetCenter().x : box.GetMax().x);
					const float blockHeight = shaped.Height * size;
					const float place = text->VerticalAlignment == UITextVerticalAlignment::Top ? 0.0f : (text->VerticalAlignment == UITextVerticalAlignment::Middle ? 0.5f : 1.0f);
					const float top = box.Min.y + (box.Size.y - blockHeight) * place;
					RenderData::UIQuad quad;
					quad.Flags = RenderData::c_UIQuadGlyph | (font << RenderData::c_UIQuadFontShift);
					quad.Color = PackColor(text->Color);
					quad.BorderColor = PackColor(text->OutlineColor);
					quad.BorderWidth = outline ? std::clamp(text->OutlineWidth, 0.0f, 0.1f) : 0.0f;
					for (const ShapedGlyph& glyph : shaped.Glyphs)
					{
						const FontGlyphMetrics& metrics = face->GetGlyphMetrics(glyph.GlyphIndex);
						const glm::vec2 pen(originX + glyph.Pen.x * size, top - glyph.Pen.y * size);
						quad.Rect = glm::vec4(pen.x + metrics.PlaneMin.x * size, pen.y - metrics.PlaneMax.y * size, pen.x + metrics.PlaneMax.x * size,
							pen.y - metrics.PlaneMin.y * size);
						quad.UV = glm::vec4(pen, 0.0f, 0.0f);
						quad.Resource = glyph.GlyphIndex;
						out.push_back(quad);
					}
				}
			}
		}
		// Every quad of the element is clipped by the masks above it.
		const uint32_t clip = canvas.Elements[element].Clip;
		for (size_t index = first; index < out.size(); index++)
			out[index].Clip = clip;
		return static_cast<uint32_t>(out.size() - first);
	}

}
