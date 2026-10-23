#include "stpch.h"
#include "Platform/GLFW/GLFWApplicationIcon.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"

#if defined(ST_PLATFORM_MACOS)
	// Only the Objective-C runtime's C interface, no AppKit, CoreGraphics or CoreFoundation header: CompatLint compiles the
	// macOS branches with GCC, which reads these but neither Objective-C++ nor the blocks syntax of the framework headers.
	#include <objc/message.h>
	#include <objc/runtime.h>
#endif

#include <cstring>

namespace Strata
{

#if defined(ST_PLATFORM_MACOS)
	namespace
	{

		// NSSize's layout: two CGFloats, which are doubles in 64-bit programs.
		struct ObjCSize
		{
			double Width = 0.0;
			double Height = 0.0;
		};

		// NSBitmapFormatAlphaNonpremultiplied: the pixels' alpha is straight, as images decode.
		constexpr unsigned long c_BitmapFormatAlphaNonpremultiplied = 1ul << 1;

		// objc_msgSend called as the method it reaches: its type must be that method's exact signature, since the arguments
		// travel by the C calling convention of that signature (a variadic or approximate type passes them wrongly on arm64).
		// Through void (*)(), the type every compiler accepts a function pointer cast from without a warning.
		template<typename Signature>
		Signature* MessageSend()
		{
			return reinterpret_cast<Signature*>(reinterpret_cast<void (*)()>(&objc_msgSend));
		}

		SEL Selector(const char* name)
		{
			return sel_registerName(name);
		}

		id FindClass(const char* name)
		{
			return reinterpret_cast<id>(objc_getClass(name));
		}

		id Allocate(id objectClass)
		{
			return MessageSend<id(id, SEL)>()(objectClass, Selector("alloc"));
		}

		void Release(id object)
		{
			if (object)
				MessageSend<void(id, SEL)>()(object, Selector("release"));
		}

		// The image as an NSImage of one NSBitmapImageRep holding a copy of the pixels (retained: the caller releases it), or
		// nullptr.
		id CreateImage(uint32_t width, uint32_t height, std::span<const uint8_t> pixels, id stringClass, id bitmapClass, id imageClass)
		{
			const id colorSpace = MessageSend<id(id, SEL, const char*)>()(Allocate(stringClass), Selector("initWithUTF8String:"), "NSDeviceRGBColorSpace");
			if (!colorSpace)
				return nullptr;

			// No planes: the representation allocates its pixels, which are filled below.
			const long pixelsWide = static_cast<long>(width);
			const long pixelsHigh = static_cast<long>(height);
			const id bitmap = MessageSend<id(id, SEL, unsigned char**, long, long, long, long, BOOL, BOOL, id, unsigned long, long, long)>()(
				Allocate(bitmapClass),
				Selector("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:"),
				nullptr, pixelsWide, pixelsHigh, 8, 4, YES, NO, colorSpace, c_BitmapFormatAlphaNonpremultiplied, pixelsWide * 4, 32);
			Release(colorSpace);
			if (!bitmap)
				return nullptr;

			unsigned char* data = MessageSend<unsigned char*(id, SEL)>()(bitmap, Selector("bitmapData"));
			const long bytesPerRow = MessageSend<long(id, SEL)>()(bitmap, Selector("bytesPerRow"));
			const size_t rowBytes = static_cast<size_t>(width) * 4;
			id image = nullptr;
			if (data && bytesPerRow >= static_cast<long>(rowBytes))
			{
				for (uint32_t row = 0; row < height; row++)
					std::memcpy(data + static_cast<size_t>(row) * static_cast<size_t>(bytesPerRow), pixels.data() + row * rowBytes, rowBytes);
				image = MessageSend<id(id, SEL, ObjCSize)>()(Allocate(imageClass), Selector("initWithSize:"),
					ObjCSize { static_cast<double>(width), static_cast<double>(height) });
				if (image)
					MessageSend<void(id, SEL, id)>()(image, Selector("addRepresentation:"), bitmap);
			}
			Release(bitmap);
			return image;
		}

	}
#endif

	bool SetApplicationIcon(uint32_t width, uint32_t height, std::span<const uint8_t> pixels)
	{
		if (width == 0 || height == 0 || width > c_MaxApplicationIconSize || height > c_MaxApplicationIconSize)
			return false;
		if (pixels.size() != static_cast<size_t>(width) * height * 4)
			return false;

#if defined(ST_PLATFORM_MACOS)
		const id applicationClass = FindClass("NSApplication");
		const id imageClass = FindClass("NSImage");
		const id bitmapClass = FindClass("NSBitmapImageRep");
		const id stringClass = FindClass("NSString");
		const id poolClass = FindClass("NSAutoreleasePool");
		if (!applicationClass || !imageClass || !bitmapClass || !stringClass || !poolClass)
		{
			ST_CORE_WARN("The application icon cannot be set: AppKit is not loaded");
			return false;
		}

		// What AppKit autoreleases while it takes the image is freed here: the caller need not be inside an event loop's pool.
		const id pool = MessageSend<id(id, SEL)>()(Allocate(poolClass), Selector("init"));
		const id image = CreateImage(width, height, pixels, stringClass, bitmapClass, imageClass);
		const id application = image ? MessageSend<id(id, SEL)>()(applicationClass, Selector("sharedApplication")) : nullptr;
		if (application)
			MessageSend<void(id, SEL, id)>()(application, Selector("setApplicationIconImage:"), image);
		Release(image);
		if (pool)
			MessageSend<void(id, SEL)>()(pool, Selector("drain"));
		if (!application)
			ST_CORE_WARN("The application icon ({} x {}) could not be set", width, height);
		return application != nullptr;
#else
		return false;
#endif
	}

}
