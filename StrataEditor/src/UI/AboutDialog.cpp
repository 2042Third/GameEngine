#include "UI/AboutDialog.h"

#include "UI/EditorFonts.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace Strata::EmbeddedFiles
{

	// ThirdPartyNotices.md, embedded by StrataEditor/CMakeLists.txt.
	std::span<const uint8_t> GetThirdPartyNotices();

}

namespace Strata
{

	namespace
	{

		// The dialog's content width and the notices' height, in text heights.
		constexpr float c_WidthInFontSizes = 44.0f;
		constexpr float c_NoticesHeightInFontSizes = 16.0f;
		// The mark next to the name, in text heights.
		constexpr float c_MarkSizeInFontSizes = 4.0f;

	}

	std::string_view AboutDialog::GetThirdPartyNotices()
	{
		const std::span<const uint8_t> bytes = EmbeddedFiles::GetThirdPartyNotices();
		return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

	std::vector<std::pair<std::string, std::string>> AboutDialog::DescribeEnvironment(const EditorEnvironment& environment)
	{
		std::vector<std::pair<std::string, std::string>> rows;
		rows.emplace_back("Version", c_EngineVersion);
		rows.emplace_back("Commit", c_EngineCommit);
		rows.emplace_back("Platform", std::string(Platform::GetName()));
		if (environment.GraphicsDevice)
		{
			const GraphicsDeviceInfo& device = *environment.GraphicsDevice;
			rows.emplace_back("GPU", device.AdapterName.empty() ? std::string("Unknown") : device.AdapterName);
			rows.emplace_back("Driver", device.DriverVersion.empty() ? std::string("Unknown") : device.DriverVersion);
			rows.emplace_back("Graphics API", fmt::format("{} {}", device.API, device.APIVersion));
		}
		else
		{
			rows.emplace_back("GPU", "None: the editor runs without a graphics device");
		}
		rows.emplace_back("Startup", environment.StartupSeconds ? fmt::format("{:.2f} s to the first frame", *environment.StartupSeconds) : std::string("Not measured"));
		rows.emplace_back("UI scale", fmt::format("{:.0f}%", std::round(environment.UIScale * 100.0f)));
		return rows;
	}

	std::string AboutDialog::FormatDetails(const EditorEnvironment& environment)
	{
		std::string text = fmt::format("{} Editor\n", c_EngineName);
		for (const auto& [label, value] : DescribeEnvironment(environment))
			text += fmt::format("{}: {}\n", label, value);
		return text;
	}

	void AboutDialog::Draw(const EditorEnvironment& environment)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(c_Popup);
			m_OpenRequested = false;
			if (m_Notices.empty())
				m_Notices = UI::ParseMarkdown(GetThirdPartyNotices());
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "About Strata", &open);
		if (!m_Open)
			return;
		const bool editing = ImGui::IsAnyItemActive();
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		const float width = fontSize * c_WidthInFontSizes;

		// The mark, the name and the version.
		UI::BrandMark(std::round(fontSize * c_MarkSizeInFontSizes));
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
		ImGui::BeginGroup();
		UI::Heading(c_EngineName, UI::TextSize::Display);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
		ImGui::TextUnformatted("Worlds built in layers.");
		ImGui::PopStyleColor();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextDisabled);
		ImGui::Text("%s  %s", c_EngineVersion, c_EngineCommit);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::EndGroup();
		ImGui::Spacing();
		ImGui::Spacing();

		// The details of this build and machine.
		if (ImGui::BeginTable("Details", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX, ImVec2(width, 0.0f)))
		{
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			for (const auto& [label, value] : DescribeEnvironment(environment))
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
				ImGui::TextUnformatted(label.c_str(), label.c_str() + label.size());
				ImGui::PopStyleColor();
				ImGui::TableSetColumnIndex(1);
				UI::PushFont(UI::EditorFont::Mono);
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(value.c_str(), value.c_str() + value.size());
				ImGui::PopTextWrapPos();
				ImGui::PopFont();
			}
			ImGui::EndTable();
		}
		ImGui::Spacing();
		if (UI::DialogButton("About.CopyDetails", "Copy Details"))
			ImGui::SetClipboardText(FormatDetails(environment).c_str());
		ImGui::Spacing();
		ImGui::Spacing();

		// What the editor is built from.
		UI::Heading("Third-party software", UI::TextSize::Body);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.Chrome);
		ImGui::BeginChild("Notices", ImVec2(width, fontSize * c_NoticesHeightInFontSizes), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleColor();
		UI::DrawMarkdown("Notices", m_Notices);
		ImGui::EndChild();
		ImGui::Spacing();

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(width - UI::DialogButtonWidth("Close"), 0.0f));
		if (UI::DialogButton("About.Close", "Close", true) || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editing))
			open = false;
		if (!open)
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		UI::EndModal();
	}

}
