#pragma once

#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace Strata::Tests
{

	// An EditorLayer drawn by an ImGuiHarness through a fake host: no automation, no file watchers, no native dialogs.
	struct HarnessEditor
	{
		Ref<FakeEditorHost::State> Host = CreateRef<FakeEditorHost::State>();
		ImGuiHarness Harness;
		Scope<EditorLayer> Layer;

		// iniSettings: imgui.ini contents loaded before the first frame (the editor's saved layout and panel states).
		explicit HarnessEditor(const ImGuiHarness::Specification& specification = {}, EditorOptions options = {}, const std::string& iniSettings = {})
			: Harness(specification)
		{
			options.EnableAutomation = false;
			options.WatchFiles = false;
			Host->UIScale = specification.ContentScale;
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

		bool Click(std::string_view probeKey)
		{
			Host->Time += 3.0 * ImGuiHarness::c_DeltaTime;
			const bool clicked = Harness.ClickItem(probeKey, *Layer);
			Host->FrameCount += 3;
			return clicked;
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
	};

	inline EditorOptions WithFeatureProject(const std::string& directoryName)
	{
		EditorOptions options;
		options.ProjectPath = CopyFeatureProject(CreateTemporaryDirectory(directoryName) / "Project");
		return options;
	}

}
