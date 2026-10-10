#pragma once

#include <Strata/Core/Window.h>

#include <array>
#include <cstdint>
#include <span>

namespace Strata::EmbeddedFiles
{

	// StrataEditor/Resources/Brand/StrataMark<size>.rgba (Tools/GenerateBrandAssets.py), embedded by
	// StrataEditor/CMakeLists.txt: the strata mark as RGBA8 rows, top row first.
	std::span<const uint8_t> GetStrataMark16();
	std::span<const uint8_t> GetStrataMark32();
	std::span<const uint8_t> GetStrataMark48();

}

namespace Strata
{

	// The editor window's icon: the strata mark at 16, 32 and 48 pixels.
	inline std::array<WindowIconImage, 3> GetEditorWindowIcon()
	{
		return { {
			{ 16, 16, EmbeddedFiles::GetStrataMark16() },
			{ 32, 32, EmbeddedFiles::GetStrataMark32() },
			{ 48, 48, EmbeddedFiles::GetStrataMark48() }
		} };
	}

}
