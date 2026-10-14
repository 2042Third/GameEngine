#include "stpch.h"
#include "Strata/GameUI/GameUIScene.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/GameUI/GameUISceneData.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <span>
#include <string_view>

// Texts of UI elements: shaped from their font's glyph metrics (FontFace, never rasterizing), once per change of the text,
// its font or the options its rect gives it, per layout.
namespace Strata
{

	namespace
	{

		TextOverflowMode ToOverflowMode(UITextOverflow overflow)
		{
			switch (overflow)
			{
			case UITextOverflow::Overflow: return TextOverflowMode::Overflow;
			case UITextOverflow::Truncate: return TextOverflowMode::Truncate;
			case UITextOverflow::Ellipsis: return TextOverflowMode::Ellipsis;
			}
			return TextOverflowMode::Overflow;
		}

		bool SameOptions(const TextShapeOptions& a, const TextShapeOptions& b)
		{
			return a.Alignment == b.Alignment && a.VerticalAlignment == b.VerticalAlignment && a.WrapWidth == b.WrapWidth && a.MaxWidth == b.MaxWidth
				&& a.BoxHeight == b.BoxHeight && a.LineSpacing == b.LineSpacing && a.Overflow == b.Overflow;
		}

	}

	const FontFace* GameUIScene::GetTextFace(const UITextComponent& text, AssetHandle& outShapedFont)
	{
		// The text's font once it is loaded (requested at High priority: text is what players read first), the engine's default
		// font meanwhile or when it has none. A font that loads later is a content change: its texts are shaped again.
		outShapedFont = UUID::Null();
		if (text.Font.IsValid())
		{
			if (!m_Validating)
				m_WatchedAssets.insert(text.Font);
			if (AssetManagerBase* assets = m_Scene->GetAssetManager())
			{
				const Ref<Font> font = m_Validating ? assets->PeekLoadedAsset<Font>(text.Font) : assets->GetAsset<Font>(text.Font, AssetPriority::High);
				// The font asset outlives the call (the manager holds it while it is loaded), and with it its face.
				if (const FontFace* face = font ? font->GetFace() : nullptr)
				{
					outShapedFont = text.Font;
					return face;
				}
			}
		}
		return Font::GetDefault()->GetFace();
	}

	TextShapeOptions GameUIScene::GetMeasureOptions(const UITextComponent& text, float wrapWidth) const
	{
		// The options a fitted rect measures its text with, and the same ones it is drawn with (so it is shaped once): the
		// block placed at the top, wrapped only when the width does not fit the text.
		TextShapeOptions options;
		options.Alignment = text.Alignment;
		options.VerticalAlignment = TextVerticalAlignment::Top;
		options.WrapWidth = wrapWidth;
		options.LineSpacing = std::clamp(text.LineSpacing, 0.5f, 3.0f);
		options.Overflow = ToOverflowMode(text.Overflow);
		return options;
	}

	TextShapeOptions GameUIScene::GetShapeOptions(const UITextComponent& text, const UIRectComponent* rect, const UIBox& box) const
	{
		const float fontSize = std::clamp(text.FontSize, 1.0f, 1024.0f);
		const bool fitsWidth = rect && (rect->FitContent == UIFitContent::Width || rect->FitContent == UIFitContent::Both);
		const bool fitsHeight = rect && (rect->FitContent == UIFitContent::Height || rect->FitContent == UIFitContent::Both);
		const bool cuts = text.Overflow != UITextOverflow::Overflow;
		// A rect that fits the text's width holds it unwrapped; one that fits its height holds every line.
		TextShapeOptions options = GetMeasureOptions(text, text.Wrap && !fitsWidth ? box.Size.x / fontSize : 0.0f);
		options.MaxWidth = cuts && !text.Wrap && !fitsWidth ? box.Size.x / fontSize : 0.0f;
		options.BoxHeight = cuts && !fitsHeight ? box.Size.y / fontSize : 0.0f;
		return options;
	}

	std::string_view GameUIScene::GetShownText(const Canvas& canvas, uint32_t element, const UITextComponent& text) const
	{
		const uint32_t field = canvas.Elements[element].FieldTextOf;
		if (field == c_NoElement)
			return text.Text;
		const UIInputFieldComponent* component = m_Registry->try_get<UIInputFieldComponent>(canvas.Elements[field].Entity);
		return component ? std::string_view(component->Text) : std::string_view();
	}

	TextShapeOptions GameUIScene::GetElementShapeOptions(const Canvas& canvas, uint32_t element, const UITextComponent& text, const UIRectComponent* rect,
		const UIBox& box) const
	{
		if (canvas.Elements[element].FieldTextOf == c_NoElement)
			return GetShapeOptions(text, rect, box);
		// A field's text is one line, never wrapped or cut: it scrolls instead.
		TextShapeOptions options = GetMeasureOptions(text, 0.0f);
		options.Overflow = TextOverflowMode::Overflow;
		return options;
	}

	glm::vec2 GameUIScene::PlaceFieldLine(const UITextComponent& text, const ShapedText& shaped, const UIBox& box, float size, FieldRecord* record,
		std::span<const TextCaretStop> stops) const
	{
		const float place = text.VerticalAlignment == UITextVerticalAlignment::Top ? 0.0f : (text.VerticalAlignment == UITextVerticalAlignment::Middle ? 0.5f : 1.0f);
		const float top = box.Min.y + (box.Size.y - shaped.Height * size) * place;
		const float width = shaped.Width * size;
		const float caretWidth = GetCaretWidth(size);
		if (width + caretWidth <= box.Size.x)
		{
			// It fits: placed by its alignment, like any text.
			if (record)
				record->Scroll = 0.0f;
			const float origin = text.Alignment == TextAlignment::Left ? box.Min.x : (text.Alignment == TextAlignment::Center ? box.GetCenter().x : box.GetMax().x);
			return glm::vec2(origin + shaped.Min.x * size, top);
		}
		// Wider than the element: from its left edge, scrolled so that the caret stays inside.
		float scroll = record ? record->Scroll : 0.0f;
		if (record)
		{
			float caret = 0.0f;
			for (const TextCaretStop& stop : stops)
			{
				if (stop.Byte == record->State.Caret)
					caret = stop.X * size;
			}
			scroll = std::min(scroll, caret);
			scroll = std::max(scroll, caret + caretWidth - box.Size.x);
			scroll = std::clamp(scroll, 0.0f, width + caretWidth - box.Size.x);
			record->Scroll = scroll;
		}
		return glm::vec2(box.Min.x - scroll, top);
	}

	const ShapedText& GameUIScene::ShapeElementText(Layout& layout, uint32_t element, const UITextComponent& text, std::string_view shown,
		const TextShapeOptions& options)
	{
		AssetHandle shapedFont;
		const FontFace* face = GetTextFace(text, shapedFont);
		if (m_Validating)
		{
			// A fresh shaping, so that a text written without a signal shows as a difference.
			ShapeText(*face, shown, options, m_ValidateShaped);
			return m_ValidateShaped;
		}
		ElementText& cached = layout.Texts[element];
		if (cached.Valid && cached.ShapedFont == shapedFont && SameOptions(cached.Options, options))
			return cached.Shaped;
		ShapeText(*face, shown, options, cached.Shaped);
		cached.Valid = true;
		cached.Options = options;
		cached.ShapedFont = shapedFont;
		m_Stats.TextsShaped++;
		return cached.Shaped;
	}

}
