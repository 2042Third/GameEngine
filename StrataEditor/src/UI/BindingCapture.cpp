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
			uint64_t Id = 0;          // InputActions::GetCaptureId of the capture it began (a game's later one replaces it)
			double StartTime = 0.0;   // ImGui::GetTime when it began
			int AskedFrame = 0;       // ImGui::GetFrameCount when the owner last asked after it
			bool KeyboardHeld = false; // Until every key is up after the capture
			bool Ended = false;       // InputActions' capture is over: Result is what it took, for the owner's Take
			std::optional<std::string> Result;
		};

		CaptureState s_Capture;

		// Forgets the capture (the keyboard stays held until every key is up).
		void ClearCapture()
		{
			s_Capture.Owner.clear();
			s_Capture.Ended = false;
			s_Capture.Result.reset();
		}

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

		// Takes what InputActions' capture took once it is over, and locks a mouse button it took until it is released, so that
		// its press clicks nothing. BeginFrame does this first in the frame of the press (the capture reads the devices before
		// the editor's UI): the owner asks only when it is drawn, after windows that act on a press (the viewport picks with
		// it, the content browser deselects).
		// Whether InputActions' capture is still the one the UI began: a game's rebinding menu may have replaced it.
		bool IsOwnCapture()
		{
			return InputActions::GetCaptureId() == s_Capture.Id;
		}

		void Collect()
		{
			if (s_Capture.Owner.empty() || s_Capture.Ended || (IsOwnCapture() && InputActions::IsCapturing()))
				return;
			s_Capture.Ended = true;
			// A replaced capture ended without a binding: what the other one takes is not the UI's.
			if (!IsOwnCapture())
				return;
			s_Capture.Result = InputActions::TakeCapturedBinding();
			if (s_Capture.Result && s_Capture.Result->starts_with("Mouse/"))
			{
				for (int key = ImGuiKey_Mouse_BEGIN; key < ImGuiKey_Mouse_END; key++)
					ImGui::SetKeyOwner(static_cast<ImGuiKey>(key), GetOwnerId(), ImGuiInputFlags_LockUntilRelease);
			}
		}

	}

	bool BindingCapture::Begin(std::string_view owner, const InputCaptureDevices& devices)
	{
		if (!InputActions::BeginCapture(devices))
			return false;
		ClearCapture();
		s_Capture.Owner = owner;
		s_Capture.Id = InputActions::GetCaptureId();
		s_Capture.StartTime = ImGui::GetTime();
		s_Capture.AskedFrame = ImGui::GetFrameCount();
		s_Capture.KeyboardHeld = true;
		HoldKeyboard();
		return true;
	}

	bool BindingCapture::IsListening(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner || s_Capture.Ended || !IsOwnCapture() || !InputActions::IsCapturing())
			return false;
		s_Capture.AskedFrame = ImGui::GetFrameCount();
		return true;
	}

	std::optional<std::string> BindingCapture::Take(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner)
			return std::nullopt;
		// Also here: a capture can end after BeginFrame (play started or stopped by a command).
		Collect();
		if (!s_Capture.Ended)
			return std::nullopt;
		std::optional<std::string> binding = std::move(s_Capture.Result);
		ClearCapture();
		return binding;
	}

	void BindingCapture::Cancel(std::string_view owner)
	{
		if (s_Capture.Owner.empty() || s_Capture.Owner != owner)
			return;
		if (!s_Capture.Ended && IsOwnCapture()) // Once collected or replaced, InputActions' capture is not this owner's
			InputActions::CancelCapture();
		ClearCapture();
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
			if (abandoned || (timedOut && !s_Capture.Ended && IsOwnCapture() && InputActions::IsCapturing()))
			{
				// Also a binding taken but never collected: nobody will.
				if (!s_Capture.Ended && IsOwnCapture())
					InputActions::CancelCapture();
				ClearCapture();
			}
			else
			{
				Collect();
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
