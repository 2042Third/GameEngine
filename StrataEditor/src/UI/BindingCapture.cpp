#include "UI/BindingCapture.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		struct CaptureState
		{
			std::string Owner;        // Empty when the UI runs no capture
			double StartTime = 0.0;   // ImGui::GetTime when it began
			int AskedFrame = 0;       // ImGui::GetFrameCount when the owner last asked after it
			bool KeyboardHeld = false; // Until every key is up after the capture
		};

		CaptureState s_Capture;

		// What owns the keys the capture takes from ImGui's widgets.
		ImGuiID GetOwnerId()
		{
			return ImHashStr("Strata.BindingCapture");
		}

		// Locks every keyboard and gamepad key for this frame and, for those pressed in the next one, until they are released:
		// ImGui updates the locks after it read the next frame's key events and before its navigation reads them.
		void HoldKeyboard()
		{
			for (int key = ImGuiKey_Keyboard_BEGIN; key < ImGuiKey_Gamepad_END; key++)
				ImGui::SetKeyOwner(static_cast<ImGuiKey>(key), GetOwnerId(), ImGuiInputFlags_LockUntilRelease);
		}

		bool IsAnyKeyDown()
		{
			for (int key = ImGuiKey_Keyboard_BEGIN; key < ImGuiKey_Gamepad_END; key++)
			{
				if (ImGui::GetKeyData(static_cast<ImGuiKey>(key))->Down)
					return true;
			}
			return false;
		}

	}

	bool BindingCapture::Begin(std::string_view owner, const InputCaptureDevices& devices)
	{
		if (!InputActions::BeginCapture(devices))
			return false;
		s_Capture.Owner = owner;
		s_Capture.StartTime = ImGui::GetTime();
		s_Capture.AskedFrame = ImGui::GetFrameCount();
		s_Capture.KeyboardHeld = true;
		HoldKeyboard();
		return true;
	}

	bool BindingCapture::IsListening(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner || !InputActions::IsCapturing())
			return false;
		s_Capture.AskedFrame = ImGui::GetFrameCount();
		return true;
	}

	std::optional<std::string> BindingCapture::Take(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner || InputActions::IsCapturing())
			return std::nullopt;
		s_Capture.Owner.clear();
		std::optional<std::string> binding = InputActions::TakeCapturedBinding();
		// A mouse press it captured clicks nothing on its way: no item sees it until the button is released.
		if (binding && binding->starts_with("Mouse/"))
		{
			for (int key = ImGuiKey_Mouse_BEGIN; key < ImGuiKey_Mouse_END; key++)
				ImGui::SetKeyOwner(static_cast<ImGuiKey>(key), GetOwnerId(), ImGuiInputFlags_LockUntilRelease);
		}
		return binding;
	}

	void BindingCapture::Cancel(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner)
			return;
		InputActions::CancelCapture();
		s_Capture.Owner.clear();
	}

	double BindingCapture::GetRemainingSeconds()
	{
		if (s_Capture.Owner.empty())
			return 0.0;
		return std::max(0.0, c_TimeoutSeconds - (ImGui::GetTime() - s_Capture.StartTime));
	}

	void BindingCapture::BeginFrame()
	{
		if (!s_Capture.Owner.empty())
		{
			// The owner asks every frame it draws: one that skipped a frame is gone (another page, a closed panel).
			const bool abandoned = ImGui::GetFrameCount() - s_Capture.AskedFrame > 1;
			const bool timedOut = ImGui::GetTime() - s_Capture.StartTime >= c_TimeoutSeconds;
			if (abandoned || (timedOut && InputActions::IsCapturing()))
			{
				// Also a binding taken but never collected: nobody will.
				InputActions::CancelCapture();
				s_Capture.Owner.clear();
			}
		}
		if (!s_Capture.KeyboardHeld)
			return;
		if (s_Capture.Owner.empty() && !IsAnyKeyDown())
		{
			s_Capture.KeyboardHeld = false;
			return;
		}
		HoldKeyboard();
	}

}
