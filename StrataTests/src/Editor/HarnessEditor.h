#pragma once

#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/Log.h>
#include <Strata/Core/Timestep.h>

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Strata::Tests
{

	// An EditorLayer drawn by an ImGuiHarness through a fake host: no file watchers, no native dialogs, and no automation
	// unless asked for. The log starts empty, as in a new editor process (the Console reads the log from its start).
	struct HarnessEditor
	{
		Ref<FakeEditorHost::State> Host = CreateRef<FakeEditorHost::State>();
		ImGuiHarness Harness;
		Scope<EditorLayer> Layer;

		// iniSettings: imgui.ini contents loaded before the first frame (the editor's saved layout and panel states).
		explicit HarnessEditor(const ImGuiHarness::Specification& specification = {}, EditorOptions options = {}, const std::string& iniSettings = {},
			bool automation = false)
			: Harness(specification)
		{
			options.EnableAutomation = automation;
			options.WatchFiles = false;
			Host->UIScale = specification.ContentScale;
			Log::GetBuffer().Clear();
			Layer = CreateScope<EditorLayer>(options, CreateScope<FakeEditorHost>(Host));
			Layer->OnAttach();
			if (!iniSettings.empty())
				ImGui::LoadIniSettingsFromMemory(iniSettings.c_str(), iniSettings.size());
		}

		~HarnessEditor()
		{
			Layer->OnDetach();
		}

		HarnessEditor(const HarnessEditor&) = delete;
		HarnessEditor& operator=(const HarnessEditor&) = delete;

		// Frames of the application: the host's clock and frame count advance with them.
		void Frames(int count, float seconds = ImGuiHarness::c_DeltaTime)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Host->Time += seconds;
				Harness.Frame(*Layer, seconds);
				Host->FrameCount++;
			}
		}

		// A frame that runs `beforeUI` between the layer's update and its UI, as if something happened in the frame then.
		void FrameWith(const std::function<void()>& beforeUI, float seconds = ImGuiHarness::c_DeltaTime)
		{
			Host->Time += seconds;
			Layer->OnUpdate(Timestep(seconds));
			beforeUI();
			Harness.Frame([this]() { Layer->OnImGuiRender(); }, seconds);
			Host->FrameCount++;
		}

		bool Click(std::string_view probeKey)
		{
			Host->Time += 3.0 * ImGuiHarness::c_DeltaTime;
			const bool clicked = Harness.ClickItem(probeKey, *Layer);
			Host->FrameCount += 3;
			return clicked;
		}

		// Moves the mouse to a point, presses and releases the left button over three frames.
		void ClickAt(const ImVec2& position)
		{
			Harness.MoveMouse(position);
			Frames(1);
			Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			Frames(1);
			Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			Frames(1);
		}

		// Clicks a text field of the widget kit, selects its text and types another in its place.
		bool ReplaceText(std::string_view probeKey, std::string_view text)
		{
			if (!Click(probeKey))
				return false;
			Harness.SetKey(ImGuiMod_Ctrl, true);
			Harness.SetKey(ImGuiKey_A, true);
			Frames(1);
			Harness.SetKey(ImGuiKey_A, false);
			Harness.SetKey(ImGuiMod_Ctrl, false);
			Frames(1);
			Harness.TypeText(text);
			Frames(1);
			return true;
		}

		// Runs frames (with a little sleep, for work on other threads) until the condition holds or ten seconds passed.
		bool FramesUntil(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Frames(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return true;
		}

		EditorContext& Context() { return Layer->GetContext(); }

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Layer->GetCommands().Execute(Context(), name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		ImGuiWindow* FindPanelWindow(const char* id)
		{
			return ImGui::FindWindowByName(Layer->GetPanels().GetWindowName(id).c_str());
		}

		// The color a status pill was drawn in last.
		ImU32 GetPillColor(std::string_view probeKey)
		{
			const std::optional<UI::ItemProbe::Item> pill = UI::ItemProbe::Find(probeKey);
			REQUIRE_MESSAGE(pill.has_value(), "No ", std::string(probeKey), " in the last frame");
			return pill->Color;
		}

		// The text a status pill showed last.
		std::string GetPillText(std::string_view probeKey)
		{
			const std::optional<UI::ItemProbe::Item> pill = UI::ItemProbe::Find(probeKey);
			REQUIRE_MESSAGE(pill.has_value(), "No ", std::string(probeKey), " in the last frame");
			return pill->Text;
		}
	};

	inline EditorOptions WithFeatureProject(const std::string& directoryName)
	{
		EditorOptions options;
		options.ProjectPath = CopyFeatureProject(CreateTemporaryDirectory(directoryName) / "Project");
		return options;
	}

}
