#pragma once

#include <Strata/Core/Assert.h>
#include <Strata/Core/Window.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Strata::EmbeddedFiles
{

	// StrataEditor/Resources/Brand/StrataMark<size>.rgba (Tools/GenerateBrandAssets.py), embedded by
	// StrataEditor/CMakeLists.txt: the strata mark as RGBA8 rows, top row first.
	std::span<const uint8_t> GetStrataMark16();
	std::span<const uint8_t> GetStrataMark20();
	std::span<const uint8_t> GetStrataMark24();
	std::span<const uint8_t> GetStrataMark28();
	std::span<const uint8_t> GetStrataMark32();
	std::span<const uint8_t> GetStrataMark40();
	std::span<const uint8_t> GetStrataMark48();
	std::span<const uint8_t> GetStrataMark56();
	std::span<const uint8_t> GetStrataMark64();

}

namespace Strata
{

	// The sizes of the window icon's images: the small (title bar) and large (taskbar, Alt+Tab) icons of Windows at 100,
	// 125, 150, 175 and 200% display scaling, 16 and 32 pixels times the scale. The system picks the image whose area is
	// closest to the size it needs, so with an exact one it never stretches a smaller image (Tools/GenerateBrandAssets.py
	// writes the same sizes; StrataEditor/CMakeLists.txt embeds them).
	constexpr std::array<uint32_t, 9> c_EditorIconSizes = { 16, 20, 24, 28, 32, 40, 48, 56, 64 };

	// The editor window's icon: the strata mark at every size of c_EditorIconSizes.
	inline std::array<WindowIconImage, c_EditorIconSizes.size()> GetEditorWindowIcon()
	{
		const std::array<WindowIconImage, c_EditorIconSizes.size()> images = { {
			{ 16, 16, EmbeddedFiles::GetStrataMark16() },
			{ 20, 20, EmbeddedFiles::GetStrataMark20() },
			{ 24, 24, EmbeddedFiles::GetStrataMark24() },
			{ 28, 28, EmbeddedFiles::GetStrataMark28() },
			{ 32, 32, EmbeddedFiles::GetStrataMark32() },
			{ 40, 40, EmbeddedFiles::GetStrataMark40() },
			{ 48, 48, EmbeddedFiles::GetStrataMark48() },
			{ 56, 56, EmbeddedFiles::GetStrataMark56() },
			{ 64, 64, EmbeddedFiles::GetStrataMark64() }
		} };
		for (size_t index = 0; index < images.size(); index++)
		{
			const size_t size = c_EditorIconSizes[index];
			ST_CORE_ASSERT(images[index].Width == size && images[index].Pixels.size() == size * size * 4, "The window icon's image {} does not match c_EditorIconSizes",
				index);
		}
		return images;
	}

}
