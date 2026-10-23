#include <doctest/doctest.h>

#include "Strata/Core/PlatformDetection.h"

#if defined(ST_PLATFORM_MACOS)

	#include "Strata/Core/Log.h"
	#include "Strata/Core/Window.h"

	#include <objc/message.h>
	#include <objc/runtime.h>

	#include <array>
	#include <cstdint>
	#include <cstdlib>
	#include <cstring>
	#include <string>
	#include <vector>

using namespace Strata;

namespace
{

	// objc_msgSend called with the exact signature of the method it reaches (see GLFWApplicationIcon.cpp).
	template<typename Signature>
	Signature* MessageSend()
	{
		return reinterpret_cast<Signature*>(reinterpret_cast<void (*)()>(&objc_msgSend));
	}

	id Send(id receiver, const char* selector)
	{
		return MessageSend<id(id, SEL)>()(receiver, sel_registerName(selector));
	}

	id FindClass(const char* name)
	{
		return reinterpret_cast<id>(objc_getClass(name));
	}

	// NSRect's and NSSize's layouts: CGFloats, which are doubles in 64-bit programs.
	struct ObjCRect
	{
		double X = 0.0;
		double Y = 0.0;
		double Width = 0.0;
		double Height = 0.0;
	};

	constexpr uint32_t c_IconSize = 48;
	constexpr unsigned long c_CompositingOperationCopy = 1;

	// The Dock icon drawn into an image of c_IconSize pixels (RGBA8, premultiplied alpha, rows top to bottom). AppKit keeps a
	// snapshot of the image set, at the display's scale, rather than the image itself: drawing it is how it reads back.
	std::vector<uint8_t> DrawDockIcon()
	{
		const id poolClass = FindClass("NSAutoreleasePool");
		const id applicationClass = FindClass("NSApplication");
		const id bitmapClass = FindClass("NSBitmapImageRep");
		const id contextClass = FindClass("NSGraphicsContext");
		const id stringClass = FindClass("NSString");
		REQUIRE(poolClass);
		REQUIRE(applicationClass);
		REQUIRE(bitmapClass);
		REQUIRE(contextClass);
		REQUIRE(stringClass);

		std::vector<uint8_t> pixels;
		const id pool = Send(Send(poolClass, "alloc"), "init");
		const id colorSpace = MessageSend<id(id, SEL, const char*)>()(Send(stringClass, "alloc"), sel_registerName("initWithUTF8String:"), "NSDeviceRGBColorSpace");
		// Drawing contexts take premultiplied bitmaps only (bitmap format 0).
		const long size = static_cast<long>(c_IconSize);
		const id bitmap = MessageSend<id(id, SEL, unsigned char**, long, long, long, long, BOOL, BOOL, id, unsigned long, long, long)>()(
			Send(bitmapClass, "alloc"),
			sel_registerName("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:"),
			nullptr, size, size, 8, 4, YES, NO, colorSpace, 0ul, size * 4, 32);
		const id icon = Send(Send(applicationClass, "sharedApplication"), "applicationIconImage");
		const id context = bitmap ? MessageSend<id(id, SEL, id)>()(contextClass, sel_registerName("graphicsContextWithBitmapImageRep:"), bitmap) : nullptr;
		if (icon && context)
		{
			Send(contextClass, "saveGraphicsState");
			MessageSend<void(id, SEL, id)>()(contextClass, sel_registerName("setCurrentContext:"), context);
			// An empty source rectangle draws the whole image.
			MessageSend<void(id, SEL, ObjCRect, ObjCRect, unsigned long, double)>()(icon, sel_registerName("drawInRect:fromRect:operation:fraction:"),
				ObjCRect { 0.0, 0.0, static_cast<double>(size), static_cast<double>(size) }, ObjCRect {}, c_CompositingOperationCopy, 1.0);
			Send(contextClass, "restoreGraphicsState");
			const auto* data = MessageSend<const unsigned char*(id, SEL)>()(bitmap, sel_registerName("bitmapData"));
			const long bytesPerRow = MessageSend<long(id, SEL)>()(bitmap, sel_registerName("bytesPerRow"));
			if (data && bytesPerRow >= size * 4)
			{
				pixels.resize(static_cast<size_t>(c_IconSize) * c_IconSize * 4);
				for (uint32_t row = 0; row < c_IconSize; row++)
					std::memcpy(pixels.data() + static_cast<size_t>(row) * c_IconSize * 4, data + static_cast<size_t>(row) * static_cast<size_t>(bytesPerRow), static_cast<size_t>(c_IconSize) * 4);
			}
		}
		if (bitmap)
			Send(bitmap, "release");
		if (colorSpace)
			Send(colorSpace, "release");
		Send(pool, "drain");
		return pixels;
	}

	using Color = std::array<uint8_t, 4>;

	// An image of four quadrants (top left, top right, bottom left, bottom right) of straight-alpha colors.
	std::vector<uint8_t> MakeQuadrants(uint32_t size, const std::array<Color, 4>& colors)
	{
		std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const Color& color = colors[(y < size / 2 ? 0 : 2) + (x < size / 2 ? 0 : 1)];
				std::memcpy(pixels.data() + (static_cast<size_t>(y) * size + x) * 4, color.data(), 4);
			}
		}
		return pixels;
	}

	// The pixel of a premultiplied image of c_IconSize pixels at the center of a quadrant.
	Color GetQuadrantCenter(const std::vector<uint8_t>& pixels, size_t quadrant)
	{
		const size_t x = (quadrant % 2 == 0 ? 1 : 3) * c_IconSize / 4;
		const size_t y = (quadrant < 2 ? 1 : 3) * c_IconSize / 4;
		Color color {};
		std::memcpy(color.data(), pixels.data() + (y * c_IconSize + x) * 4, 4);
		return color;
	}

	Color Premultiply(const Color& color)
	{
		const auto scale = [&](uint8_t channel) { return static_cast<uint8_t>((channel * color[3] + 127) / 255); };
		return { scale(color[0]), scale(color[1]), scale(color[2]), color[3] };
	}

	bool IsClose(const Color& a, const Color& b)
	{
		for (size_t channel = 0; channel < 4; channel++)
		{
			if (std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel])) > 2)
				return false;
		}
		return true;
	}

}

// A test of its own, StrataTests.DockIcon (label gpu, on the Mac only): it needs the window server, which runs of the gpu
// label have, but no GPU.
TEST_SUITE("Platform.DockIcon")
{
	TEST_CASE("The Dock icon is the image set")
	{
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		WindowSpecification specification;
		specification.Title = "Strata Dock icon test";
		specification.Width = 320;
		specification.Height = 200;
		specification.Visible = false;
		const Scope<Window> window = Window::Create(specification);
		REQUIRE(window);

		// Opaque red, green and blue, and half-transparent orange (straight alpha); the small image's quadrants in another order.
		const std::array<Color, 4> colors = { Color { 255, 0, 0, 255 }, Color { 0, 255, 0, 255 }, Color { 0, 0, 255, 255 }, Color { 255, 128, 0, 128 } };
		const std::vector<uint8_t> largeImage = MakeQuadrants(c_IconSize, colors);
		const std::vector<uint8_t> smallImage = MakeQuadrants(16, { colors[3], colors[2], colors[1], colors[0] });
		const std::vector<uint8_t> before = DrawDockIcon();
		REQUIRE(before.size() == largeImage.size());
		CHECK_FALSE(IsClose(GetQuadrantCenter(before, 0), colors[0]));

		// The largest image becomes the Dock's, whatever the order, upright and with its alpha.
		const WindowIconImage images[] = { { 16, 16, smallImage }, { c_IconSize, c_IconSize, largeImage } };
		window->SetIcon(images);
		const std::vector<uint8_t> icon = DrawDockIcon();
		REQUIRE(icon.size() == largeImage.size());
		for (size_t quadrant = 0; quadrant < colors.size(); quadrant++)
		{
			CAPTURE(quadrant);
			const Color color = GetQuadrantCenter(icon, quadrant);
			const std::string drawn = std::to_string(color[0]) + " " + std::to_string(color[1]) + " " + std::to_string(color[2]) + " " + std::to_string(color[3]);
			CAPTURE(drawn);
			CHECK(IsClose(color, Premultiply(colors[quadrant])));
		}

		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logStart))
			CHECK_MESSAGE(entry.Level < LogLevel::Warn, entry.Message);
	}
}

#endif
