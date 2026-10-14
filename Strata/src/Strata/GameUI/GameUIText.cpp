#include "stpch.h"
#include "Strata/GameUI/GameUIScene.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/GameUI/GameUISceneData.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>

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

	const ShapedText& GameUIScene::ShapeElementText(Layout& layout, uint32_t element, const UITextComponent& text, const TextShapeOptions& options)
	{
		AssetHandle shapedFont;
		const FontFace* face = GetTextFace(text, shapedFont);
		if (m_Validating)
		{
			// A fresh shaping, so that a text written without a signal shows as a difference.
			ShapeText(*face, text.Text, options, m_ValidateShaped);
			return m_ValidateShaped;
		}
		ElementText& cached = layout.Texts[element];
		if (cached.Valid && cached.ShapedFont == shapedFont && SameOptions(cached.Options, options))
			return cached.Shaped;
		ShapeText(*face, text.Text, options, cached.Shaped);
		cached.Valid = true;
		cached.Options = options;
		cached.ShapedFont = shapedFont;
		m_Stats.TextsShaped++;
		return cached.Shaped;
	}

}
