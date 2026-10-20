#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/HarnessEditor.h"
#include "EditorLayer.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/PerformancePanel.h"
#include "Panels/StreamingPanel.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "TestHelpers.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Renderer/GPUProfiler.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	ImU32 ToColor(const ImVec4& color)
	{
		return UI::ToColorU32(color);
	}

	// Frames of the harness editor that the frame profiler records, as the application's are.
	void ProfiledFrames(HarnessEditor& editor, int count)
	{
		FrameProfiler& profiler = FrameProfiler::Get();
		for (int frame = 0; frame < count; frame++)
		{
			profiler.BeginFrame();
			editor.Frames(1);
			profiler.EndFrame(0.002);
		}
	}

	// Whether the probe's rectangle lies within the window's visible inner area (not scrolled or cut away).
	bool IsVisibleIn(const UI::ItemProbe::Item& item, const ImGuiWindow& window)
	{
		const ImRect& inner = window.InnerRect;
		return item.Min.y >= inner.Min.y && item.Max.y <= inner.Max.y && item.Min.x >= inner.Min.x && item.Max.x <= inner.Max.x;
	}

	// The table a panel drew last with this many columns (in its window or a child window of it).
	ImGuiTable* FindTable(ImGuiWindow* window, int columns)
	{
		ImGuiContext& context = *GImGui;
		for (int index = 0; index < context.Tables.GetMapSize(); index++)
		{
			ImGuiTable* table = context.Tables.TryGetMapData(index);
			if (table && table->ColumnsCount == columns && table->OuterWindow
				&& (table->OuterWindow == window || ImGui::IsWindowChildOf(table->OuterWindow, window, false, false)))
			{
				return table;
			}
		}
		return nullptr;
	}

	// Makes an asset manager the active one for a test, and none afterwards (also when the test fails).
	class ScopedActiveAssetManager
	{
	public:
		explicit ScopedActiveAssetManager(const Ref<AssetManagerBase>& manager)
		{
			AssetManager::SetActive(manager);
		}

		~ScopedActiveAssetManager()
		{
			AssetManager::SetActive(nullptr);
		}

		ScopedActiveAssetManager(const ScopedActiveAssetManager&) = delete;
		ScopedActiveAssetManager& operator=(const ScopedActiveAssetManager&) = delete;
	};

	// A panel drawn alone in a window of the harness, as the perf tests draw it.
	void DrawPanel(ImGuiHarness& harness, EditorPanel& panel, EditorPanelContext& context, const ImVec2& size, int frames = 1)
	{
		for (int frame = 0; frame < frames; frame++)
		{
			harness.Frame([&]()
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(size);
				ImGui::Begin("Panel", nullptr, ImGuiWindowFlags_NoSavedSettings);
				panel.OnUpdate(context);
				panel.OnImGuiRender(context);
				ImGui::End();
			});
		}
	}

}

TEST_SUITE("Editor.UI")
{
	TEST_CASE("Budget pressure is comfortable, high or over")
	{
		CHECK(GetBudgetPressure(10, AssetResidencyBudgets::c_Unlimited) == BudgetPressure::Unbudgeted);
		CHECK(GetBudgetPressure(0, 100) == BudgetPressure::Comfortable);
		CHECK(GetBudgetPressure(84, 100) == BudgetPressure::Comfortable);
		CHECK(GetBudgetPressure(85, 100) == BudgetPressure::High);
		CHECK(GetBudgetPressure(100, 100) == BudgetPressure::High);
		CHECK(GetBudgetPressure(101, 100) == BudgetPressure::Over);
		CHECK(GetBudgetPressure(1, 0) == BudgetPressure::Over);

		AssetManagerStats stats;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::Unbudgeted);
		CHECK_FALSE(GetMostPressuredPool(stats).has_value());
		stats.Budgets.GpuTextures = 100;
		stats.Budgets.GpuBuffers = 100;
		// Nothing in use: GPU textures stand for the pools.
		CHECK(GetMostPressuredPool(stats) == AssetMemoryPool::GpuTextures);
		stats.Resident.GpuTextures = 50;
		stats.Resident.GpuBuffers = 95;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::High);
		CHECK(GetMostPressuredPool(stats) == AssetMemoryPool::GpuBuffers);
		stats.Resident.GpuTextures = 150;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::Over);
		CHECK(GetMostPressuredPool(stats) == AssetMemoryPool::GpuTextures);
		// Among pools of one pressure, the fullest.
		stats.Resident.GpuTextures = 90;
		stats.Budgets.Cpu = 100;
		stats.Resident.Cpu = 99;
		CHECK(GetMostPressuredPool(stats) == AssetMemoryPool::Cpu);
		// All comfortable: GPU textures stand for them, however full the others are (the figure does not hop between pools).
		stats.Resident = AssetMemoryUsage { 10, 20, 30 };
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::Comfortable);
		CHECK(GetMostPressuredPool(stats) == AssetMemoryPool::GpuTextures);

		// The theme's meanings: the accent while there is room, a warning close to the budget, an error beyond it.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Comfortable)) == ToColor(colors.Accent));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::High)) == ToColor(colors.Warning));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Over)) == ToColor(colors.Error));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Unbudgeted)) == ToColor(colors.TextSecondary));
		// Asset states: loading in Sulfur, as the viewport's streaming view tints it, failed in Cinnabar.
		CHECK(ToColor(GetAssetStateColor(AssetState::Loading)) == ToColor(colors.Warning));
		CHECK(ToColor(GetAssetStateColor(AssetState::Failed)) == ToColor(colors.Error));
		CHECK(ToColor(GetAssetStateColor(AssetState::Ready)) == ToColor(colors.Text));
	}

	TEST_CASE("The assets pill opens the Streaming panel, which shows asset.stats of its frame and reveals assets in the Content Browser")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIStreaming"));
		REQUIRE(editor.Context().HasProject());
		editor.Frames(2);
		// Everything the project's scene uses arrives first, so that nothing changes between the panel's statistics and the
		// command's.
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		REQUIRE(editor.FramesUntil([&]() { return !assets->HasPendingLoads(); }));

		const std::string pill = editor.GetPillText("Status.Assets");
		CHECK(pill.find("ready") != std::string::npos);
		CHECK(pill.find("streaming") != std::string::npos);
		CHECK(pill.find("MB") != std::string::npos);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming)); // An instrument, closed until asked for

		REQUIRE(editor.Click("Status.Assets"));
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Streaming);
		REQUIRE(window);
		CHECK(window->Active);
		// It joined the bottom area, beside the Console (no saved layout knew it).
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(window->DockId == console->DockId);

		// The totals are asset.stats' of the same frame: the panel reads them after the frame's commands ran.
		StreamingPanel* panel = editor.Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
		REQUIRE(panel);
		nlohmann::json command;
		editor.FrameWith([&]() { command = editor.Run("asset.stats"); });
		REQUIRE(panel->GetStats());
		// asset.stats adds the renderer's geometry pool (null here: no renderer) and the imports' memory estimates of the
		// project's manager (EditorAssetManager), which the panel's statistics, those of any asset manager, do not hold.
		REQUIRE(command.contains("importMemoryMB"));
		nlohmann::json managerStats = command;
		managerStats.erase("importMemoryMB");
		nlohmann::json expected = CommandUtils::DescribeAssetStats(*panel->GetStats());
		expected["geometryPool"] = CommandUtils::DescribeGeometryPool();
		CHECK(expected == managerStats);
		CHECK(command["registered"].get<uint32_t>() > 10);

		// Nothing was uploaded (no renderer): the sparkline says so instead of showing an empty box.
		const std::optional<UI::ItemProbe::Item> uploads = UI::ItemProbe::Find("Streaming.Uploads");
		REQUIRE(uploads);
		CHECK(uploads->Text == "No uploads in the last 120 frames");

		// The table lists the assets that are resident, loading, failed or pinned. Without a renderer nothing keeps the
		// project's assets in use, so the scene is pinned; filtered to the project's scenes, the table's first row is the
		// feature scene, and a click shows it in the Content Browser.
		CHECK(panel->GetTableRowCount() > 0);
		const AssetPin pin = assets->Pin(assets->FindAssetByPath("Scenes/Feature.stscene"));
		REQUIRE(pin.IsValid());
		REQUIRE(editor.ReplaceText("Streaming.Filter", "Scenes/Feature"));
		// The table is read again a few times per second.
		editor.Frames(static_cast<int>(StreamingPanel::c_TableRefreshSeconds / ImGuiHarness::c_DeltaTime) + 2);
		const std::optional<UI::ItemProbe::Item> first = UI::ItemProbe::Find("Streaming.FirstAsset");
		REQUIRE(first);
		CHECK(first->Enabled);
		REQUIRE(editor.Click("Streaming.FirstAsset"));
		editor.Frames(2);
		ContentBrowserPanel* contentBrowser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(contentBrowser);
		CHECK(contentBrowser->GetRevealedAsset() == assets->FindAssetByPath("Scenes/Feature.stscene"));
		CHECK(contentBrowser->GetCurrentDirectory() == "Scenes");

		// Assets without a file are not revealed.
		CHECK_FALSE(contentBrowser->RevealAsset(editor.Context(), BuiltinAssets::CubeMesh));
		CHECK(contentBrowser->GetRevealedAsset() == assets->FindAssetByPath("Scenes/Feature.stscene"));
	}

	TEST_CASE("The Streaming table tells the sub-assets of a model apart")
	{
		AssetMetadata file;
		file.Handle = UUID(0x1000);
		file.Path = "Models/Platform.gltf";
		file.Name = "Platform";
		CHECK(StreamingPanel::GetAssetLabel(file) == "Models/Platform.gltf");
		AssetMetadata material = file;
		material.Handle = UUID(0x1001);
		material.Parent = file.Handle;
		material.SubAssetKey = "Material/1";
		material.Name = "Material/1";
		CHECK(StreamingPanel::GetAssetLabel(material) == "Models/Platform.gltf \xE2\x80\xBA Material/1");
		material.Name = "Steel";
		CHECK(StreamingPanel::GetAssetLabel(material) == "Models/Platform.gltf \xE2\x80\xBA Material/1 (Steel)");
		AssetMetadata memory;
		memory.Name = "Generated";
		CHECK(StreamingPanel::GetAssetLabel(memory) == "Generated");

		// The feature project's model: its mesh and materials, each pinned so the table lists it, under labels of their own.
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIStreamingSubAssets"));
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle model = assets->FindAssetByPath("Models/Platform.gltf");
		REQUIRE(model.IsValid());
		std::vector<AssetPin> pins;
		for (const AssetMetadata& metadata : assets->GetAllMetadata())
		{
			if (metadata.Parent == model)
				pins.push_back(assets->Pin(metadata.Handle));
		}
		REQUIRE(pins.size() >= 3);
		editor.Run("editor.showPanel", { { "panel", "Streaming" } });
		editor.Frames(3);
		REQUIRE(editor.ReplaceText("Streaming.Filter", "Platform.gltf \xE2\x80\xBA"));
		StreamingPanel* panel = editor.Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
		REQUIRE(panel);
		REQUIRE(editor.FramesUntil([&]() { return panel->GetShownLabels().size() == pins.size(); }));
		const std::vector<std::string> labels = panel->GetShownLabels();
		CHECK(std::set<std::string>(labels.begin(), labels.end()).size() == labels.size());
		for (const std::string& label : labels)
		{
			CAPTURE(label);
			CHECK(label.rfind("Models/Platform.gltf \xE2\x80\xBA ", 0) == 0);
		}
		// Every character of the labels is in the font.
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Streaming)) == 0);
	}

	TEST_CASE("The Streaming table puts failed assets first and is built on the job threads")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedFakeLoader loader;
		ImGuiHarness harness;
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };

		// Two textures of different sizes and an asset whose data no loader takes (beside the built-in assets, which use no
		// GPU memory without a device).
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle large = manager->Add(0x74001, MakeUsage(0, 2 << 20));
		const AssetHandle smallAsset = manager->Add(0x74002, MakeUsage(0, 1 << 20));
		const AssetHandle broken = manager->AddStored(0x74003, c_FakeAssetType, { 1, 2, 3 }, "Fake/Broken");
		for (const AssetHandle handle : { large, smallAsset, broken })
			manager->RequestLoad(handle);
		REQUIRE(manager->WaitForPendingLoads());
		manager->Update();
		REQUIRE(manager->GetAssetState(broken) == AssetState::Failed);
		REQUIRE(manager->GetAssetState(large) == AssetState::Ready);
		const ScopedActiveAssetManager active(manager);

		StreamingPanel panel;
		const ImVec2 size(1200.0f, 800.0f);
		DrawPanel(harness, panel, panelContext, size);
		const size_t listed = manager->GetResidencyInfo().size();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((panel.IsBuildingTable() || panel.GetTableRowCount() < listed) && std::chrono::steady_clock::now() < deadline)
			DrawPanel(harness, panel, panelContext, size);
		CHECK(panel.GetTableBuildCount() >= 1);
		// By GPU memory, largest first (the default), but the failure leads: it is what the assets pill sends people here for.
		const std::vector<std::string> labels = panel.GetShownLabels();
		REQUIRE(labels.size() == listed);
		REQUIRE(labels.size() >= 3);
		CHECK(labels[0] == "Fake/Broken");
		CHECK(labels[1] == manager->GetMetadata(large)->Path);
		CHECK(labels[2] == manager->GetMetadata(smallAsset)->Path);
	}

	TEST_CASE("The Streaming table shows the mip levels of assets that stream them")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedFakeLoader loader;
		ImGuiHarness harness({ ImVec2(1600.0f, 900.0f), 1.0f });
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle streaming = manager->Add(0x76001, MakeUsage(0, 3 << 20));
		manager->RequestLoad(streaming);
		REQUIRE(manager->WaitForPendingLoads());
		manager->Update();
		const Ref<FakeSizedAsset> streamed = std::dynamic_pointer_cast<FakeSizedAsset>(manager->PeekLoadedAsset(streaming));
		REQUIRE(streamed);
		streamed->SetDetailLevels(AssetDetailLevels { 4, 2, 12, 3 << 20 });
		const ScopedActiveAssetManager active(manager);

		// "Resident / wanted", 0 the full resolution.
		CHECK(StreamingPanel::FormatMipLevels(AssetDetailLevels { 4, 2, 12, 3 << 20 }) == "4 / 2");
		CHECK(StreamingPanel::FormatMipLevels(AssetDetailLevels { 0, 0, 1, 64 }) == "0 / 0");

		StreamingPanel panel;
		const ImVec2 size(1600.0f, 600.0f);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		do
		{
			DrawPanel(harness, panel, panelContext, size);
		} while ((panel.IsBuildingTable() || panel.GetTableRowCount() < manager->GetResidencyInfo().size()) && std::chrono::steady_clock::now() < deadline);
		DrawPanel(harness, panel, panelContext, size);
		ImGuiTable* table = FindTable(ImGui::FindWindowByName("Panel"), 8);
		REQUIRE(table);
		bool mipShown = false;
		for (int column = 0; column < table->ColumnsCount; column++)
			mipShown = mipShown || (std::string(ImGui::TableGetColumnName(table, column)) == "Mip" && table->Columns[column].IsEnabled);
		CHECK(mipShown);
		CHECK(ImGuiHarness::CountFallbackGlyphs(ImGui::FindWindowByName("Panel")) == 0);
	}

	TEST_CASE("The Streaming table's fixed columns hold their headers at 150%")
	{
		// Wide enough for every column, not for the overview beside the table (60 text heights).
		ImGuiHarness harness({ ImVec2(1250.0f, 1600.0f), 1.5f });
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };
		SceneTestAssets assets;
		StreamingPanel panel;
		DrawPanel(harness, panel, panelContext, ImVec2(1250.0f, 1600.0f), 3);
		ImGuiTable* table = FindTable(ImGui::FindWindowByName("Panel"), 8);
		REQUIRE(table);
		for (int column = 0; column < table->ColumnsCount; column++)
		{
			const ImGuiTableColumn& entry = table->Columns[column];
			CAPTURE(std::string(ImGui::TableGetColumnName(table, column)));
			CHECK(entry.IsEnabled);
			// The header's text and sort arrow end within the column.
			CHECK(entry.ContentMaxXHeadersIdeal <= entry.WorkMaxX + 0.5f);
		}
	}

	TEST_CASE("The Streaming table's Type column shows every asset type whole")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedFakeLoader loader;
		ImGuiHarness harness({ ImVec2(2400.0f, 1350.0f), 1.5f });
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };

		// An asset of every type (each fails to load: the fake data is no type's stored form, so the table lists them all).
		std::vector<AssetType> types;
		for (uint32_t value = static_cast<uint32_t>(AssetType::Scene);; value++)
		{
			const AssetType type = static_cast<AssetType>(value);
			if (AssetTypeFromString(AssetTypeToString(type)) != type)
				break;
			types.push_back(type);
		}
		REQUIRE(types.size() >= 11);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		uint64_t handle = 0x75001;
		for (const AssetType type : types)
			manager->RequestLoad(manager->Add(handle++, MakeUsage(0, 1 << 10), 0, type));
		REQUIRE(manager->WaitForPendingLoads());
		manager->Update();
		const ScopedActiveAssetManager active(manager);

		StreamingPanel panel;
		const ImVec2 size(1600.0f, 900.0f);
		DrawPanel(harness, panel, panelContext, size);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((panel.IsBuildingTable() || panel.GetTableRowCount() < types.size()) && std::chrono::steady_clock::now() < deadline)
			DrawPanel(harness, panel, panelContext, size);
		DrawPanel(harness, panel, panelContext, size);
		REQUIRE(panel.GetShownLabels().size() >= types.size());
		ImGuiTable* table = FindTable(ImGui::FindWindowByName("Panel"), 8);
		REQUIRE(table);
		int typeColumn = -1;
		for (int column = 0; column < table->ColumnsCount; column++)
		{
			if (std::string_view(ImGui::TableGetColumnName(table, column)) == "Type")
				typeColumn = column;
		}
		REQUIRE(typeColumn >= 0);
		// What the cells drew ends within the column: no type's name is cut off.
		const ImGuiTableColumn& column = table->Columns[typeColumn];
		CHECK(column.ContentMaxXUnfrozen > column.WorkMinX);
		CHECK(column.ContentMaxXUnfrozen <= column.WorkMaxX + 0.5f);
		// Each type has a name of its own there.
		std::set<std::string> labels;
		for (const AssetType type : types)
			labels.insert(StreamingPanel::GetTypeLabel(type));
		CHECK(labels.size() == types.size());
	}

	TEST_CASE("The Streaming table keeps asset names readable where it is narrow: the least telling columns go first")
	{
		// A laptop's window at 150%: the bottom dock is wide enough for the overview beside the table, but not for every column.
		ImGuiHarness harness({ ImVec2(2400.0f, 1350.0f), 1.5f });
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };
		SceneTestAssets assets;
		StreamingPanel panel;
		struct Shown
		{
			float AssetWidth = 0.0f;
			int Columns = 0;
		};
		const auto draw = [&](float width)
		{
			DrawPanel(harness, panel, panelContext, ImVec2(width, 400.0f), 4);
			ImGuiTable* table = FindTable(ImGui::FindWindowByName("Panel"), 8);
			REQUIRE(table);
			Shown shown;
			shown.AssetWidth = table->Columns[0].WidthGiven;
			for (int column = 0; column < table->ColumnsCount; column++)
				shown.Columns += table->Columns[column].IsEnabled ? 1 : 0;
			return shown;
		};
		const float minAsset = ImGui::GetFontSize() * 14.0f - 1.0f;

		// Wide: every column, the name beside them.
		const Shown wide = draw(3000.0f);
		CHECK(wide.Columns == 8);
		CHECK(wide.AssetWidth >= minAsset);
		// The bottom dock of the laptop's window: some columns go, the name keeps its width.
		const Shown laptop = draw(1300.0f);
		CHECK(laptop.Columns < 8);
		CHECK(laptop.Columns >= 4);
		CHECK(laptop.AssetWidth >= minAsset);
		// Wide again: the columns come back.
		CHECK(draw(3000.0f).Columns == 8);
	}

	TEST_CASE("Instruments opened in a layout saved before they existed join the Console")
	{
		// A layout saved by an editor without the instruments: their windows' settings removed.
		std::string ini;
		{
			HarnessEditor first({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIOldLayout"));
			first.Frames(2);
			size_t size = 0;
			const char* data = ImGui::SaveIniSettingsToMemory(&size);
			ini.assign(data, size);
		}
		for (const char* id : { "###Streaming]", "###Performance]" })
		{
			size_t start = ini.find(id);
			while (start != std::string::npos)
			{
				start = ini.rfind("[Window][", start);
				REQUIRE(start != std::string::npos);
				const size_t end = ini.find("\n\n", start);
				ini.erase(start, end == std::string::npos ? std::string::npos : end + 2 - start);
				start = ini.find(id);
			}
		}

		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIOldLayoutReopened"), ini);
		editor.Frames(2);
		REQUIRE(editor.Click("Status.Frame"));
		editor.Frames(2);
		ImGuiWindow* performance = editor.FindPanelWindow(EditorPanels::c_Performance);
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(performance);
		REQUIRE(console);
		CHECK(console->DockId != 0);
		CHECK(performance->DockId == console->DockId);
	}

	TEST_CASE("The frame and assets pills explain themselves while hovered")
	{
		// Their tooltips are written only while they show (UI::IsTooltipDue).
		HarnessEditor editor({}, WithFeatureProject("EditorUIPillTooltips"));
		editor.Frames(2);
		for (const char* pill : { "Status.Frame", "Status.Assets" })
		{
			CAPTURE(pill);
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(pill);
			REQUIRE(item);
			editor.Harness.MoveMouse(item->GetCenter());
			editor.Frames(8, 0.1f);
			ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
			REQUIRE(tooltip);
			CHECK(tooltip->Active);
			CHECK(tooltip->DrawList->VtxBuffer.Size > 0);
			editor.Harness.MoveMouse(ImVec2(item->Min.x, item->Min.y - ImGui::GetFrameHeight() * 4.0f));
			editor.Frames(2);
			CHECK_FALSE(tooltip->Active);
		}
	}

	TEST_CASE("The assets pill shows budget pressure")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUIBudgetPressure"));
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// Without a graphics device the pools have no budgets: the pill is quiet.
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.TextSecondary));

		// The built-in assets alone (never evicted) exceed a tiny CPU budget.
		editor.Run("asset.setBudget", { { "cpuMB", 0.0001 } });
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.Error));
		CHECK(editor.GetPillText("Status.Assets").find("CPU ") != std::string::npos);
		CHECK(editor.GetPillText("Status.Assets").find("/0 MB") != std::string::npos);

		// Close to a CPU budget beside a roomy texture budget: the pill shows the CPU pool, whose pressure it is colored by
		// (the pools added up would read far below the budget).
		const nlohmann::json stats = editor.Run("asset.stats");
		const double cpuMB = stats["pools"]["cpu"]["residentBytes"].get<double>() / (1024.0 * 1024.0);
		REQUIRE(cpuMB > 0.0);
		editor.Run("asset.setBudget", { { "cpuMB", cpuMB / 0.9 }, { "gpuTexturesMB", 100000 } });
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.Warning));
		const std::string text = editor.GetPillText("Status.Assets");
		CAPTURE(text);
		CHECK(text.find("CPU ") != std::string::npos);
		CHECK(text.find("textures") == std::string::npos);

		// Room again: quiet again.
		editor.Run("asset.setBudget", { { "cpuMB", 100000 } });
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.TextSecondary));
	}

	TEST_CASE("The Performance timeline is scaled to the usual frames and marks only frames that hitch")
	{
		FrameProfileSummary summary;
		// An idle editor: half a millisecond per frame, whatever its startup frame took.
		summary.Work = FrameTimeStats { 1.0f, 0.5f, 0.6f, 292.0f };
		CHECK(PerformancePanel::GetTimelineScale(summary) == doctest::Approx(1.0f));
		CHECK(PerformancePanel::GetSpikeThreshold(summary) == doctest::Approx(1000.0f / 60.0f));
		// A game at 12 ms per frame.
		summary.Work = FrameTimeStats { 12.5f, 12.0f, 14.0f, 40.0f };
		CHECK(PerformancePanel::GetTimelineScale(summary) == doctest::Approx(20.0f));
		CHECK(PerformancePanel::GetSpikeThreshold(summary) == doctest::Approx(24.0f));
	}

	TEST_CASE("The frame pill counts jobs and opens the Performance panel, which copies editor.perf's statistics")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIPerformance"));
		FrameProfiler::Get().Reset();
		ProfiledFrames(editor, 3);
		CHECK(editor.GetPillText("Status.Frame").find("jobs") != std::string::npos);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));

		REQUIRE(editor.Click("Status.Frame"));
		ProfiledFrames(editor, 3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
		REQUIRE(window);
		CHECK(window->Active);
		CHECK(UI::ItemProbe::Find("Performance.Timeline").has_value());

		// Hovering a frame of the timeline explains it.
		const std::optional<UI::ItemProbe::Item> timeline = UI::ItemProbe::Find("Performance.Timeline");
		REQUIRE(timeline);
		editor.Harness.MoveMouse(ImVec2(timeline->Max.x - 1.0f, timeline->GetCenter().y));
		ProfiledFrames(editor, 2);
		ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
		REQUIRE(tooltip);
		// Its swatches and the stage table's are drawn, not glyphs: the table's font has no icons.
		CHECK(ImGuiHarness::CountFallbackGlyphs(tooltip) == 0);
		CHECK(ImGuiHarness::CountFallbackGlyphs(window) == 0);

		// Copy as JSON: what editor.perf returns for every recorded frame.
		REQUIRE(editor.Click("Performance.CopyJson"));
		std::string error;
		const std::optional<nlohmann::json> copied = JsonUtils::Parse(editor.Harness.GetClipboard(), &error);
		REQUIRE_MESSAGE(copied, error);
		const nlohmann::json perf = editor.Run("editor.perf", { { "frames", 600 } });
		for (const char* key : { "frames", "frame", "framesPerSecond", "stageSum", "untimed", "jobs", "stages" })
		{
			CAPTURE(key);
			CHECK(copied->contains(key));
		}
		CHECK((*copied)["frames"].get<uint32_t>() >= 8);
		CHECK((*copied)["stages"].size() == perf["stages"].size());
		CHECK((*copied)["stages"]["Assets.Update"]["avgMs"].get<float>() > 0.0f);
	}

	TEST_CASE("The Performance panel shows the GPU's frames under the CPU's, and the frame pill what bounds them")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIPerformanceGPU"));
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		const bool expected = profiler.AreGPUTimesExpected();
		profiler.SetGPUTimesExpected(true);
		const RendererGPUStages& stages = RendererGPUStages::Get();
		// Fake GPU times, a frame late as the renderer delivers them: far longer than the frames, so the GPU bounds them.
		// Every fourth frame's never come: its GPU column stays blank.
		const auto profiledFrames = [&](int count)
		{
			for (int frame = 0; frame < count; frame++)
			{
				profiler.BeginFrame();
				editor.Frames(1);
				profiler.EndFrame(0.002);
				const uint64_t index = profiler.GetTotalFrameCount() - 1;
				if (index % 4 != 3)
					REQUIRE(profiler.RecordGPUFrame(index, std::vector<GPUStageTime> { { stages.Opaque, 600.0f }, { stages.Shadows, 400.0f } }));
			}
		};
		profiledFrames(8);
		REQUIRE(editor.Click("Status.Frame"));
		profiledFrames(8);
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
		REQUIRE(window);
		const std::optional<UI::ItemProbe::Item> cpu = UI::ItemProbe::Find("Performance.Timeline");
		const std::optional<UI::ItemProbe::Item> gpu = UI::ItemProbe::Find("Performance.GPUTimeline");
		REQUIRE(cpu);
		REQUIRE(gpu);
		// The same frames in the same columns: the GPU's strip under the CPU's, as wide.
		CHECK(gpu->Min.y >= cpu->Max.y);
		CHECK(gpu->Min.x == doctest::Approx(cpu->Min.x));
		CHECK(gpu->Max.x == doctest::Approx(cpu->Max.x));
		CHECK(UI::ItemProbe::Find("Performance.GPUSection").has_value());
		const std::optional<UI::ItemProbe::Item> bound = UI::ItemProbe::Find("Performance.Bound");
		REQUIRE(bound);
		CHECK(bound->Text.starts_with("GPU-bound: GPU 1000.0 ms busy"));
		CHECK(ImGuiHarness::CountFallbackGlyphs(window) == 0);

		// A GPU frame under the mouse explains itself too.
		editor.Harness.MoveMouse(ImVec2(gpu->Max.x - 2.0f, gpu->GetCenter().y));
		profiledFrames(2);
		ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
		REQUIRE(tooltip);
		CHECK(tooltip->Active);
		CHECK(ImGuiHarness::CountFallbackGlyphs(tooltip) == 0);

		// The frame pill's tooltip names the bound.
		const std::optional<UI::ItemProbe::Item> pill = UI::ItemProbe::Find("Status.Frame");
		REQUIRE(pill);
		editor.Harness.MoveMouse(pill->GetCenter());
		editor.Frames(8, 0.1f);
		const std::optional<UI::ItemProbe::Item> pillTooltip = UI::ItemProbe::Find("Status.FrameTooltip");
		REQUIRE(pillTooltip);
		CHECK(pillTooltip->Text.find("GPU-bound: GPU") != std::string::npos);

		profiler.SetGPUTimesExpected(expected);
		profiler.Reset();
	}

	TEST_CASE("What bounds the frames reads as a line for the panel and the frame pill")
	{
		FrameProfileSummary summary;
		summary.Work.P50 = 9.8f;
		summary.Interval.P50 = 12.3f;
		summary.GPU.Frames = 10;
		summary.GPU.Busy.P50 = 11.8f;
		summary.GPU.BoundBy = FrameBound::GPU;
		CHECK(PerformancePanel::DescribeBound(summary) == "GPU-bound: GPU 11.8 ms busy of 12.3 ms frames (CPU 9.8 ms)");
		summary.GPU.BoundBy = FrameBound::CPU;
		CHECK(PerformancePanel::DescribeBound(summary).starts_with("CPU-bound: CPU 9.8 ms of 12.3 ms frames"));
		summary.GPU.BoundBy = FrameBound::Capped;
		CHECK(PerformancePanel::DescribeBound(summary).starts_with("Capped: 12.3 ms frames"));
		summary.GPU.BoundBy = FrameBound::Unknown;
		CHECK(PerformancePanel::DescribeBound(summary).starts_with("Bound by: unknown"));
	}

	TEST_CASE("The Performance panel opened before any frame was profiled shows the first frames at once")
	{
		ImGuiHarness harness;
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		PerformancePanel panel;
		DrawPanel(harness, panel, panelContext, ImVec2(800.0f, 600.0f));
		REQUIRE(panel.GetSummary());
		CHECK(panel.GetSummary()->Frames == 0);
		// Well within the refresh interval: statistics without frames are taken again anyway.
		profiler.BeginFrame();
		profiler.EndFrame(0.001);
		DrawPanel(harness, panel, panelContext, ImVec2(800.0f, 600.0f));
		CHECK(panel.GetSummary()->Frames == 1);
	}

	TEST_CASE("The harness counts glyphs a font lacks")
	{
		// The check the panels' tests rely on: an icon drawn in the monospaced font (which has none) is its fallback glyph.
		ImGuiHarness harness;
		uint32_t regular = 0;
		uint32_t mono = 0;
		for (const bool monospaced : { false, true })
		{
			harness.Frame([&]()
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
				ImGui::Begin("Glyphs", nullptr, ImGuiWindowFlags_NoSavedSettings);
				UI::PushFont(monospaced ? UI::EditorFont::Mono : UI::EditorFont::Regular, UI::TextSize::Body);
				ImGui::TextUnformatted(Icons::Square);
				ImGui::PopFont();
				ImGui::End();
			});
			(monospaced ? mono : regular) = ImGuiHarness::CountFallbackGlyphs(ImGui::FindWindowByName("Glyphs"));
		}
		CHECK(regular == 0);
		CHECK(mono == 1);
	}

	TEST_CASE("The Performance panel keeps its stage table and totals in view in short and narrow docks")
	{
		// The timeline leaves the table room: its header and the frame's totals are in view, and only the table scrolls.
		const auto checkTableInView = [](ImGuiWindow& window)
		{
			const std::optional<UI::ItemProbe::Item> header = UI::ItemProbe::Find("Performance.StagesHeader");
			const std::optional<UI::ItemProbe::Item> totals = UI::ItemProbe::Find("Performance.FrameRow");
			REQUIRE(header);
			REQUIRE(totals);
			CHECK(IsVisibleIn(*header, window));
			CHECK(IsVisibleIn(*totals, window));
			CHECK(totals->Min.y > header->Min.y);
			CHECK(window.ScrollMax.y == 0.0f);
		};
		{
			// 150% in a window of 1600 x 900: the bottom dock is a few rows tall, and wide.
			HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.5f }, WithFeatureProject("EditorUIPerformanceShort"));
			FrameProfiler::Get().Reset();
			ProfiledFrames(editor, 2);
			editor.Run("editor.showPanel", { { "panel", "Performance" } });
			ProfiledFrames(editor, 3);
			ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
			REQUIRE(window);
			REQUIRE(window->Active);
			checkTableInView(*window);
			// The timeline beside the table, at the panel's full height.
			const std::optional<UI::ItemProbe::Item> timeline = UI::ItemProbe::Find("Performance.Timeline");
			REQUIRE(timeline);
			CHECK(timeline->Max.x < UI::ItemProbe::Find("Performance.StagesHeader")->Min.x);
		}
		{
			// A narrow, tall panel: the timeline above the table.
			ImGuiHarness harness;
			EditorContext context { EditorContextSpecification { false } };
			EditorCommandRegistry commands;
			EditorCommandRunner runner;
			EditorPanelContext panelContext { context, commands, runner };
			PerformancePanel panel;
			DrawPanel(harness, panel, panelContext, ImVec2(500.0f, 700.0f), 3);
			ImGuiWindow* window = ImGui::FindWindowByName("Panel");
			REQUIRE(window);
			checkTableInView(*window);
			const std::optional<UI::ItemProbe::Item> timeline = UI::ItemProbe::Find("Performance.Timeline");
			REQUIRE(timeline);
			CHECK(timeline->Max.y < UI::ItemProbe::Find("Performance.StagesHeader")->Min.y);
		}
	}

	TEST_CASE("editor.showPanel opens a panel and brings it to the front")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIShowPanel"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		CHECK(editor.Run("editor.showPanel", { { "panel", "Streaming" } })["panel"] == "Streaming");
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Streaming);
		REQUIRE(window);
		CHECK(window->Active);
		CHECK(window->DockTabIsVisible); // The visible tab of its dock node

		// Only the editor's docked panels; the launcher is no panel to show.
		for (const char* panel : { "Nope", "Welcome" })
		{
			CAPTURE(panel);
			const EditorCommandResult result = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showPanel", { { "panel", panel } });
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		}
	}

	TEST_CASE("editor.showPanel before the first frame shows the panel, whatever layout the frame starts")
	{
		std::string savedLayout;
		{
			// No saved layout: the first frame builds the default one, with the instruments closed.
			HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIShowPanelFirst"));
			editor.Run("editor.showPanel", { { "panel", "Performance" } });
			editor.Frames(3); // The layout settles in the first frames
			CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));
			ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
			REQUIRE(window);
			CHECK(window->Active);
			CHECK(window->DockTabIsVisible);
			// A layout saved with the panel closed.
			editor.Layer->GetPanels().SetOpen(EditorPanels::c_Performance, false);
			editor.Frames(1);
			size_t size = 0;
			const char* data = ImGui::SaveIniSettingsToMemory(&size);
			savedLayout.assign(data, size);
		}
		REQUIRE(savedLayout.find("Performance=0") != std::string::npos);

		// A saved layout is read at the first frame, after a command run before it: the command still wins.
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIShowPanelSaved"));
		editor.Run("editor.showPanel", { { "panel", "Performance" } });
		ImGui::LoadIniSettingsFromMemory(savedLayout.c_str(), savedLayout.size());
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
		REQUIRE(window);
		CHECK(window->Active);
		CHECK(window->DockTabIsVisible);
	}

	TEST_CASE("editor.showPanel fails while the launcher shows")
	{
		HarnessEditor editor;
		editor.Frames(2);
		REQUIRE(editor.Layer->IsLauncherShown());
		const EditorCommandResult result = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showPanel", { { "panel", "Streaming" } });
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::Failed);
		CHECK(result.Error.find("project") != std::string::npos);
	}

	TEST_CASE("A legend lines up its counts whatever its labels")
	{
		ImGuiHarness harness;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const std::vector<UI::LegendLine> lines = { { colors.Warning, "Loading", "0" }, { colors.Error, "Failed", "12345" }, { colors.Info, "LOD 3+", "7" } };
		harness.Frame([&]()
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
			ImGui::Begin("Legend", nullptr, ImGuiWindowFlags_NoSavedSettings);
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
			UI::Legend("Test.Legend", ImVec2(10.0f, 390.0f), lines, "2 boxes: meshes not there yet");
			ImGui::PopFont();
			ImGui::End();
		});
		float labelsEnd = 0.0f;
		std::optional<float> countsEnd;
		for (size_t index = 0; index < lines.size(); index++)
		{
			CAPTURE(index);
			const std::optional<UI::ItemProbe::Item> label = UI::ItemProbe::Find("Test.Legend.Label" + std::to_string(index));
			const std::optional<UI::ItemProbe::Item> count = UI::ItemProbe::Find("Test.Legend.Count" + std::to_string(index));
			REQUIRE(label);
			REQUIRE(count);
			CHECK(label->Text == lines[index].Label);
			CHECK(count->Text == lines[index].Count);
			labelsEnd = std::max(labelsEnd, label->Max.x);
			if (countsEnd)
				CHECK(count->Max.x == doctest::Approx(*countsEnd)); // Right-aligned in one column
			countsEnd = count->Max.x;
		}
		for (size_t index = 0; index < lines.size(); index++)
			CHECK(UI::ItemProbe::Find("Test.Legend.Count" + std::to_string(index))->Min.x > labelsEnd);
		CHECK(ImGuiHarness::CountFallbackGlyphs(ImGui::FindWindowByName("Legend")) == 0);
	}
}
