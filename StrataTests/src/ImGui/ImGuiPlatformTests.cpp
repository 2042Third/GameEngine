#include <doctest/doctest.h>

#include "Strata/Core/Window.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"
#include "Strata/ImGui/ImGuiPlatform.h"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

using namespace Strata;

namespace
{

	// A window that records what the platform layer asks of it.
	class TestWindow final : public Window
	{
	public:
		void ProcessEvents() override {}
		uint32_t GetWidth() const override { return Width; }
		uint32_t GetHeight() const override { return Height; }
		void SetCursorMode(CursorMode mode) override { Mode = mode; }
		glm::uvec2 GetFramebufferSize() const override { return Framebuffer; }
		float GetContentScale() const override { return 1.0f; }
		void SetEventCallback(const EventCallbackFn&) override {}
		void SetVSync(bool) override {}
		bool IsVSync() const override { return true; }
		void SetTitle(const std::string&) override {}
		void SetFullscreen(bool) override {}
		bool IsFullscreen() const override { return false; }
		bool IsMinimized() const override { return false; }
		bool IsFocused() const override { return true; }
		void Maximize() override {}
		CursorMode GetCursorMode() const override { return Mode; }
		void SetCursorShape(CursorShape shape) override
		{
			Shape = shape;
			ShapeChanges++;
		}
		std::string GetClipboardText() const override { return Clipboard; }
		void SetClipboardText(const std::string& text) override { Clipboard = text; }
		KeyCode GetLayoutKey(KeyCode key) const override
		{
			const auto it = Layout.find(key);
			return it != Layout.end() ? it->second : key;
		}
		void* GetPlatformHandle() const override { return Handle; }
		void SetIcon(std::span<const WindowIconImage>) override {}
		void* GetNativeWindow() const override { return nullptr; }

		uint32_t Width = 1280;
		uint32_t Height = 720;
		glm::uvec2 Framebuffer = glm::uvec2(1280, 720);
		CursorMode Mode = CursorMode::Normal;
		CursorShape Shape = CursorShape::Arrow;
		int ShapeChanges = 0;
		std::string Clipboard;
		// Keys the layout names otherwise (KeyCode by position -> KeyCode by what it types).
		std::map<KeyCode, KeyCode> Layout;
		int HandleTarget = 0;
		void* Handle = &HandleTarget;
	};

	// An ImGui context with the platform layer as its backend, and no renderer (fonts are requested, never drawn).
	struct PlatformContext
	{
		TestWindow Window;
		ImGuiPlatform Platform;
		ImGuiContext* Context = nullptr;

		PlatformContext()
		{
			Context = ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			Platform.Init(Window);
		}

		~PlatformContext()
		{
			Platform.Shutdown();
			ImGui::DestroyContext(Context);
		}

		PlatformContext(const PlatformContext&) = delete;
		PlatformContext& operator=(const PlatformContext&) = delete;

		void Send(Event& event) { Platform.OnEvent(event); }
		void Press(KeyCode key, bool repeat = false)
		{
			KeyPressedEvent event(key, repeat);
			Send(event);
		}
		void Release(KeyCode key)
		{
			KeyReleasedEvent event(key);
			Send(event);
		}

		// One frame as the application runs it: the platform's part, then ImGui's (which applies the queued input), with
		// `draw` between NewFrame and EndFrame (checks of what ImGui saw this frame go there).
		void Frame(const std::function<void()>& draw = {})
		{
			Platform.NewFrame();
			ImGui::NewFrame();
			if (draw)
				draw();
			ImGui::EndFrame();
		}
	};

}

TEST_SUITE("ImGui.Platform")
{
	TEST_CASE("Keys reach ImGui as the keyboard layout names them, with their modifiers")
	{
		PlatformContext imgui;
		// A German keyboard: Y and Z have each other's places.
		imgui.Window.Layout = { { Key::Y, Key::Z }, { Key::Z, Key::Y } };
		imgui.Frame();

		imgui.Press(Key::LeftControl);
		imgui.Press(Key::Y);
		imgui.Frame([]()
		{
			CHECK(ImGui::GetIO().KeyCtrl);
			CHECK(ImGui::IsKeyDown(ImGuiKey_Z));
			CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Y));
			CHECK(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z));
			CHECK(ImGui::IsKeyDown(ImGuiKey_LeftCtrl));
		});

		// A press the system repeats while the key is held is no new press: ImGui repeats keys itself.
		imgui.Press(Key::Y, true);
		imgui.Frame([]()
		{
			CHECK(ImGui::IsKeyDown(ImGuiKey_Z));
			CHECK_FALSE(ImGui::IsKeyPressed(ImGuiKey_Z, false));
		});

		// The layout changes while the key is down: its release is the key its press was.
		imgui.Window.Layout.clear();
		imgui.Release(Key::Y);
		imgui.Release(Key::LeftControl);
		imgui.Frame([]()
		{
			CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Z));
			CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Y));
			CHECK_FALSE(ImGui::GetIO().KeyCtrl);
		});

		// A modifier is down while either of its keys is.
		imgui.Press(Key::LeftShift);
		imgui.Press(Key::RightShift);
		imgui.Release(Key::LeftShift);
		imgui.Frame([]() { CHECK(ImGui::GetIO().KeyShift); });
		imgui.Release(Key::RightShift);
		imgui.Press(Key::RightAlt);
		imgui.Frame([]()
		{
			CHECK_FALSE(ImGui::GetIO().KeyShift);
			CHECK(ImGui::GetIO().KeyAlt);
		});
		imgui.Release(Key::RightAlt);

		// Keys without an ImGui key are left out; the others all have one.
		imgui.Press(Key::World1);
		imgui.Release(Key::World1);
		imgui.Frame();
		CHECK(ImGuiPlatform::ToImGuiKey(Key::World1) == ImGuiKey_None);
		for (const auto [key, expected] : { std::pair(Key::A, ImGuiKey_A), std::pair(Key::D7, ImGuiKey_7), std::pair(Key::F12, ImGuiKey_F12),
			std::pair(Key::KP3, ImGuiKey_Keypad3), std::pair(Key::Enter, ImGuiKey_Enter), std::pair(Key::Left, ImGuiKey_LeftArrow),
			std::pair(Key::GraveAccent, ImGuiKey_GraveAccent), std::pair(Key::World2, ImGuiKey_Oem102), std::pair(Key::RightSuper, ImGuiKey_RightSuper) })
		{
			const int code = static_cast<int>(key);
			CAPTURE(code);
			CHECK(ImGuiPlatform::ToImGuiKey(static_cast<KeyCode>(key)) == expected);
		}
	}

	TEST_CASE("With macOS behaviors, Cmd is the shortcut modifier and Ctrl takes Cmd's place")
	{
		PlatformContext imgui;
		ImGui::GetIO().ConfigMacOSXBehaviors = true;
		imgui.Frame();

		// The keys as they are: ImGui itself swaps the modifiers for macOS.
		imgui.Press(Key::LeftSuper);
		imgui.Press(Key::Z);
		imgui.Frame([]()
		{
			CHECK(ImGui::GetIO().KeyCtrl);
			CHECK_FALSE(ImGui::GetIO().KeySuper);
			CHECK(ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z));
		});
		imgui.Release(Key::Z);
		imgui.Release(Key::LeftSuper);
		imgui.Press(Key::RightControl);
		imgui.Frame([]()
		{
			CHECK(ImGui::GetIO().KeySuper);
			CHECK_FALSE(ImGui::GetIO().KeyCtrl);
		});
	}

	TEST_CASE("Typed text reaches ImGui, also what an input method composed")
	{
		PlatformContext imgui;
		char text[64] = {};
		const auto field = [&]()
		{
			ImGui::Begin("Text");
			if (!ImGui::IsAnyItemActive())
				ImGui::SetKeyboardFocusHere();
			ImGui::InputText("##Field", text, sizeof(text));
			ImGui::End();
		};
		imgui.Frame(field);
		imgui.Frame(field);

		// Characters, as the window reports them (Unicode code points): ASCII, accented, and Chinese composed by an input
		// method.
		for (const uint32_t codepoint : { 0x53u, 0xE9u, 0x4E2Du })
		{
			KeyTypedEvent event(codepoint);
			imgui.Send(event);
		}
		imgui.Frame(field);
		CHECK(std::string(text) == "S\xC3\xA9\xE4\xB8\xAD");

		// Input methods place their candidate lists by the window (ImGui's own support, on Windows).
		CHECK(ImGui::GetMainViewport()->PlatformHandleRaw == imgui.Window.Handle);
#if defined(ST_PLATFORM_WINDOWS)
		CHECK(ImGui::GetPlatformIO().Platform_SetImeDataFn != nullptr);
#endif
	}

	TEST_CASE("Mouse moves, buttons and leaving the window reach ImGui")
	{
		PlatformContext imgui;
		imgui.Frame();
		MouseMovedEvent move(120.0f, 48.5f);
		imgui.Send(move);
		MouseButtonPressedEvent press(Mouse::ButtonRight);
		imgui.Send(press);
		imgui.Frame([]()
		{
			// In whole pixels: ImGui floors the position.
			CHECK(ImGui::GetIO().MousePos.x == 120.0f);
			CHECK(ImGui::GetIO().MousePos.y == 48.0f);
			CHECK(ImGui::IsMouseClicked(ImGuiMouseButton_Right));
			CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Right));
		});
		MouseButtonReleasedEvent release(Mouse::ButtonRight);
		imgui.Send(release);
		// Buttons beyond ImGui's five are left out.
		MouseButtonPressedEvent extra(Mouse::Button7);
		imgui.Send(extra);
		imgui.Frame([]()
		{
			CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Right));
			for (int button = 0; button < ImGuiMouseButton_COUNT; button++)
				CHECK_FALSE(ImGui::IsMouseDown(button));
		});

		// Off the window nothing is under the cursor, until it moves over it again.
		MouseLeaveEvent leave;
		imgui.Send(leave);
		imgui.Frame([]() { CHECK_FALSE(ImGui::IsMousePosValid()); });
		MouseMovedEvent back(5.0f, 6.0f);
		imgui.Send(back);
		imgui.Frame([]()
		{
			CHECK(ImGui::IsMousePosValid());
			CHECK(ImGui::GetIO().MousePos.x == 5.0f);
		});
	}

	TEST_CASE("The wheel scrolls both ways")
	{
		PlatformContext imgui;
		imgui.Frame();
		MouseScrolledEvent scroll(0.5f, -2.0f);
		imgui.Send(scroll);
		imgui.Frame([]()
		{
			CHECK(ImGui::GetIO().MouseWheelH == 0.5f);
			CHECK(ImGui::GetIO().MouseWheel == -2.0f);
		});
		imgui.Frame([]() { CHECK(ImGui::GetIO().MouseWheel == 0.0f); });
	}

	TEST_CASE("The cursor takes the shape ImGui asks for, set only when it changes, while the cursor is ImGui's")
	{
		PlatformContext imgui;
		const auto ask = [](ImGuiMouseCursor cursor) { return [cursor]() { ImGui::SetMouseCursor(cursor); }; };
		imgui.Frame();
		const int initial = imgui.Window.ShapeChanges;
		CHECK(imgui.Window.Shape == CursorShape::Arrow);

		// A frame's request is applied when the next one begins.
		imgui.Frame(ask(ImGuiMouseCursor_ResizeEW));
		imgui.Frame(ask(ImGuiMouseCursor_ResizeEW));
		CHECK(imgui.Window.Shape == CursorShape::ResizeEW);
		CHECK(imgui.Window.ShapeChanges == initial + 1);
		// The same shape again asks the window for nothing.
		imgui.Frame(ask(ImGuiMouseCursor_TextInput));
		CHECK(imgui.Window.ShapeChanges == initial + 1);
		imgui.Frame(ask(ImGuiMouseCursor_Hand));
		CHECK(imgui.Window.Shape == CursorShape::TextInput);
		imgui.Frame(ask(ImGuiMouseCursor_NotAllowed));
		CHECK(imgui.Window.Shape == CursorShape::Hand);

		// A game that locked the cursor keeps it; ImGui's shape returns with the cursor.
		imgui.Window.Mode = CursorMode::Locked;
		const int beforeLock = imgui.Window.ShapeChanges;
		imgui.Frame(ask(ImGuiMouseCursor_ResizeNS));
		imgui.Frame(ask(ImGuiMouseCursor_ResizeNS));
		CHECK(imgui.Window.ShapeChanges == beforeLock);
		imgui.Window.Mode = CursorMode::Normal;
		imgui.Frame(ask(ImGuiMouseCursor_ResizeNS));
		CHECK(imgui.Window.Shape == CursorShape::ResizeNS);
		CHECK(imgui.Window.ShapeChanges == beforeLock + 1);

		// No cursor (or ImGui drawing its own) hides the system's; cursors windows lack are the arrow.
		imgui.Frame(ask(ImGuiMouseCursor_None));
		imgui.Frame(ask(ImGuiMouseCursor_Wait));
		CHECK(imgui.Window.Shape == CursorShape::Hidden);
		imgui.Frame(ask(ImGuiMouseCursor_ResizeNWSE));
		CHECK(imgui.Window.Shape == CursorShape::Arrow);
		ImGui::GetIO().MouseDrawCursor = true;
		imgui.Frame();
		CHECK(imgui.Window.Shape == CursorShape::Hidden);
		ImGui::GetIO().MouseDrawCursor = false;

		// Applications that keep the cursor to themselves (ImGuiConfigFlags_NoMouseCursorChange) get no shapes.
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		const int beforeNoChange = imgui.Window.ShapeChanges;
		imgui.Frame(ask(ImGuiMouseCursor_ResizeAll));
		imgui.Frame();
		CHECK(imgui.Window.ShapeChanges == beforeNoChange);
	}

	TEST_CASE("ImGui's clipboard is the window's")
	{
		PlatformContext imgui;
		ImGui::SetClipboardText("Strata \xC3\xA9" "diteur");
		CHECK(imgui.Window.Clipboard == "Strata \xC3\xA9" "diteur");
		imgui.Window.Clipboard = "copied elsewhere";
		CHECK(std::string(ImGui::GetClipboardText()) == "copied elsewhere");
		imgui.Window.Clipboard.clear();
		CHECK(std::string(ImGui::GetClipboardText()).empty());
	}

	TEST_CASE("Losing the focus releases what was held")
	{
		PlatformContext imgui;
		imgui.Frame();
		imgui.Press(Key::LeftControl);
		imgui.Press(Key::A);
		MouseButtonPressedEvent press(Mouse::ButtonLeft);
		imgui.Send(press);
		imgui.Frame([]()
		{
			CHECK(ImGui::IsKeyDown(ImGuiKey_A));
			CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
		});

		WindowLostFocusEvent lost;
		imgui.Send(lost);
		imgui.Frame([]()
		{
			CHECK(ImGui::GetIO().AppFocusLost);
			CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
			CHECK_FALSE(ImGui::GetIO().KeyCtrl);
			CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
		});
		// The releases the window reports for the keys it had down change nothing more; Ctrl pressed after the focus returned
		// is pressed anew, alone.
		imgui.Release(Key::A);
		imgui.Release(Key::LeftControl);
		WindowFocusEvent focus;
		imgui.Send(focus);
		imgui.Press(Key::RightControl);
		imgui.Frame([]()
		{
			CHECK_FALSE(ImGui::GetIO().AppFocusLost);
			CHECK(ImGui::GetIO().KeyCtrl);
			CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
		});
	}

	TEST_CASE("The display follows the window's size and pixel density, and frames take time")
	{
		PlatformContext imgui;
		imgui.Window.Width = 800;
		imgui.Window.Height = 600;
		imgui.Window.Framebuffer = glm::uvec2(1600, 1200);
		imgui.Frame([]()
		{
			const ImGuiIO& io = ImGui::GetIO();
			CHECK(io.DisplaySize.x == 800.0f);
			CHECK(io.DisplaySize.y == 600.0f);
			CHECK(io.DisplayFramebufferScale.x == 2.0f);
			CHECK(io.DisplayFramebufferScale.y == 2.0f);
			CHECK(io.DeltaTime > 0.0f);
		});
		imgui.Window.Width = 1024;
		imgui.Window.Height = 512;
		imgui.Window.Framebuffer = glm::uvec2(1024, 512);
		imgui.Frame([]()
		{
			const ImGuiIO& io = ImGui::GetIO();
			CHECK(io.DisplaySize.x == 1024.0f);
			CHECK(io.DisplayFramebufferScale.x == 1.0f);
			CHECK(io.DeltaTime > 0.0f);
		});
	}

	TEST_CASE("Shutting the platform layer down leaves ImGui without a platform backend")
	{
		PlatformContext imgui;
		CHECK(ImGui::GetIO().BackendPlatformUserData == &imgui.Platform);
		CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasMouseCursors) != 0);
		imgui.Platform.Shutdown();
		CHECK_FALSE(imgui.Platform.IsInitialized());
		CHECK(ImGui::GetIO().BackendPlatformUserData == nullptr);
		CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasMouseCursors) == 0);
		CHECK(ImGui::GetPlatformIO().Platform_GetClipboardTextFn == nullptr);
		CHECK(ImGui::GetMainViewport()->PlatformHandleRaw == nullptr);
		// Events after it change nothing.
		imgui.Press(Key::A);
		imgui.Platform.Init(imgui.Window);
		imgui.Frame([]() { CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A)); });
	}
}
