#include "Panels/ProblemsPanel.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <imgui.h>

#include <string_view>
#include <system_error>

namespace Strata
{

	namespace
	{

		// The Line column, in text heights.
		constexpr float c_LineColumnInFontSizes = 4.0f;
		// The File column's share of the table's width.
		constexpr float c_FileColumnWeight = 0.4f;
		// Rows recorded in the item probe (for tests).
		constexpr size_t c_ProbedRows = 8;
		// Functions of the script SDK's internals start with it.
		constexpr std::string_view c_SDKInternalNamespace = "Strata::Detail::";

		// The file a compiler or the symbolizer named, found on disk: as given when absolute, else in the project, its
		// script directory or the build tree (compilers name files relative to where they ran).
		std::filesystem::path ResolveSource(const EditorContext& context, const std::string& file, const std::filesystem::path& buildDirectory)
		{
			if (file.empty() || !context.GetProject())
				return {};
			const std::filesystem::path path = FileSystem::FromUTF8(file);
			if (path.is_absolute())
				return FileSystem::IsRegularFile(path) ? path : std::filesystem::path();
			const Project& project = *context.GetProject();
			for (const std::filesystem::path& base : { project.GetProjectDirectory(), project.GetScriptSourceDirectory(), buildDirectory })
			{
				if (base.empty())
					continue;
				const std::filesystem::path candidate = (base / path).lexically_normal();
				if (FileSystem::IsRegularFile(candidate))
					return candidate;
			}
			return {};
		}

		// A file as the File column shows it: relative to the project directory when it lies there (what a person knows the
		// project's files by), else as the compiler or the symbolizer named it.
		std::string MakeShownFile(const EditorContext& context, const std::string& file, const std::filesystem::path& source)
		{
			if (source.empty() || !context.GetProject())
				return file;
			const std::filesystem::path& directory = context.GetProject()->GetProjectDirectory();
			std::filesystem::path relative = FileSystem::GetRelativePath(source, directory);
			if (relative.empty())
			{
				// The compiler may name the folder in another form than the project (a short 8.3 name, a link): compare where
				// both lead.
				std::error_code error;
				const std::filesystem::path resolvedSource = std::filesystem::weakly_canonical(source, error);
				const std::filesystem::path resolvedDirectory = error ? std::filesystem::path() : std::filesystem::weakly_canonical(directory, error);
				if (!error)
					relative = FileSystem::GetRelativePath(resolvedSource, resolvedDirectory);
			}
			return relative.empty() ? file : FileSystem::ToUTF8(relative);
		}

		std::string MakeFaultKey(const std::optional<ScriptFault>& fault)
		{
			if (!fault)
				return {};
			return fmt::format("{}|{}|{}|{}|{}", fault->ModuleName, fault->Method, fault->Entity.ToString(), fault->Description, fault->Symbolized);
		}

		const char* GetIcon(ProblemsPanel::RowKind kind)
		{
			switch (kind)
			{
				case ProblemsPanel::RowKind::Error:   return Icons::CircleX;
				case ProblemsPanel::RowKind::Warning: return Icons::TriangleAlert;
				case ProblemsPanel::RowKind::Crash:   return Icons::Bug;
				case ProblemsPanel::RowKind::Frame:   return Icons::CornerDownRight;
			}
			return Icons::CircleAlert;
		}

	}

	void ProblemsPanel::OnUpdate(EditorPanelContext& context)
	{
		// Idle: one number and an empty string compared (a string of a few fields while a crash is shown).
		const uint64_t buildID = context.Context.GetScriptBuilder().GetLastResult().ID;
		const std::optional<ScriptFault>& fault = context.Context.GetLastScriptFault();
		if (m_Built && buildID == m_BuildID && (fault.has_value() == !m_FaultKey.empty()) && (!fault || MakeFaultKey(fault) == m_FaultKey))
			return;
		Rebuild(context);
	}

	void ProblemsPanel::Rebuild(EditorPanelContext& context)
	{
		const EditorContext& editor = context.Context;
		const ScriptBuildResult& build = editor.GetScriptBuilder().GetLastResult();
		const std::optional<ScriptFault>& fault = editor.GetLastScriptFault();
		m_Built = true;
		m_BuildID = build.ID;
		m_FaultKey = MakeFaultKey(fault);
		m_Rows.clear();
		m_FileTexts.clear();
		m_FileTextsWidth = -1.0f;
		m_ErrorCount = 0;
		m_WarningCount = 0;

		if (fault)
		{
			Row crash;
			crash.Kind = RowKind::Crash;
			const std::string where = fault->ClassName.empty() ? fault->Method : fault->ClassName + "::" + fault->Method;
			crash.Text = fault->EntityName.empty() ? fmt::format("Crash in {}: {}", where, fault->Description)
				: fmt::format("Crash in {} on '{}': {}", where, fault->EntityName, fault->Description);
			if (fault->Location)
			{
				crash.File = fault->Location->File;
				crash.Line = fault->Location->Line;
				crash.Source = ResolveSource(editor, crash.File, {});
				crash.ShownFile = MakeShownFile(editor, crash.File, crash.Source);
			}
			m_Rows.push_back(std::move(crash));
			// The stack's frames with a source: where the crash came from. Calls inlined at one address share it, so the
			// location's own frame is the one at its address with its line. The SDK's internals (Strata::Detail: the thunks
			// the engine calls scripts through) are no code a person acts on; script.status keeps them.
			const auto isLocation = [&](const ScriptCodeLocation& frame)
			{
				return fault->Location && frame.Address == fault->Location->Address && frame.File == fault->Location->File && frame.Line == fault->Location->Line;
			};
			for (const ScriptCodeLocation& frame : fault->Stack)
			{
				if (frame.File.empty() || isLocation(frame) || frame.Function.starts_with(c_SDKInternalNamespace))
					continue;
				Row row;
				row.Kind = RowKind::Frame;
				row.Text = frame.Function.empty() ? fmt::format("{}+0x{:X}", frame.Module, frame.ModuleOffset) : frame.Function;
				if (frame.Inlined)
					row.Text += " (inlined)";
				row.File = frame.File;
				row.Line = frame.Line;
				row.Source = ResolveSource(editor, row.File, {});
				row.ShownFile = MakeShownFile(editor, row.File, row.Source);
				m_Rows.push_back(std::move(row));
			}
		}

		for (const ScriptDiagnostic& diagnostic : build.Diagnostics)
		{
			Row row;
			row.Kind = diagnostic.Severity == "warning" ? RowKind::Warning : RowKind::Error;
			row.Text = diagnostic.Code.empty() ? diagnostic.Message : diagnostic.Code + ": " + diagnostic.Message;
			row.File = diagnostic.File;
			row.Line = diagnostic.Line;
			row.Column = diagnostic.Column;
			row.Source = ResolveSource(editor, row.File, build.BuildDirectory);
			row.ShownFile = MakeShownFile(editor, row.File, row.Source);
			(row.Kind == RowKind::Warning ? m_WarningCount : m_ErrorCount)++;
			m_Rows.push_back(std::move(row));
		}

		if (build.ID == 0)
			m_Summary = "No script build in this session yet.";
		else if (build.Success)
			m_Summary = fmt::format("Build {} succeeded{}.", build.ID, m_WarningCount > 0 ? fmt::format(" with {} warning{}", m_WarningCount, m_WarningCount == 1 ? "" : "s")
				: std::string());
		else
			m_Summary = fmt::format("Build {} failed: {} error{}, {} warning{}.", build.ID, m_ErrorCount, m_ErrorCount == 1 ? "" : "s", m_WarningCount,
				m_WarningCount == 1 ? "" : "s");
		if (fault)
			m_Summary += fault->Symbolized || !fault->Location ? " The scripts crashed." : " The scripts crashed (finding where).";
	}

	bool ProblemsPanel::OpenRow(EditorPanelContext& context, size_t index)
	{
		m_LastOpenError.clear();
		if (index >= m_Rows.size())
			return false;
		const Row& row = m_Rows[index];
		if (row.Source.empty())
		{
			m_LastOpenError = row.File.empty() ? std::string("This entry names no source file") : fmt::format("There is no file '{}'", row.File);
			return false;
		}
		std::string how;
		if (!context.Context.OpenScriptSource(row.Source, row.Line, row.Column, &how, &m_LastOpenError))
		{
			ST_ERROR("Problems: {}", m_LastOpenError);
			return false;
		}
		return true;
	}

	void ProblemsPanel::OnImGuiRender(EditorPanelContext& context)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const bool building = context.Context.GetScriptBuilder().IsRunning();
		if (UI::ToolbarButton("Problems.Build", Icons::Hammer, "Build Scripts", "Build the project's scripts (Ctrl+B)", UI::ButtonStyle { .Enabled =
			context.Context.HasProject() && !building }))
		{
			context.Runner.Run(context.Context, context.Commands, "script.build", { { "wait", true } }, [](const EditorCommandResult&) {});
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, m_ErrorCount > 0 ? colors.Error : colors.TextSecondary);
		ImGui::TextUnformatted(building ? "Building..." : m_Summary.c_str());
		ImGui::PopStyleColor();
		UI::ItemProbe::Record("Problems.Summary", true, 0, m_Summary);
		if (!m_LastOpenError.empty())
		{
			ImGui::SameLine();
			ImGui::TextColored(colors.Warning, "%s", m_LastOpenError.c_str());
		}

		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
			| ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable("Problems", 3, flags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 3.0f))))
			return;
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch, 1.0f - c_FileColumnWeight);
		ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, c_FileColumnWeight);
		ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * c_LineColumnInFontSizes);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_Rows.size()));
		std::optional<size_t> open;
		while (clipper.Step())
		{
			for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++)
			{
				const size_t index = static_cast<size_t>(line);
				const Row& row = m_Rows[index];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(line);
				const ImVec4& color = row.Kind == RowKind::Warning ? colors.Warning : (row.Kind == RowKind::Frame ? colors.TextSecondary : colors.Error);
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(GetIcon(row.Kind));
				ImGui::PopStyleColor();
				ImGui::SameLine();
				if (row.Kind == RowKind::Frame)
					ImGui::Indent(ImGui::GetFontSize());
				// An unlabeled row with the text drawn after it: messages may hold "##" (token pasting), which a label would cut.
				ImGui::Selectable("##Row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
				// The first rows only: the probe keeps a fixed set of keys.
				if (index < c_ProbedRows)
					UI::ItemProbe::Record(UI::NumberedProbeKey("Problems.Row.", index), !row.Source.empty(), 0, row.Text);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					open = index;
				if (UI::IsTooltipDue())
				{
					const std::string where = row.File.empty() ? std::string() : (row.Line > 0 ? fmt::format("{}({})\n", row.File, row.Line) : row.File + "\n");
					ImGui::SetTooltip("%s\n%s%s", row.Text.c_str(), where.c_str(), row.Source.empty() ? (row.File.empty() ? "No source file"
						: "The file is not on this machine") : "Double-click to open it in the external editor");
				}
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::TextUnformatted(row.Text.c_str());
				if (row.Kind == RowKind::Frame)
					ImGui::Unindent(ImGui::GetFontSize());
				ImGui::PopID();
				ImGui::TableSetColumnIndex(1);
				const float fileWidth = ImGui::GetContentRegionAvail().x;
				if (fileWidth != m_FileTextsWidth || m_FileTexts.size() != m_Rows.size())
				{
					m_FileTexts.clear();
					for (const Row& each : m_Rows)
						m_FileTexts.push_back(UI::ElideStart(each.ShownFile, fileWidth));
					m_FileTextsWidth = fileWidth;
				}
				ImGui::TextUnformatted(m_FileTexts[index].c_str());
				if (index < c_ProbedRows)
					UI::ItemProbe::Record(UI::NumberedProbeKey("Problems.File.", index), true, 0, m_FileTexts[index]);
				ImGui::TableSetColumnIndex(2);
				if (row.Line > 0)
					ImGui::Text("%u", row.Line);
			}
		}
		ImGui::EndTable();
		if (open)
			OpenRow(context, *open);
	}

}
