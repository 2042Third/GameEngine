#include "EditorLayer.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ExternalEditor.h"
#include "Editor/GameExport.h"
#include "Editor/ProjectSamples.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/SceneRecovery.h"
#include "Editor/SettingsFileWriter.h"
#include "Panels/ActivityPanel.h"
#include "Panels/AssetInspectorPanel.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/GameViewPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/PerformancePanel.h"
#include "Panels/ProjectSettingsPanel.h"
#include "Panels/ProblemsPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/StreamingPanel.h"
#include "Panels/ViewportPanel.h"
#include "Panels/WelcomePanel.h"
#include "UI/BindingCapture.h"
#include "UI/CollisionLayersPage.h"
#include "UI/InputActionsPage.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/FileDialogs.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Notifications.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Events/ApplicationEvent.h>
#include <Strata/Events/KeyEvent.h>
#include <Strata/Events/MouseEvent.h>
#include <Strata/Input/InputActionSet.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Scene/ProjectCollisionLayers.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Strata
{

	namespace
	{

		constexpr const char* c_EditorStatusSection = "editor";
		constexpr const char* c_UnsavedChangesModal = "Unsaved Changes";
		constexpr const char* c_SavingModal = "Saving";
		constexpr const char* c_UnwrittenSettingsModal = "Project Settings Not Written";
		constexpr const char* c_UnwrittenPreferencesModal = "Preferences Not Written";
		// Between the parts of a pill's text.
		constexpr const char* c_Separator = " \xC2\xB7 ";
		// Frames the frame pill's tooltip summarizes for what bounds them: two seconds at 60 frames per second.
		constexpr uint32_t c_FramePillBoundFrames = 120;

		// The default layout, as fractions of the dock space: the viewport gets over 45% of a 3840 x 2054 window at 150%.
		constexpr float c_HierarchyWidth = 0.15f;
		constexpr float c_InspectorWidth = 0.20f;
		constexpr float c_BottomHeight = 0.22f;
		// At large UI scales (a 1600 x 900 window at 200%) the side panels keep room for their rows: at least this many text
		// heights wide (a Hierarchy row's name about eighteen characters), each at most c_MaxSideWidth of the width.
		constexpr float c_MinHierarchyFontSizes = 16.0f;
		constexpr float c_MinInspectorFontSizes = 18.0f;
		constexpr float c_MaxSideWidth = 0.3f;
		// The status bar's pills grow back from their compact texts once the full ones fit with this margin, in font sizes, so
		// that a frame time's digits cannot make them flicker between the two.
		constexpr float c_StatusBarMarginFontSizes = 2.0f;

		// Fields of the snap steps popup, in text heights.
		constexpr float c_SnapFieldWidthInFontSizes = 8.0f;

		// How strongly the viewport's debug views tint the scene: enough to read the color, little enough to see the shapes.
		constexpr float c_DebugTintAlpha = 0.6f;

		EditorContextSpecification MakeContextSpecification(const EditorOptions& options)
		{
			EditorContextSpecification specification;
			specification.RecentProjectsFile = options.RecentProjectsFile;
			specification.RecentProjectsReadOnly = options.RecentProjectsReadOnly;
			specification.PreferencesFile = options.PreferencesFile;
			specification.EditorSettingsFile = options.EditorSettingsFile;
			specification.Settings = options.Settings;
			specification.WatchAssetFiles = options.WatchFiles;
			specification.HotReloadScripts = options.WatchFiles;
			// Play mode runs on the wall clock, except in scripted runs (--frames, --commands), which count frames; and projects
			// open at once with their imports in the background, except in scripted runs, which expect every asset imported.
			specification.TimeDilation = !options.MaxFrames && options.CommandScript.empty();
			specification.ImportInBackground = options.ImportInBackground && !options.MaxFrames && options.CommandScript.empty();
			if (options.ScriptBuild)
				specification.ScriptBuild = *options.ScriptBuild;
			specification.ToolchainEnvironment = options.ToolchainEnvironment;
			return specification;
		}

		glm::vec4 ToVec4(const ImVec4& color)
		{
			return glm::vec4(color.x, color.y, color.z, color.w);
		}

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;

		std::string FormatMegabytes(uint64_t bytes)
		{
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / c_BytesPerMB);
		}

		nlohmann::json DescribeGraphicsDevice(const std::optional<GraphicsDeviceInfo>& device)
		{
			if (!device)
				return nullptr;
			return { { "name", device->AdapterName }, { "driver", device->DriverVersion }, { "api", device->API }, { "apiVersion", device->APIVersion } };
		}

	}

	EditorLayer::EditorLayer(const EditorOptions& options, Scope<EditorHost> host)
		: Layer("EditorLayer"), m_Options(options), m_Host(std::move(host)), m_Context(MakeContextSpecification(options)),
		m_Automation(m_Context, m_Commands, m_CommandRunner), m_ShowImGuiDemo(options.ShowImGuiDemo)
	{
		ST_ASSERT(m_Host, "The editor layer needs a host");
		if (!options.DefaultProjectLocation.empty())
			m_ProjectDialogs.SetDefaultLocation(options.DefaultProjectLocation);
		// The settings windows' pages of complex sections; simple sections need none (a generic page draws them).
		m_SettingsPages.Register(ExternalEditor::c_SettingsSection, [](EditorContext& context, const SettingsSection&, SettingsWindowKind)
		{
			return CreateScope<ExternalEditorPage>(context);
		});
		m_SettingsPages.Register(ProjectCollisionLayers::c_SectionId, [](EditorContext&, const SettingsSection&, SettingsWindowKind)
		{
			return CreateScope<CollisionLayersPage>();
		});
		m_SettingsPages.Register(InputActionSet::c_SectionId, [](EditorContext&, const SettingsSection&, SettingsWindowKind)
		{
			return CreateScope<InputActionsPage>();
		});
		RegisterBuiltinPanels();
		RegisterPanelCommands();
	}

	void EditorLayer::RegisterPanelCommands()
	{
		nlohmann::json panels = nlohmann::json::array();
		for (size_t index = 0; index < m_Panels.GetCount(); index++)
		{
			if (m_Panels.GetDescriptor(index).Placement == EditorPanelPlacement::Docked)
				panels.push_back(m_Panels.GetDescriptor(index).Id);
		}
		panels.push_back("Scene");
		m_Commands.Register({ "editor.showPanel",
			"Opens a panel of the editor's window and brings it to the front, e.g. to show the person at the editor what you are looking at: "
			"Streaming (asset memory per pool against its budget, loads, uploads, the assets), Performance (where the frames go, stage by "
			"stage), Problems (the last script build's errors and warnings, the scripts' crash with where it happened), Activity (every "
			"command the person, agents and scripts ran: editor.activity), ProjectSettings (the project's settings, page by page: what "
			"project.settings lists; needs a project), Console, ContentBrowser, Hierarchy, Inspector, AssetInspector (the "
			"Asset tab: the inspected asset, its settings and preview), Viewport (the Scene view; \"Scene\" works too) or Game (the Game view: the "
			"game through its camera); for the ContentBrowser, scripts: true shows its Scripts root "
			"(the project's script files with their classes, as script.list lists them) and false its asset folders; for the Hierarchy, search "
			"replaces the text of its search field as if the person typed it (a name, c:Light, t:Enemy, and in a world l:Props or cell:3,-1; "
			"\"\" clears it). Changes no scene data and records no undo; fails in an editor without a window (--no-gpu or headless) and while "
			"the launcher shows (no project is open).",
			CommandUtils::ObjectSchema({ { "panel", { { "type", "string" }, { "enum", panels }, { "description", "The panel to show" } } },
				{ "scripts", CommandUtils::BoolSchema("ContentBrowser only: show its Scripts root (true) or its asset folders (false); "
					"without it, the browser shows what it showed") },
				{ "search", CommandUtils::StringSchema("Hierarchy only: the search field's new text (\"\" clears it); without it, the search stays") } },
				{ "panel" }),
			[this](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				std::string panel = arguments.GetString("panel");
				const std::optional<bool> scripts = arguments.Has("scripts") ? std::optional<bool>(arguments.GetBool("scripts", false)) : std::nullopt;
				// The Scene view keeps the id it had as the viewport.
				if (panel == "Scene")
					panel = EditorPanels::c_Viewport;
				const std::optional<std::string> search = arguments.Has("search") ? std::optional<std::string>(arguments.GetString("search", "")) : std::nullopt;
				if (!arguments.IsValid())
					return arguments.Fail();
				const EditorPanelDescriptor* descriptor = m_Panels.FindDescriptor(panel);
				if (!descriptor || descriptor->Placement != EditorPanelPlacement::Docked)
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'panel': the editor has no panel '{}'", panel));
				if (scripts && panel != EditorPanels::c_ContentBrowser)
					return EditorCommandResult::InvalidParameters("Parameter 'scripts': only the ContentBrowser has a Scripts root");
				if (search && panel != EditorPanels::c_Hierarchy)
					return EditorCommandResult::InvalidParameters("Parameter 'search': only the Hierarchy has a search field this command fills");
				if (m_Options.Headless)
					return EditorCommandResult::Fail("This editor runs without a window: it has no panels to show");
				if (IsLauncherShown())
					return EditorCommandResult::Fail("No project is open: the editor shows its launcher instead of the panels. Open or create a project first.");
				m_Panels.Focus(panel);
				nlohmann::json result = { { "panel", panel } };
				if (scripts)
				{
					if (ContentBrowserPanel* browser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
						browser->ShowScripts(context, *scripts);
					result["scripts"] = *scripts;
				}
				if (search)
				{
					if (SceneHierarchyPanel* hierarchy = m_Panels.Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy))
						hierarchy->SetSearch(*search);
					result["search"] = *search;
				}
				return EditorCommandResult::Ok(std::move(result));
			}, EditorCommandKind::Session });

		m_Commands.Register({ "editor.showDialog",
			"Opens a dialog of the editor's window for the person at it, e.g. to show what you are about to do or for a screenshot: export (the "
			"Export Game dialog; needs a project), newProject (New Project, with template preselected), openSample (Open Sample: where to copy "
			"the sample, the first one by default; see project.samples), recovery (Recover Unsaved Changes: the project's recovery data that "
			"scene.recoveryStatus lists, for the person to recover or discard; needs a project, and closes by itself when nothing applies), "
			"newScript (New Script: a script class for the project's script directory, as script.create writes; needs a project), "
			"preferences (Preferences: the person's preferences that preferences.get lists, at section's page; works without a project), "
			"externalEditor (Preferences at its External Editor page: the program that opens script files, as script.setExternalEditor sets) or "
			"about (About Strata). The project dialogs ask about unsaved scene changes first. Changes no scene data and records no undo; fails "
			"in an editor without a window (--no-gpu or headless). Returns {dialog} and, for openSample, the sample it offers (sample), for "
			"preferences and externalEditor the page it shows (section).",
			CommandUtils::ObjectSchema({
				{ "dialog", { { "type", "string" }, { "enum", { "export", "newProject", "openSample", "recovery", "newScript", "preferences", "externalEditor",
					"about" } }, { "description", "The dialog to open" } } },
				{ "template", CommandUtils::StringSchema("newProject: the template to preselect (project.templates; default basic3d)") },
				{ "sample", CommandUtils::StringSchema("openSample: the sample to copy (project.samples; default: the first one)") },
				{ "section", CommandUtils::StringSchema("preferences: the preference section to show (preferences.get; default: the page shown last)") } },
				{ "dialog" }),
			[this](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string dialog = arguments.GetString("dialog");
				const std::string templateId = arguments.GetString("template", "");
				std::string sample = arguments.GetString("sample", "");
				std::string section = arguments.GetString("section", "");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (dialog != "export" && dialog != "newProject" && dialog != "openSample" && dialog != "recovery" && dialog != "newScript"
					&& dialog != "preferences" && dialog != "externalEditor" && dialog != "about")
				{
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'dialog': the editor has no dialog '{}' (export, newProject, "
						"openSample, recovery, newScript, preferences, externalEditor, about)", dialog));
				}
				if (!section.empty() && dialog != "preferences")
					return EditorCommandResult::InvalidParameters("Parameter 'section': only the preferences dialog shows a section");
				if (dialog == "externalEditor")
					section = ExternalEditor::c_SettingsSection;
				if (!section.empty())
				{
					const SettingsSection* found = context.GetSettingsRegistry().Find(section);
					if (!found || found->Scope != SettingsScope::User)
					{
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'section': '{}' is no preference section (preferences.get "
							"lists them)", section));
					}
				}
				if (!templateId.empty() && !ProjectTemplates::Find(templateId))
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'template': unknown template '{}' (templates: {})", templateId, ProjectTemplates::ListIds()));
				if (m_Options.Headless)
					return EditorCommandResult::Fail("This editor runs without a window: it has no dialogs to show");

				if (dialog == "export")
				{
					if (!context.HasProject())
						return EditorCommandResult::Fail("No project is open: there is nothing to export (project.open or project.create first)");
					ShowExportDialog();
				}
				else if (dialog == "newProject")
				{
					ShowNewProjectDialog(templateId);
				}
				else if (dialog == "openSample")
				{
					std::string error;
					const std::optional<std::vector<ProjectSample>> samples = ProjectSamples::List(context.GetSamplesDirectory(), &error);
					if (!samples || samples->empty())
						return EditorCommandResult::Fail(samples ? std::string("No samples were installed next to the editor") : error);
					if (sample.empty())
						sample = samples->front().Id;
					else if (std::none_of(samples->begin(), samples->end(), [&sample](const ProjectSample& offered) { return offered.Id == sample; }))
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'sample': no sample '{}' (see project.samples)", sample));
					ShowOpenSampleDialog(sample);
				}
				else if (dialog == "recovery")
				{
					if (!context.HasProject())
						return EditorCommandResult::Fail("No project is open: recovery data belongs to a project (project.open first)");
					// Read again, for what another editor left since: the dialog waits for the scan.
					context.GetRecovery().Scan();
					ShowRecoveryDialog();
				}
				else if (dialog == "newScript")
				{
					if (!context.HasProject())
						return EditorCommandResult::Fail("No project is open: scripts belong to a project (project.open or project.create first)");
					ShowNewScriptDialog();
				}
				else if (dialog == "preferences" || dialog == "externalEditor")
				{
					ShowPreferencesDialog(section);
				}
				else
				{
					ShowAboutDialog();
				}
				nlohmann::json result = { { "dialog", dialog } };
				if (dialog == "openSample")
					result["sample"] = sample;
				if (dialog == "preferences" || dialog == "externalEditor")
					result["section"] = m_PreferencesDialog.GetSelected().empty() ? nlohmann::json(nullptr) : nlohmann::json(m_PreferencesDialog.GetSelected());
				return EditorCommandResult::Ok(std::move(result));
			}, EditorCommandKind::Session });
	}

	void EditorLayer::RegisterBuiltinPanels()
	{
		auto add = [this](const char* id, const char* title, const char* icon, std::function<Scope<EditorPanel>()> create,
			EditorPanelPlacement placement = EditorPanelPlacement::Docked, bool openByDefault = true)
		{
			EditorPanelDescriptor descriptor;
			descriptor.Id = id;
			descriptor.Title = title;
			descriptor.Icon = icon;
			descriptor.Placement = placement;
			descriptor.OpenByDefault = openByDefault;
			descriptor.Create = std::move(create);
			std::string error;
			if (!m_Panels.Register(std::move(descriptor), &error))
				ST_ERROR("Could not register the panel '{}': {}", id, error);
		};
		// Drawn in this order; the Content Browser comes after the Console, which shares its dock node.
		add(EditorPanels::c_Viewport, "Scene", Icons::Box, []() { return CreateScope<ViewportPanel>(); });
		add(EditorPanels::c_Game, "Game", Icons::Gamepad2, []() { return CreateScope<GameViewPanel>(); });
		add(EditorPanels::c_Hierarchy, "Hierarchy", Icons::ListTree, []() { return CreateScope<SceneHierarchyPanel>(); });
		add(EditorPanels::c_Inspector, "Inspector", Icons::SlidersHorizontal, []() { return CreateScope<InspectorPanel>(); });
		add(EditorPanels::c_AssetInspector, "Asset", Icons::FileBox, []() { return CreateScope<AssetInspectorPanel>(); });
		add(EditorPanels::c_Console, "Console", Icons::Terminal, []() { return CreateScope<ConsolePanel>(); });
		add(EditorPanels::c_Problems, "Problems", Icons::CircleAlert, []() { return CreateScope<ProblemsPanel>(); });
		add(EditorPanels::c_ContentBrowser, "Content Browser", Icons::FolderOpen, []() { return CreateScope<ContentBrowserPanel>(); });
		add(EditorPanels::c_Welcome, "Welcome", Icons::House, []() { return CreateScope<WelcomePanel>(); }, EditorPanelPlacement::Launcher);
		// Instruments, closed until the status bar's pills (or the View menu) open them.
		add(EditorPanels::c_Streaming, "Streaming", Icons::HardDrive, []() { return CreateScope<StreamingPanel>(); }, EditorPanelPlacement::Docked, false);
		add(EditorPanels::c_Performance, "Performance", Icons::ChartColumnStacked, []() { return CreateScope<PerformancePanel>(); }, EditorPanelPlacement::Docked,
			false);
		add(EditorPanels::c_Activity, "Activity", Icons::History, []() { return CreateScope<ActivityPanel>(); }, EditorPanelPlacement::Docked, false);
		add(EditorPanels::c_ProjectSettings, "Project Settings", Icons::Settings2, [this]() { return CreateScope<ProjectSettingsPanel>(m_SettingsPages); },
			EditorPanelPlacement::Docked, false);

		if (ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
		{
			contentBrowser->SetOpenSceneHandler([this](AssetHandle scene)
			{
				RequestDiscardChanges([this, scene]()
				{
					RunEditorCommandDeferred(m_CommandRunner, m_Context, m_Commands, "scene.open", { { "scene", UUIDToJson(scene) }, { "force", true } });
				});
			});
		}
		if (StreamingPanel* streaming = m_Panels.Get<StreamingPanel>(EditorPanels::c_Streaming))
		{
			streaming->SetRevealAssetHandler([this](AssetHandle asset)
			{
				ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
				if (contentBrowser && contentBrowser->RevealAsset(m_Context, asset))
					m_Panels.Focus(EditorPanels::c_ContentBrowser);
			});
		}
		if (SceneHierarchyPanel* hierarchy = m_Panels.Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy))
		{
			hierarchy->SetShowCellMapHandler([this]()
			{
				if (StreamingPanel* streaming = m_Panels.Get<StreamingPanel>(EditorPanels::c_Streaming))
				{
					streaming->RevealWorld();
					m_Panels.Focus(EditorPanels::c_Streaming);
				}
			});
		}
	}

	void EditorLayer::OnAttach()
	{
		if (!m_Options.Headless && m_Host->HasWindow())
		{
			const bool folderPicker = FileDialogs::Init();
			m_ProjectDialogs.SetFolderPickerAvailable(folderPicker);
			m_ExportDialog.SetFolderPickerAvailable(folderPicker);
		}
		if (ImGui::GetCurrentContext())
		{
			m_Panels.InstallSettingsHandler();
			m_ProjectDialogs.InstallSettingsHandler();
		}
		// The automation client agents connect through (the launcher shows how), when it was built next to the editor.
		if (const std::filesystem::path cli = Platform::GetExecutableDirectory() / GetExecutableFileName("StrataCLI"); FileSystem::IsRegularFile(cli))
			m_CLIExecutable = cli;
		// The selection outline carries the accent, like selected rows; the streaming view colors loading and failed assets as
		// the Streaming panel does (GetAssetStateColor), and the LOD and Mip views tell the levels of detail and the mip levels
		// textures lack apart from success to error.
		ViewportSettings& viewportSettings = m_Context.GetViewport().GetSettings();
		const UI::ThemeColors& themeColors = UI::GetThemeColors();
		viewportSettings.SelectionColor = ToVec4(themeColors.Accent);
		viewportSettings.DebugColors.Pending = ToVec4(UI::WithAlpha(GetAssetStateColor(AssetState::Loading), c_DebugTintAlpha));
		viewportSettings.DebugColors.Failed = ToVec4(UI::WithAlpha(GetAssetStateColor(AssetState::Failed), c_DebugTintAlpha));
		const ImVec4* lodColors[] = { &themeColors.Success, &themeColors.Info, &themeColors.Warning, &themeColors.Error };
		for (size_t level = 0; level < viewportSettings.DebugColors.LOD.size(); level++)
			viewportSettings.DebugColors.LOD[level] = ToVec4(UI::WithAlpha(*lodColors[level], c_DebugTintAlpha));
		for (size_t level = 0; level < viewportSettings.DebugColors.Mip.size(); level++)
			viewportSettings.DebugColors.Mip[level] = ToVec4(UI::WithAlpha(*lodColors[level], c_DebugTintAlpha));
		// The cell bounds in the cell map's colors: loading Sulfur, loaded Ochre, unloading the secondary text.
		viewportSettings.DebugColors.CellLoading = ToVec4(themeColors.Warning);
		viewportSettings.DebugColors.CellLoaded = ToVec4(themeColors.Accent);
		viewportSettings.DebugColors.CellUnloading = ToVec4(themeColors.TextSecondary);
		// Startup counts as activity: the layout settles at the full rate.
		m_LastInputTime = m_Host->GetTime();

		if (!m_Options.ProjectPath.empty())
		{
			// A window shows the start scene's load (the viewport's load card) while frames go on; otherwise the editor starts
			// with the scene open.
			std::string error;
			const bool opened = m_Options.OpenStartSceneInBackground ? m_Context.BeginOpenProject(m_Options.ProjectPath, &error)
				: m_Context.OpenProject(m_Options.ProjectPath, &error);
			if (!opened)
				ReportError(fmt::format("Could not open the project '{}': {}", FileSystem::ToUTF8(m_Options.ProjectPath), error));
		}

		m_Context.SetStatusProvider(c_EditorStatusSection, [this]()
		{
			nlohmann::json window = nullptr;
			if (m_Host->HasWindow())
			{
				const glm::uvec2 size = m_Host->GetWindowSize();
				window = { { "width", size.x }, { "height", size.y }, { "focused", m_Host->IsWindowFocused() } };
			}
			return nlohmann::json {
				{ "window", window },
				{ "headless", m_Options.Headless },
				{ "graphicsDevice", m_Host->HasGraphicsDevice() },
				{ "frame", m_Host->GetFrameCount() },
				{ "maxFrames", m_Options.MaxFrames ? nlohmann::json(*m_Options.MaxFrames) : nlohmann::json(nullptr) },
				{ "uiScale", m_Host->GetUIScale() },
				{ "launcher", IsLauncherShown() },
				{ "startupSeconds", m_StartupSeconds ? nlohmann::json(*m_StartupSeconds) : nlohmann::json(nullptr) },
				{ "gpu", DescribeGraphicsDevice(m_Host->GetGraphicsDeviceInfo()) },
				{ "frameRate", {
					{ "average", GetAverageFrameRate() },
					{ "frameMilliseconds", m_FrameStats.WorkMilliseconds },
					{ "cap", m_Host->GetMaxFrameRate() },
					{ "idle", m_Idle },
					{ "throttling", IsThrottlingEnabled() } } } };
		}, [this]() { return nlohmann::json { { "launcher", IsLauncherShown() } }; });
		if (m_Options.EnableAutomation)
			StartAutomation();
		else if (m_Options.Headless && !m_Options.MaxFrames && !m_Options.QuitAfterCommands)
			ST_WARN("Running headless without automation or --frames: the editor runs until it is stopped with a signal (Ctrl+C)");

		if (!m_Options.CommandScript.empty())
		{
			std::string error;
			m_CommandScript = EditorCommandScript::Load(m_Options.CommandScript, &error);
			if (m_CommandScript)
			{
				// Commands that finish at once run before the first frame.
				UpdateCommandScript();
			}
			else
			{
				ST_ERROR("Command script: {}", error);
				m_Host->SetExitCode(1);
				if (m_Options.QuitAfterCommands)
					m_Host->Close();
			}
		}
	}

	void EditorLayer::StartAutomation()
	{
		EditorAutomationSpecification specification;
		specification.Port = m_Options.AutomationPort;
		specification.Headless = m_Options.Headless;
		specification.IdleTimeout = m_Options.IdleTimeout;
		std::string error;
		if (m_Automation.Start(specification, &error))
			return;

		ST_ERROR("Automation is unavailable: {}", error);
		m_AutomationError = std::move(error);
		// Nothing could reach a headless editor that runs until it is told to quit.
		if (m_Options.Headless && !m_Options.MaxFrames)
		{
			ST_ERROR("A headless editor without --frames needs automation; exiting");
			m_Host->SetExitCode(1);
			m_Host->Close();
		}
	}

	void EditorLayer::UpdateCommandScript()
	{
		if (!m_CommandScript || !m_CommandScript->Update(m_CommandRunner, m_Context, m_Commands))
			return;
		// A failed script fails the process, so scripted runs (CI, automation) notice.
		if (m_CommandScript->HasFailed())
		{
			ST_ERROR("Command script finished with errors");
			m_Host->SetExitCode(1);
		}
		else
		{
			ST_INFO("Command script finished ({} commands)", m_CommandScript->GetStepCount());
		}
		m_CommandScript.reset();
		if (!m_Options.QuitAfterCommands)
			return;
		// The screenshot shows the end of the script: this frame, drawn and on screen before the editor closes (also when
		// the script finished before the first frame).
		if (!m_Options.ScreenshotPath.empty())
			TakeScreenshot(true);
		else
			m_Host->Close();
	}

	void EditorLayer::TakeScreenshot(bool closeAfter)
	{
		if (m_ScreenshotRequested)
			return;
		m_ScreenshotRequested = true;
		m_Host->RequestScreenshot([this, path = m_Options.ScreenshotPath, closeAfter](const ReadbackImage& image)
		{
			std::string error;
			if (ImageWriter::SavePNG(image, path, true, &error))
			{
				ST_INFO("Saved screenshot to {}", FileSystem::ToUTF8(path));
			}
			else
			{
				ST_ERROR("Screenshot failed: {}", error);
				m_Host->SetExitCode(1);
			}
			if (closeAfter)
				m_Host->Close();
		});
	}

	void EditorLayer::OnDetach()
	{
		// Before the project closes: completions may still look at the editor state. Automation stops afterwards, so the
		// clients of cancelled commands still get their answers.
		m_CommandRunner.CancelAll("The editor is closing");
		if (m_UIDrawn)
		{
			EditorPanelContext panelContext = MakePanelContext();
			m_Panels.OnDetach(panelContext);
		}
		m_Panels.RemoveSettingsHandler();
		m_ProjectDialogs.RemoveSettingsHandler();
		m_Automation.Stop();
		m_Context.SetStatusProvider(c_EditorStatusSection, nullptr);
		m_Context.CloseProject();
		FileDialogs::Shutdown();
	}

	void EditorLayer::OnUpdate(Timestep timestep)
	{
		// The previous frame has ended: its time is known (editor.wait reports the frames it waited for).
		if (m_Host->GetFrameCount() > 0)
			m_Context.RecordFrameTime(m_Host->GetLastFrameWorkTime() * 1000.0);

		// The first frame is on screen once the second one starts: that is when the editor has started.
		if (!m_StartupMeasured && m_Host->GetFrameCount() >= 1)
		{
			m_StartupMeasured = true;
			m_StartupSeconds = m_Host->GetProcessUptime();
			if (m_StartupSeconds)
				ST_INFO("Started in {:.2f} s", *m_StartupSeconds);
		}
		// The game's content scale in play mode is the editor's (UI scaled to a physical size matches the editor's UI).
		m_Context.GetGameWindow().SetContentScale(m_Host->GetUIScale());
		m_Context.Update(timestep);
		// Between frames: the system's fonts for Chinese, Japanese and Korean text join the editor's once read.
		if (!m_Options.Headless)
			UI::EditorFonts::UpdateFallback();
		// The launcher returns when the next project closes.
		if (m_Context.HasProject())
			m_LauncherDismissed = false;
		m_CommandRunner.Update(m_Context);
		m_Automation.Update();
		UpdateCommandScript();
		UpdateWindowTitle();
		UpdateSteppingFrameRate();

		const double now = m_Host->GetTime();
		// An automation request was answered or is pending: an agent drives the editor.
		const uint64_t answered = m_Automation.GetCompletedRequestCount();
		if (answered != m_LastAutomationRequests || m_Automation.GetPendingRequestCount() > 0)
		{
			m_LastAutomationRequests = answered;
			m_LastAutomationTime = now;
		}
		m_FrameTimes.push_back(now);
		while (!m_FrameTimes.empty() && now - m_FrameTimes.front() > c_FrameRateWindowSeconds)
			m_FrameTimes.pop_front();
		UpdateFrameStats();
		if (!m_Options.Headless)
		{
			EditorPanelContext panelContext = MakePanelContext();
			m_Panels.OnUpdate(panelContext);
		}

		// editor.quit answered already (it checked for unsaved changes); this frame is the last one.
		if (m_Context.IsQuitRequested() && m_Host->IsRunning())
		{
			ST_INFO("Closing the editor (editor.quit)");
			// The rest of the command script will not run: a scripted run (CI, automation) must not look successful.
			if (m_CommandScript)
			{
				ST_ERROR("The command script did not finish before editor.quit ({} of {} commands done)", m_CommandScript->GetCompletedCount(),
					m_CommandScript->GetStepCount());
				m_Host->SetExitCode(1);
			}
			m_Host->Close();
		}
		// Started for a tool that has gone away (--idle-timeout): nobody is left to quit it.
		if (m_Automation.HasIdledOut() && m_Host->IsRunning())
		{
			const char* discarding = !m_Context.IsSceneModified() ? ""
				: m_Context.GetRecovery().IsActive() ? " and discarding unsaved scene changes (kept as recovery data of the project)"
				: " and discarding unsaved scene changes";
			ST_WARN("No automation client for {} s (--idle-timeout): closing the editor{}", m_Options.IdleTimeout.count(), discarding);
			m_Host->Close();
		}
		const bool lastFrame = m_Options.MaxFrames && m_Host->GetFrameCount() + 1 == *m_Options.MaxFrames;
		if (lastFrame && m_CommandScript)
		{
			ST_ERROR("The command script did not finish within {} frames ({} of {} commands done)", *m_Options.MaxFrames,
				m_CommandScript->GetCompletedCount(), m_CommandScript->GetStepCount());
			m_Host->SetExitCode(1);
		}
		if (lastFrame && !m_Options.ScreenshotPath.empty())
			TakeScreenshot(false);
	}

	void EditorLayer::UpdateWindowTitle()
	{
		if (!m_Host->HasWindow())
			return;
		// The launcher has no scene to name.
		std::string title = "Strata Editor";
		if (!IsLauncherShown())
		{
			if (m_Context.HasProject())
				title += " - " + m_Context.GetProject()->GetConfig().Name;
			title += " - " + m_Context.GetEditScene()->GetName();
			if (m_Context.IsSceneModified())
				title += " *";
			if (m_Context.IsPlaying())
				title += fmt::format(" [{}]", SceneStateToString(m_Context.GetSceneState()));
		}
		if (title != m_WindowTitle)
		{
			m_Host->SetWindowTitle(title);
			m_WindowTitle = std::move(title);
		}
	}

	void EditorLayer::OnImGuiRender()
	{
		m_UIDrawn = true;
		TrackInput();
		// Before any shortcut reads the keyboard: a binding capture the Project Settings window runs keeps it.
		BindingCapture::BeginFrame();
		EditorPanelContext panelContext = MakePanelContext();
		if (IsLauncherShown())
		{
			DrawLauncher(panelContext);
		}
		else
		{
			HandleShortcuts();
			DrawDockspace();
			DockNewToolPanels();
			// A new inspection target brings its tab forward (the Asset tab for an asset, the Inspector for the entities),
			// leaving the keyboard focus where it is (e.g. in the Content Browser that was clicked).
			if (m_Context.GetInspectionVersion() != m_ShownInspectionVersion)
			{
				m_ShownInspectionVersion = m_Context.GetInspectionVersion();
				m_Panels.ShowTab(m_Context.GetInspectionTarget() == InspectionTarget::Asset ? EditorPanels::c_AssetInspector : EditorPanels::c_Inspector);
			}
			// A play whose script build failed shows its errors.
			if (m_Context.GetProblemsPanelRequests() != m_ShownProblemsRequests)
			{
				m_ShownProblemsRequests = m_Context.GetProblemsPanelRequests();
				m_Panels.Focus(EditorPanels::c_Problems);
			}
			m_Panels.OnImGuiRender(panelContext);
		}
		m_ProjectDialogs.Draw(panelContext);
		m_AboutDialog.Draw(GetEnvironment());
		m_ExportDialog.Draw(panelContext);
		m_NewScriptDialog.Draw(panelContext);
		m_PreferencesDialog.Draw(panelContext, m_SettingsPages);
		m_RecoveryDialog.Draw(panelContext);
		// Closed with changes still on offer ("Later"): a toast says where they are.
		const bool recoveryOpen = m_RecoveryDialog.IsOpen();
		if (m_RecoveryDialogWasOpen && !recoveryOpen && RecoveryDialog::HasOffer(m_Context))
		{
			EditorNotification notification;
			notification.Title = "Unsaved changes can still be recovered";
			notification.Message = "File > Recover Unsaved Changes, until they are recovered or discarded";
			notification.ShowRecovery = true;
			m_Context.GetNotifications().Post(std::move(notification));
		}
		m_RecoveryDialogWasOpen = recoveryOpen;
		DrawUnsavedChangesModal();
		// Toasts last, over everything: at the bottom right of the Scene or Game view (their dock node), where they cover
		// neither the Inspector nor the Hierarchy; above the status bar in the window's corner without either view.
		const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
		const float statusBar = IsLauncherShown() ? 0.0f : m_StatusBarHeight;
		ImVec2 toastsMin = mainViewport->WorkPos;
		ImVec2 toastsMax(mainViewport->WorkPos.x + mainViewport->WorkSize.x, mainViewport->WorkPos.y + mainViewport->WorkSize.y - statusBar);
		std::optional<EditorPanelRegistry::WindowRect> view = m_Panels.GetShownWindowRect(EditorPanels::c_Viewport);
		if (!view)
			view = m_Panels.GetShownWindowRect(EditorPanels::c_Game);
		if (view)
		{
			toastsMin = view->Min;
			toastsMax = view->Max;
		}
		UI::DrawNotifications(m_Context.GetNotifications(), *this, EditorPanels::c_Console, EditorPanels::c_Problems, toastsMin, toastsMax);
		if (m_ShowImGuiDemo)
			ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
		UpdateFrameRate();
	}

	void EditorLayer::OnEvent(Event& event)
	{
		// Whatever reaches the editor (window changes, input ImGui did not take) is activity.
		m_LastInputTime = m_Host->GetTime();

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowCloseEvent>([this](WindowCloseEvent&)
		{
			if (!m_Context.IsSceneModified() && !m_Context.HasSavesInFlight() && !m_Context.HasUnwrittenProjectSettings()
				&& !m_Context.HasUnwrittenPreferences())
			{
				return false;
			}
			// Keep running and ask first (after the saving dialog while saves write); closing happens from the dialog.
			RequestQuit([this]() { m_Host->Close(); });
			return true;
		});
		dispatcher.Dispatch<WindowFileDropEvent>([this](WindowFileDropEvent& drop)
		{
			// A project file dropped on the editor opens that project (on the launcher too).
			const std::vector<std::filesystem::path>& paths = drop.GetPaths();
			const auto project = std::find_if(paths.begin(), paths.end(), [](const std::filesystem::path& path)
			{
				return FileSystem::ToUTF8(path.extension()) == Project::c_FileExtension && FileSystem::IsRegularFile(path);
			});
			if (project != paths.end())
			{
				RequestReleaseProject([this, file = *project]() { OpenProjectFileNow(file); });
				return true;
			}
			if (!m_Context.HasProject())
			{
				ReportError("Open or create a project before importing files (a dropped .stproj file opens its project)");
				return true;
			}
			if (ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
				contentBrowser->ImportFiles(m_Context, m_Commands, m_CommandRunner, drop.GetPaths());
			return true;
		});
	}

	////////////////////////////////////////////////////////////////////////////////
	// Idle throttling
	////////////////////////////////////////////////////////////////////////////////

	bool EditorLayer::IsThrottlingEnabled() const
	{
		// Scripted and headless runs measure in frames: slowing them down would only make them take longer.
		return !m_Options.Headless && !m_Options.MaxFrames && m_Options.CommandScript.empty();
	}

	void EditorLayer::TrackInput()
	{
		// ImGui keeps the input events it processed this frame (mouse moves, buttons, wheel, keys, text, focus changes);
		// held buttons and keys count too, e.g. a camera fly with the mouse at rest.
		const ImGuiContext& imgui = *ImGui::GetCurrentContext();
		bool active = imgui.InputEventsTrail.Size > 0 || ImGui::IsAnyMouseDown();
		for (int key = ImGuiKey_NamedKey_BEGIN; !active && key < ImGuiKey_NamedKey_END; key++)
			active = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
		if (active)
			m_LastInputTime = m_Host->GetTime();
	}

	bool EditorLayer::IsBusy(double now) const
	{
		if (now - m_LastInputTime < c_InputActivitySeconds || now - m_LastAutomationTime < c_AutomationActivitySeconds)
			return true;
		if (m_Panels.IsAnyAnimating())
			return true;
		if (m_Context.IsPlaying())
		{
			if (!m_Context.IsPaused() || m_Context.GetActiveScene()->GetStepFrames() > 0)
				return true;
		}
		if (const EditorAssetManager* assets = m_Context.GetAssetManager(); assets && assets->GetStats().LoadingAssets > 0)
			return true;
		return m_CommandRunner.GetPendingCount() > 0 || m_Automation.GetPendingRequestCount() > 0 || m_Context.GetScriptBuilder().IsRunning()
			|| m_Context.GetViewport().IsPickPending();
	}

	void EditorLayer::UpdateSteppingFrameRate()
	{
		const bool stepping = m_Context.HasPendingSteps();
		if (stepping && !m_CapBeforeSteps)
		{
			m_CapBeforeSteps = m_Host->GetMaxFrameRate();
			m_Host->SetMaxFrameRate(0);
		}
		else if (!stepping && m_CapBeforeSteps)
		{
			m_Host->SetMaxFrameRate(*m_CapBeforeSteps);
			m_CapBeforeSteps.reset();
		}
	}

	void EditorLayer::UpdateFrameRate()
	{
		if (!IsThrottlingEnabled())
		{
			m_Idle = false;
			return;
		}
		m_Idle = !IsBusy(m_Host->GetTime());
		if (m_Idle)
			m_Host->SetMaxFrameRate(m_Host->IsWindowFocused() ? c_IdleFrameRate : c_UnfocusedIdleFrameRate);
		else
			m_Host->SetMaxFrameRate(0);
	}

	double EditorLayer::GetAverageFrameRate() const
	{
		if (m_FrameTimes.size() < 2)
			return 0.0;
		const double span = m_FrameTimes.back() - m_FrameTimes.front();
		return span > 0.0 ? static_cast<double>(m_FrameTimes.size() - 1) / span : 0.0;
	}

	void EditorLayer::UpdateFrameStats()
	{
		// The host measures a frame once it ran: this is the previous frame's work time (none before the first frame).
		if (const double work = m_Host->GetLastFrameWorkTime(); work > 0.0)
		{
			m_FrameWorkTimes[m_NextFrameWork] = work;
			m_NextFrameWork = (m_NextFrameWork + 1) % c_FrameWorkSamples;
			m_FrameWorkCount = std::min(m_FrameWorkCount + 1, c_FrameWorkSamples);
		}
		double total = 0.0;
		for (size_t index = 0; index < m_FrameWorkCount; index++)
			total += m_FrameWorkTimes[index];
		// The frame rate shows the throttling; the work time what a frame costs (a capped frame rate says nothing about it).
		m_FrameStats.WorkMilliseconds = m_FrameWorkCount > 0 ? static_cast<float>(total / static_cast<double>(m_FrameWorkCount) * 1000.0) : 0.0f;
		m_FrameStats.FramesPerSecond = ImGui::GetCurrentContext() ? ImGui::GetIO().Framerate : static_cast<float>(GetAverageFrameRate());
		m_FrameStats.Idle = m_Idle;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Actions
	////////////////////////////////////////////////////////////////////////////////

	void EditorLayer::HandleShortcuts()
	{
		// Nothing changes behind a modal dialog (global routes ignore modality). The shortcuts that act on the selection
		// (Delete, Ctrl+D, F2, Ctrl+C/X/V, Ctrl+A) belong to the Hierarchy and the Viewport, which handle them while they have
		// the keyboard focus (EditorActions::HandleSelectionShortcuts): a key pressed in another panel never edits the scene.
		if (ImGui::GetTopMostPopupModal())
			return;
		const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
		// Play mode toggles always: it is also the way out of a game that has the input.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global))
		{
			if (m_Context.IsPlaying())
				StopPlay();
			else
				StartPlay();
		}
		// Keys the running game receives (e.g. Delete, or Ctrl+D with Ctrl to crouch) must not edit the scene.
		if (!m_Context.AcceptsEditShortcuts())
			return;
		// Undo and redo go through their commands, so the activity journal shows them and replays repeat them.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global) && m_Context.GetUndoStack().CanUndo() && !m_Context.IsPlaying())
			RunEditorCommand(m_Context, m_Commands, "edit.undo");
		if ((ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global))
			&& m_Context.GetUndoStack().CanRedo() && !m_Context.IsPlaying())
			RunEditorCommand(m_Context, m_Commands, "edit.redo");
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global))
			SaveScene();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global))
			SaveSceneAs();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global))
			ShowOpenSceneDialog();
		// Like the menu item and the toolbar button, the shortcut does nothing while a build runs.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global) && !m_Context.GetScriptBuilder().IsRunning())
			BuildScripts();
	}

	void EditorLayer::RequestDiscardChanges(std::function<void()> action)
	{
		// A save in flight is never lost: the action waits for it in the saving dialog, then asks about what is still unsaved.
		if (m_Context.HasSavesInFlight())
		{
			m_ActionAfterSaves = std::move(action);
			m_OpenSavingModal = true;
			return;
		}
		if (m_Context.IsSceneModified())
		{
			m_PendingDiscardAction = std::move(action);
			m_DiscardQuestionVersion = m_Context.GetUndoStack().GetVersion();
			m_SceneChangedSinceQuestion = false;
			m_OpenUnsavedChangesModal = true;
			return;
		}
		action();
	}

	void EditorLayer::RequestReleaseProject(std::function<void()> action)
	{
		RequestDiscardChanges([this, action = std::move(action)]()
		{
			if (!m_Context.HasUnwrittenProjectSettings())
			{
				action();
				return;
			}
			m_PendingReleaseAction = action;
			m_ReleaseQuestionVersion = m_Context.GetUndoStack().GetVersion();
			m_OpenUnwrittenSettingsModal = true;
		});
	}

	void EditorLayer::RequestQuit(std::function<void()> action)
	{
		RequestReleaseProject([this, action = std::move(action)]()
		{
			if (!m_Context.HasUnwrittenPreferences())
			{
				action();
				return;
			}
			m_PendingQuitAction = action;
			m_QuitQuestionVersion = m_Context.GetUndoStack().GetVersion();
			m_OpenUnwrittenPreferencesModal = true;
		});
	}

	void EditorLayer::RunPendingReleaseAction()
	{
		std::function<void()> action = std::exchange(m_PendingReleaseAction, nullptr);
		if (!action)
			return;
		// The answer is for the settings and for the scene as it was when asked: changes of the scene made while the question
		// waited (an agent's, a script's) are asked about first rather than discarded unseen.
		if (m_Context.IsSceneModified() && m_Context.GetUndoStack().GetVersion() != m_ReleaseQuestionVersion)
			RequestDiscardChanges(std::move(action));
		else
			action();
	}

	void EditorLayer::ReportError(const std::string& message)
	{
		ST_ERROR("{}", message);
		if (IsLauncherShown())
		{
			if (WelcomePanel* welcome = m_Panels.Get<WelcomePanel>(EditorPanels::c_Welcome))
				welcome->ShowError(message);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Shell (what panels ask of the editor window)
	////////////////////////////////////////////////////////////////////////////////

	EditorPanelContext EditorLayer::MakePanelContext()
	{
		return EditorPanelContext { m_Context, m_Commands, m_CommandRunner, m_FrameStats, this };
	}

	bool EditorLayer::IsLauncherShown() const
	{
		return !m_Options.Headless && !m_Context.HasProject() && !m_LauncherDismissed && m_Panels.HasLauncher();
	}

	void EditorLayer::ShowNewProjectDialog(const std::string& templateId)
	{
		// Asked first: creating the project replaces the scene, and the dialog should not stand between the question and it.
		RequestReleaseProject([this, templateId]()
		{
			EditorPanelContext panelContext = MakePanelContext();
			m_ProjectDialogs.OpenNewProject(panelContext, templateId);
		});
	}

	void EditorLayer::ShowOpenSampleDialog(const std::string& sampleId)
	{
		RequestReleaseProject([this, sampleId]()
		{
			EditorPanelContext panelContext = MakePanelContext();
			std::string error;
			if (!m_ProjectDialogs.OpenSample(panelContext, sampleId, &error))
				ReportError(fmt::format("The sample '{}' cannot be opened: {}", sampleId, error));
		});
	}

	void EditorLayer::ShowOpenProjectDialog()
	{
		std::optional<std::filesystem::path> file = FileDialogs::OpenFile({ { "Strata Project", "stproj" } }, m_ProjectDialogs.GetLocation());
		if (!file)
			return;
		OpenProjectFile(*file);
	}

	void EditorLayer::OpenProjectFile(const std::filesystem::path& projectFile)
	{
		RequestReleaseProject([this, projectFile]() { OpenProjectFileNow(projectFile); });
	}

	void EditorLayer::OpenProjectFileNow(const std::filesystem::path& projectFile)
	{
		// Through the runner: the project opens at once, its start scene over the next frames (the viewport shows the load).
		const std::string error = StartEditorCommand(m_CommandRunner, m_Context, m_Commands, "project.open",
			{ { "path", FileSystem::ToUTF8(projectFile) }, { "force", true } });
		if (!error.empty())
			ReportError(fmt::format("Could not open {}: {}", UI::DisplayPath(projectFile), error));
	}

	void EditorLayer::ShowAboutDialog()
	{
		m_AboutDialog.Open();
	}

	void EditorLayer::ShowNewScriptDialog()
	{
		if (m_Context.HasProject())
			m_NewScriptDialog.Open();
	}

	void EditorLayer::DismissLauncher()
	{
		m_LauncherDismissed = true;
	}

	EditorEnvironment EditorLayer::GetEnvironment() const
	{
		EditorEnvironment environment;
		environment.GraphicsDevice = m_Host->GetGraphicsDeviceInfo();
		environment.StartupSeconds = m_StartupSeconds;
		environment.UIScale = m_Host->GetUIScale();
		environment.CLIExecutable = m_CLIExecutable;
		return environment;
	}

	EditorAutomationState EditorLayer::GetAutomationState() const
	{
		EditorAutomationState state;
		state.Enabled = m_Options.EnableAutomation;
		state.Running = m_Automation.IsRunning();
		state.Port = state.Running ? m_Automation.GetPort() : 0;
		state.Clients = state.Running ? m_Automation.GetClientCount() : 0;
		if (state.Enabled && !state.Running)
			state.Error = m_AutomationError.empty() ? std::string("see the Console") : m_AutomationError;
		return state;
	}

	bool EditorLayer::SaveScene(std::function<void(bool saved)> then)
	{
		if (m_Context.IsPlaying())
		{
			ST_WARN("Stop playing before saving: changes made while playing are discarded");
			return false;
		}
		if (!m_Context.GetSceneHandle().IsValid())
			return SaveSceneAs(std::move(then));
		// The save writes in the background: its outcome comes in a later frame (failures as a notification of their own).
		m_CommandRunner.Run(m_Context, m_Commands, "scene.save", nlohmann::json::object(), [then = std::move(then)](const EditorCommandResult& result)
		{
			if (then)
				then(result.Success);
		}, CommandOrigin { CommandSource::UI });
		return true;
	}

	bool EditorLayer::SaveSceneAs(std::function<void(bool saved)> then)
	{
		if (!m_Context.HasProject())
		{
			ST_WARN("Open or create a project to save scenes");
			return false;
		}
		const std::filesystem::path assetDirectory = m_Context.GetProject()->GetAssetDirectory();
		std::optional<std::filesystem::path> path = FileDialogs::SaveFile({ { "Strata Scene", "stscene" } }, m_Context.GetEditScene()->GetName() + ".stscene", assetDirectory);
		if (!path)
			return false;
		const std::filesystem::path relative = FileSystem::GetRelativePath(*path, assetDirectory);
		if (relative.empty())
		{
			ST_ERROR("Scenes must be saved inside the project's asset directory ({})", FileSystem::ToUTF8(assetDirectory));
			return false;
		}
		m_CommandRunner.Run(m_Context, m_Commands, "scene.saveAs", { { "path", FileSystem::ToUTF8(relative) } }, [then = std::move(then)](const EditorCommandResult& result)
		{
			if (then)
				then(result.Success);
		}, CommandOrigin { CommandSource::UI });
		return true;
	}

	bool EditorLayer::ShowOpenSceneDialog()
	{
		if (!m_Context.HasProject() || m_Context.IsPlaying())
			return false;
		const std::optional<std::filesystem::path> file = FileDialogs::OpenFile({ { "Strata Scene", "stscene" } }, m_Context.GetProject()->GetAssetDirectory());
		return file && OpenSceneFile(*file);
	}

	bool EditorLayer::OpenSceneFile(const std::filesystem::path& file)
	{
		if (!m_Context.HasProject())
		{
			ST_WARN("Open or create a project to open its scenes");
			return false;
		}
		if (m_Context.IsPlaying())
		{
			ST_WARN("Stop playing before opening another scene");
			return false;
		}
		const std::filesystem::path assetDirectory = m_Context.GetProject()->GetAssetDirectory();
		const std::filesystem::path relative = FileSystem::GetRelativePath(file, assetDirectory);
		if (relative.empty())
		{
			ST_ERROR("Scenes open from the project's asset directory ({}), not from '{}'", FileSystem::ToUTF8(assetDirectory), FileSystem::ToUTF8(file));
			return false;
		}
		// The person answers the question about unsaved changes first; the scene then loads over the next frames.
		const std::string scene = FileSystem::ToUTF8(relative);
		RequestDiscardChanges([this, scene]()
		{
			RunEditorCommandDeferred(m_CommandRunner, m_Context, m_Commands, "scene.open", { { "scene", scene }, { "force", true } });
		});
		return true;
	}

	void EditorLayer::RenameEntity(UUID entity)
	{
		SceneHierarchyPanel* hierarchy = m_Panels.Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy);
		if (!hierarchy || !m_Context.GetActiveScene()->GetEntityByUUID(entity))
			return;
		m_Panels.Focus(EditorPanels::c_Hierarchy);
		hierarchy->BeginRename(entity);
	}

	void EditorLayer::StartPlay()
	{
		// One play at a time: while the scripts build to play, Play waits with them.
		if (m_Context.GetPlayBuildID() != 0 || m_Context.IsPlaying())
			return;
		m_CommandRunner.Run(m_Context, m_Commands, "play.start", nlohmann::json::object(), [this](const EditorCommandResult& result)
		{
			// A build the person cancelled (the "Building to play" pill) is no error.
			if (result.ErrorKind == EditorCommandError::Cancelled)
				ST_INFO("Play: {}", result.Error);
			else if (!result.Success && result.Announced)
				ST_WARN("Play: {}", result.Error); // The failed build's error, which its pill and toast report already
			else if (!result.Success)
				ST_ERROR("Play: {}", result.Error);
			// The game shows in the Game view: an open one docked behind another tab comes forward (and takes the keyboard, so
			// the game gets its input).
			const GameViewPanel* game = m_Panels.Get<GameViewPanel>(EditorPanels::c_Game);
			if (result.Success && game && m_Panels.IsOpen(EditorPanels::c_Game) && !game->IsShown())
			{
				m_Panels.Focus(EditorPanels::c_Game);
				m_PlayShowedGame = true;
			}
		}, CommandOrigin { CommandSource::UI });
	}

	void EditorLayer::StopPlay()
	{
		RunEditorCommand(m_Context, m_Commands, "play.stop");
		// Back to the Scene view when Play had brought the Game view forward.
		if (m_PlayShowedGame && !m_Context.IsPlaying() && m_Panels.IsOpen(EditorPanels::c_Viewport))
			m_Panels.Focus(EditorPanels::c_Viewport);
		m_PlayShowedGame = false;
	}

	void EditorLayer::ShowPanel(std::string_view panel)
	{
		m_Panels.Focus(panel);
	}

	void EditorLayer::ShowRecoveryDialog()
	{
		m_RecoveryDialog.Open();
	}

	void EditorLayer::ShowActivity(uint64_t entry)
	{
		if (ActivityPanel* activity = m_Panels.Get<ActivityPanel>(EditorPanels::c_Activity))
			activity->Select(entry);
		m_Panels.Focus(EditorPanels::c_Activity);
	}

	void EditorLayer::ShowAsset(AssetHandle asset)
	{
		// Inspecting it brings the Asset tab forward (OnImGuiRender follows the inspection); the Content Browser shows where it
		// lies without taking the front from it.
		if (!m_Context.SetInspectedAsset(asset))
			return;
		if (ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
			contentBrowser->RevealAsset(m_Context, asset);
	}

	void EditorLayer::BuildScripts()
	{
		// The builder logs the outcome of builds that ran; requests refused at once (no project, a build already running)
		// are only reported here.
		const uint64_t previousBuild = m_Context.GetScriptBuilder().GetLastResult().ID;
		m_CommandRunner.Run(m_Context, m_Commands, "script.build", { { "wait", true } }, [this, previousBuild](const EditorCommandResult& result)
		{
			if (!result.Success && m_Context.GetScriptBuilder().GetLastResult().ID == previousBuild)
				ST_ERROR("Build Scripts: {}", result.Error);
		}, CommandOrigin { CommandSource::UI });
	}

	////////////////////////////////////////////////////////////////////////////////
	// Interface
	////////////////////////////////////////////////////////////////////////////////

	void EditorLayer::BuildDefaultLayout(unsigned int dockspaceId)
	{
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetContentRegionAvail());

		// The viewport first: the side panels take a fixed share of the width (more at large UI scales, for their text), the
		// bottom area a share of the height under the viewport only.
		const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
		const auto sideShare = [width](float share, float minimumFontSizes)
		{
			return std::min(std::max(share, minimumFontSizes * ImGui::GetFontSize() / width), c_MaxSideWidth);
		};
		const float hierarchyWidth = sideShare(c_HierarchyWidth, c_MinHierarchyFontSizes);
		const float inspectorWidth = sideShare(c_InspectorWidth, c_MinInspectorFontSizes);
		ImGuiID center = dockspaceId;
		const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, hierarchyWidth, nullptr, &center);
		const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, inspectorWidth / (1.0f - hierarchyWidth), nullptr, &center);
		const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, c_BottomHeight, nullptr, &center);

		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Hierarchy).c_str(), left);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Inspector).c_str(), right);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_AssetInspector).c_str(), right);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Console).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Problems).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_ContentBrowser).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Streaming).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Performance).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Activity).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Viewport).c_str(), center);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Game).c_str(), center);
		// Pages of fields need room: the Project Settings join the views when they open.
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_ProjectSettings).c_str(), center);
		ImGui::DockBuilderFinish(dockspaceId);
		// On first run the Content Browser is the bottom area's visible tab, and the Inspector the right one's (the Asset tab
		// comes forward when an asset is inspected).
		m_Panels.SelectTab(EditorPanels::c_ContentBrowser, bottom);
		m_Panels.SelectTab(EditorPanels::c_Inspector, right);
		// The Scene view in front of the Game view (Play brings the Game view forward).
		m_Panels.SelectTab(EditorPanels::c_Viewport, center);
	}

	void EditorLayer::DrawLauncher(EditorPanelContext& panelContext)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
			| ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::GetThemeColors().Chrome);
		ImGui::Begin("EditorLauncher", nullptr, windowFlags);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
		DrawMenuBar();
		m_Panels.DrawLauncher(panelContext);
		ImGui::End();
	}

	void EditorLayer::DrawDockspace()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const ImGuiStyle& style = ImGui::GetStyle();
		// The status bar uses the caption size.
		const float statusBarHeight = UI::GetTextSize(UI::TextSize::Caption) * style.FontScaleMain * style.FontScaleDpi + style.FramePadding.y * 2.0f;
		m_StatusBarHeight = statusBarHeight;
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
			| ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::GetThemeColors().Chrome);
		ImGui::Begin("EditorDockspace", nullptr, windowFlags);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);

		DrawMenuBar();
		DrawToolbar();
		const ImGuiID dockspaceId = ImGui::GetID("EditorDockspaceID");
		// Without a saved arrangement of the current layout version (first run, or an older editor's) the panels get the
		// default one; checked once, so a user who undocks every panel keeps that choice.
		if (!m_LayoutChecked)
		{
			const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
			const bool empty = !node || (node->IsLeafNode() && node->Windows.Size == 0);
			m_ResetLayout |= empty || m_Panels.GetSavedLayoutVersion() != c_LayoutVersion;
			m_LayoutChecked = true;
		}
		if (m_ResetLayout)
		{
			m_Panels.ResetOpenStates();
			BuildDefaultLayout(dockspaceId);
			m_Panels.SetLayoutVersion(c_LayoutVersion);
			m_ResetLayout = false;
		}
		ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
		ImGui::End();

		// The status bar is a separate strip below the dock space.
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, UI::GetThemeColors().Chrome);
		ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
		// Pills sit closer together than other items.
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.5f, style.ItemSpacing.y));
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		DrawStatusBar();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::End();
	}

	void EditorLayer::DrawMenuBar()
	{
		if (!ImGui::BeginMenuBar())
			return;

		// The launcher offers only what makes sense before a project is open.
		const bool launcher = IsLauncherShown();
		const bool fileMenu = ImGui::BeginMenu("File");
		UI::ItemProbe::Record("Menu.File");
		if (fileMenu)
		{
			if (ImGui::MenuItem("New Project..."))
				ShowNewProjectDialog({});
			if (ImGui::MenuItem("Open Project..."))
				ShowOpenProjectDialog();
			const bool recentMenu = ImGui::BeginMenu("Open Recent");
			UI::ItemProbe::Record("Menu.File.OpenRecent");
			if (recentMenu)
			{
				DrawRecentMenu();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Open Sample"))
			{
				DrawSampleMenu();
				ImGui::EndMenu();
			}
			if (!launcher)
			{
				if (ImGui::MenuItem("Close Project", nullptr, false, m_Context.HasProject()))
					RequestReleaseProject([this]() { RunEditorCommand(m_Context, m_Commands, "project.close", { { "force", true } }); });
				UI::ItemProbe::Record("Menu.File.CloseProject", m_Context.HasProject());
				// Back from Continue without a project (with a project open, Close Project leads there).
				if (ImGui::MenuItem("Show Launcher", nullptr, false, !m_Context.HasProject()))
					m_LauncherDismissed = false;
				UI::ItemProbe::Record("Menu.File.ShowLauncher", !m_Context.HasProject());
				ImGui::Separator();
				if (ImGui::MenuItem("New Scene", nullptr, false, !m_Context.IsPlaying()))
					RequestDiscardChanges([this]() { RunEditorCommand(m_Context, m_Commands, "scene.new", { { "force", true } }); });
				UI::ItemProbe::Record("Menu.File.NewScene", !m_Context.IsPlaying());
				const bool canOpenScene = m_Context.HasProject() && !m_Context.IsPlaying();
				if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, canOpenScene))
					ShowOpenSceneDialog();
				UI::ItemProbe::Record("Menu.File.OpenScene", canOpenScene);
				if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_Context.IsPlaying()))
					SaveScene();
				UI::ItemProbe::Record("Menu.File.SaveScene", !m_Context.IsPlaying());
				if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, m_Context.HasProject() && !m_Context.IsPlaying()))
					SaveSceneAs();
				const bool recoverable = RecoveryDialog::HasOffer(m_Context);
				if (ImGui::MenuItem("Recover Unsaved Changes...", nullptr, false, recoverable))
					m_RecoveryDialog.Open();
				UI::ItemProbe::Record("Menu.File.Recover", recoverable);
				if (ImGui::MenuItem("Set as Start Scene", nullptr, false, m_Context.HasProject() && m_Context.GetSceneHandle().IsValid()))
					RunEditorCommand(m_Context, m_Commands, "project.setStartScene", { { "scene", UUIDToJson(m_Context.GetSceneHandle()) } });
				ImGui::Separator();
				const bool canExport = m_Context.HasProject();
				if (ImGui::MenuItem("Export Game...", nullptr, false, canExport))
					ShowExportDialog();
				UI::ItemProbe::Record("Menu.File.ExportGame", canExport);
				ImGui::Separator();
				if (ImGui::MenuItem("Project Settings...", nullptr, false, m_Context.HasProject()))
					m_Panels.Focus(EditorPanels::c_ProjectSettings);
				UI::ItemProbe::Record("Menu.File.ProjectSettings", m_Context.HasProject());
			}
			// The person's preferences, also from the launcher: they hold for every project.
			if (ImGui::MenuItem("Preferences..."))
				ShowPreferencesDialog({});
			UI::ItemProbe::Record("Menu.File.Preferences");
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				RequestQuit([this]() { m_Host->Close(); });
			UI::ItemProbe::Record("Menu.File.Exit");
			ImGui::EndMenu();
		}

		if (launcher)
		{
			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::MenuItem("About Strata"))
					ShowAboutDialog();
				if (m_Options.ShowImGuiDemo)
					ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
			return;
		}

		if (ImGui::BeginMenu("Edit"))
		{
			const std::string undo = m_Context.GetUndoStack().GetUndoName();
			const std::string redo = m_Context.GetUndoStack().GetRedoName();
			if (ImGui::MenuItem(undo.empty() ? "Undo" : ("Undo " + undo).c_str(), "Ctrl+Z", false, !undo.empty() && !m_Context.IsPlaying()))
				RunEditorCommand(m_Context, m_Commands, "edit.undo");
			if (ImGui::MenuItem(redo.empty() ? "Redo" : ("Redo " + redo).c_str(), "Ctrl+Y", false, !redo.empty() && !m_Context.IsPlaying()))
				RunEditorCommand(m_Context, m_Commands, "edit.redo");
			// The selection's actions: also shortcuts of the Hierarchy and the Viewport while they have the keyboard focus.
			ImGui::Separator();
			const bool selected = m_Context.GetPrimarySelection().IsValid();
			const bool editable = m_Context.AcceptsEditShortcuts();
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, selected && editable))
				EditorActions::CutSelection(m_Context, m_Commands);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, selected))
				EditorActions::CopySelection(m_Context, m_Commands);
			if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable && EditorActions::CanPaste()))
				EditorActions::Paste(m_Context, m_Commands);
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected && editable))
				EditorActions::DuplicateSelection(m_Context, m_Commands);
			if (ImGui::MenuItem("Rename", "F2", false, selected && editable))
				RenameEntity(m_Context.GetPrimarySelection().GetUUID());
			if (ImGui::MenuItem("Delete", "Del", false, selected && editable))
				EditorActions::DeleteSelection(m_Context, m_Commands);
			if (ImGui::MenuItem("Drop to Ground", "End", false, selected && editable))
				EditorActions::DropSelectionToGround(m_Context, m_Commands);
			ImGui::Separator();
			if (ImGui::MenuItem("Select All", "Ctrl+A"))
				EditorActions::SelectAll(m_Context);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("View"))
		{
			m_Panels.DrawMenuItems();
			ImGui::Separator();
			if (ImGui::MenuItem("Reset Layout"))
				m_ResetLayout = true;
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Entity"))
		{
			SceneHierarchyPanel::DrawCreateMenu(m_Context, m_Commands, UUID::Null());
			ImGui::EndMenu();
		}

		const bool scriptsMenu = ImGui::BeginMenu("Scripts");
		UI::ItemProbe::Record("Menu.Scripts");
		if (scriptsMenu)
		{
			const bool building = m_Context.GetScriptBuilder().IsRunning();
			if (ImGui::MenuItem("Build Scripts", "Ctrl+B", false, m_Context.HasProject() && !building))
				BuildScripts();
			const Ref<ScriptEngine>& engine = m_Context.GetScriptEngine();
			if (ImGui::MenuItem("Reload Scripts", nullptr, false, m_Context.HasProject() && !building))
				RunEditorCommand(m_Context, m_Commands, "script.reload");
			if (ImGui::MenuItem("Create Script Build", nullptr, false, m_Context.HasProject()))
				RunEditorCommand(m_Context, m_Commands, "script.init", { { "example", true } });
			ImGui::Separator();
			if (ImGui::MenuItem("New Script...", nullptr, false, m_Context.HasProject()))
				ShowNewScriptDialog();
			if (ImGui::MenuItem("Open Scripts Folder", nullptr, false, m_Context.HasProject()))
				RunEditorCommand(m_Context, m_Commands, "script.open");
			if (ImGui::MenuItem("External Editor..."))
				ShowPreferencesDialog(ExternalEditor::c_SettingsSection);
			UI::ItemProbe::Record("Menu.Scripts.ExternalEditor");
			if (ImGui::MenuItem("Show Problems", nullptr, false, m_Context.HasProject()))
				m_Panels.Focus(EditorPanels::c_Problems);
			ImGui::Separator();
			if (engine && engine->IsModuleLoaded())
				ImGui::TextDisabled("%s: %zu classes, loaded %llu times", engine->GetModuleName().c_str(), engine->GetClasses().size(),
					static_cast<unsigned long long>(engine->GetLoadCount()));
			else
				ImGui::TextDisabled("No script module loaded");
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::MenuItem("About Strata"))
				ShowAboutDialog();
			// ImGui's demo is a reference for UI work, not part of the product: only with --imgui-demo.
			if (m_Options.ShowImGuiDemo)
				ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	void EditorLayer::DrawRecentMenu()
	{
		// The projects the launcher shows: without the open one, and without those its last look found missing (looking at
		// the files here would hold up the frame for a drive that is out of reach).
		const WelcomePanel* welcome = m_Panels.Get<WelcomePanel>(EditorPanels::c_Welcome);
		const Ref<Project>& open = m_Context.GetProject();
		std::filesystem::path chosen;
		size_t shown = 0;
		for (const RecentProject& project : m_Context.GetRecentProjects().GetAllProjects())
		{
			if (open && project.Path.lexically_normal() == open->GetProjectFile().lexically_normal())
				continue;
			if (welcome && welcome->IsKnownMissing(project.Path))
				continue;
			const std::string label = fmt::format("{}###Recent{}", project.Name, shown);
			if (ImGui::MenuItem(label.c_str()))
				chosen = project.Path;
			UI::ItemProbe::Record(fmt::format("Menu.File.Recent.{}", shown));
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", UI::DisplayPath(project.Path).c_str());
			shown++;
		}
		if (shown == 0)
			ImGui::MenuItem("No recent projects", nullptr, false, false);
		if (!chosen.empty())
			OpenProjectFile(chosen);
	}

	void EditorLayer::DrawSampleMenu()
	{
		if (!m_SampleMenuRead)
		{
			m_SampleMenuRead = true;
			const EditorCommandResult result = m_Commands.Execute(m_Context, "project.samples", nlohmann::json::object(), CommandOrigin { CommandSource::UI });
			if (result.Success && result.Value.contains("samples"))
			{
				for (const nlohmann::json& sample : result.Value["samples"])
					m_SampleMenu.emplace_back(sample.value("id", ""), sample.value("name", ""));
			}
		}
		if (m_SampleMenu.empty())
			ImGui::MenuItem("No samples were installed", nullptr, false, false);
		for (const auto& [id, name] : m_SampleMenu)
		{
			if (ImGui::MenuItem((name + "...").c_str()))
				ShowOpenSampleDialog(id);
		}
	}

	void EditorLayer::DrawToolbar()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float bandPadding = style.FramePadding.y;
		const float height = ImGui::GetFrameHeight() + bandPadding * 2.0f;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, bandPadding));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.Chrome);
		ImGui::BeginChild("MainToolbar", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		// Transform tools at the left, play controls in the center, the script build at the right.
		DrawGizmoControls();
		const float buttonSize = ImGui::GetFrameHeight();
		const float playWidth = buttonSize * 5.0f + style.ItemSpacing.x * 4.0f;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max((ImGui::GetWindowWidth() - playWidth) * 0.5f, ImGui::GetCursorPosX() + style.ItemSpacing.x * 2.0f));
		DrawPlayControls();
		DrawBuildControls();
		ImGui::EndChild();

		// A hairline closes the chrome band above the panels.
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y - 1.0f), ImVec2(max.x, max.y - 1.0f), ImGui::GetColorU32(colors.Border));
	}

	void EditorLayer::DrawGizmoControls()
	{
		ViewportSettings& settings = m_Context.GetViewport().GetSettings();
		auto gizmoButton = [&settings](const char* id, const char* icon, GizmoOperation operation, const char* tooltip)
		{
			UI::ButtonStyle style;
			style.Active = settings.Gizmo == operation;
			if (UI::ToolbarButton(id, icon, nullptr, tooltip, style))
				settings.Gizmo = operation;
			ImGui::SameLine();
		};
		gizmoButton("Toolbar.Select", Icons::MousePointer2, GizmoOperation::None, "Select: no transform gizmo (Q)");
		gizmoButton("Toolbar.Move", Icons::Move3d, GizmoOperation::Translate, "Move the selection (W)");
		gizmoButton("Toolbar.Rotate", Icons::Rotate3d, GizmoOperation::Rotate, "Rotate the selection (E)");
		gizmoButton("Toolbar.Scale", Icons::Scale3d, GizmoOperation::Scale, "Scale the selection (R)");

		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
		const bool world = settings.Space == GizmoSpace::World;
		if (UI::ToolbarButton("Toolbar.Space", world ? Icons::Globe : Icons::Box, world ? "World" : "Local",
			"Gizmo axes: the world's or the entity's own (scaling always uses the entity's)"))
		{
			settings.Space = world ? GizmoSpace::Local : GizmoSpace::World;
		}
		ImGui::SameLine();
		UI::ButtonStyle snapStyle;
		snapStyle.Active = settings.Snap;
		if (UI::ToolbarButton("Toolbar.Snap", Icons::Magnet, nullptr, "Snap gizmo drags to steps (holding Ctrl inverts this)", snapStyle))
			settings.Snap = !settings.Snap;
		ImGui::SameLine(0.0f, 0.0f);
		if (UI::ToolbarButton("Toolbar.SnapSteps", Icons::ChevronDown, nullptr, "Snapping steps"))
			ImGui::OpenPopup("SnapSettings");
		if (ImGui::BeginPopup("SnapSettings"))
		{
			const float fieldWidth = ImGui::GetFontSize() * c_SnapFieldWidthInFontSizes;
			ImGui::SetNextItemWidth(fieldWidth);
			ImGui::DragFloat("Move (units)", &settings.TranslateSnap, 0.05f, 0.001f, ViewportSettings::c_MaxTranslateSnap, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(fieldWidth);
			ImGui::DragFloat("Rotate (degrees)", &settings.RotateSnap, 0.5f, 0.1f, ViewportSettings::c_MaxRotateSnap, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(fieldWidth);
			ImGui::DragFloat("Scale (factor)", &settings.ScaleSnap, 0.01f, 0.001f, ViewportSettings::c_MaxScaleSnap, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndPopup();
		}
	}

	void EditorLayer::DrawPlayControls()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const SceneState state = m_Context.GetSceneState();
		const bool playing = m_Context.IsPlaying();
		const bool paused = m_Context.IsPaused();
		// A scene being opened replaces the edited one: play mode waits for it.
		const bool sceneLoading = m_Context.IsSceneLoading();

		// Play is the primary action while editing; while a mode runs, its button shows it in the mode's color.
		UI::ButtonStyle play;
		play.Enabled = !playing && !sceneLoading && m_Context.GetPlayBuildID() == 0;
		play.Primary = !playing;
		play.Active = state == SceneState::Play;
		play.ActiveColor = colors.PlayState.Play;
		const char* playTooltip = "Play: run the scene with scripts, physics and audio (Ctrl+P); scripts that changed are built first";
		if (playing)
			playTooltip = "Playing";
		else if (sceneLoading)
			playTooltip = "A scene is being opened: play once it is open";
		else if (m_Context.GetPlayBuildID() != 0)
			playTooltip = "Building the scripts to play (the status bar's pill cancels)";
		if (UI::ToolbarButton("Toolbar.Play", Icons::Play, nullptr, playTooltip, play))
			StartPlay();
		ImGui::SameLine();

		UI::ButtonStyle simulate;
		simulate.Enabled = !playing && !sceneLoading;
		simulate.Active = state == SceneState::Simulate;
		simulate.ActiveColor = colors.PlayState.Simulate;
		if (UI::ToolbarButton("Toolbar.Simulate", Icons::Atom, nullptr, "Simulate: run the scene's physics only", simulate))
			RunEditorCommand(m_Context, m_Commands, "play.simulate");
		ImGui::SameLine();

		UI::ButtonStyle pause;
		pause.Enabled = playing;
		pause.Active = paused;
		pause.ActiveColor = colors.PlayState.Paused;
		if (UI::ToolbarButton("Toolbar.Pause", Icons::Pause, nullptr, paused ? "Resume" : "Pause", pause))
			RunEditorCommand(m_Context, m_Commands, "play.pause", { { "paused", !paused } });
		ImGui::SameLine();

		UI::ButtonStyle step;
		step.Enabled = paused;
		if (UI::ToolbarButton("Toolbar.Step", Icons::StepForward, nullptr, "Step one fixed update while paused", step))
			// One step over the next frame: the button need not wait for it.
			RunEditorCommand(m_Context, m_Commands, "play.step", { { "wait", false } });
		ImGui::SameLine();

		UI::ButtonStyle stop;
		stop.Enabled = playing;
		if (UI::ToolbarButton("Toolbar.Stop", Icons::Square, nullptr, "Stop and return to the edited scene (Ctrl+P)", stop))
			StopPlay();
	}

	void EditorLayer::DrawBuildControls()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const ScriptBuilder& builder = m_Context.GetScriptBuilder();
		const bool building = builder.IsRunning();
		const bool failed = !building && builder.GetLastResult().ID != 0 && !builder.GetLastResult().Success && !builder.GetLastResult().Cancelled;

		const std::string label = building ? fmt::format("Building {:.0f} s", builder.GetElapsedSeconds()) : std::string("Build Scripts");
		const char* icon = building ? Icons::LoaderCircle : (failed ? Icons::CircleAlert : Icons::Hammer);
		UI::ButtonStyle buildStyle;
		buildStyle.Enabled = m_Context.HasProject() && !building;
		buildStyle.Active = building || failed;
		buildStyle.ActiveColor = building ? colors.Info : colors.Error;
		const char* tooltip = failed ? "The last build failed (see the Console): build the scripts again (Ctrl+B)"
			: "Build the project's scripts and hot-reload them (Ctrl+B)";

		// Export: the game's progress while it is written.
		const Ref<GameExport>& exporter = m_Context.GetGameExport();
		const bool exporting = exporter && !exporter->IsFinished();
		const std::string exportLabel = exporting ? fmt::format("Exporting {:.0f}%", std::floor(exporter->GetProgress() * 100.0f)) : std::string("Export");
		const char* exportIcon = exporting ? Icons::LoaderCircle : Icons::PackageOpen;
		UI::ButtonStyle exportStyle;
		exportStyle.Enabled = m_Context.HasProject();
		exportStyle.Active = exporting;
		exportStyle.ActiveColor = colors.Info;

		// Right-aligned: Export, then Build Scripts.
		auto buttonWidth = [&style](const char* buttonIcon, const std::string& buttonLabel)
		{
			return style.FramePadding.x * 2.0f + ImGui::CalcTextSize(buttonIcon).x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(buttonLabel.c_str()).x;
		};
		const float width = buttonWidth(exportIcon, exportLabel) + style.ItemSpacing.x + buttonWidth(icon, label);
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetWindowWidth() - style.WindowPadding.x - width, ImGui::GetCursorPosX()));
		if (UI::ToolbarButton("Toolbar.Export", exportIcon, exportLabel.c_str(), exporting ? "Show the running export" : "Export the project as a playable game",
			exportStyle))
		{
			ShowExportDialog();
		}
		ImGui::SameLine();
		if (UI::ToolbarButton("Toolbar.BuildScripts", icon, label.c_str(), tooltip, buildStyle))
			BuildScripts();
	}

	void EditorLayer::ShowExportDialog()
	{
		EditorPanelContext panelContext = MakePanelContext();
		m_ExportDialog.Open(panelContext);
	}

	void EditorLayer::ShowPreferencesDialog(const std::string& section)
	{
		m_PreferencesDialog.Open(section);
	}

	void EditorLayer::ShowProjectSettings(const std::string& page)
	{
		if (ProjectSettingsPanel* panel = m_Panels.Get<ProjectSettingsPanel>(EditorPanels::c_ProjectSettings))
			panel->Select(page);
		m_Panels.Focus(EditorPanels::c_ProjectSettings);
	}

	void EditorLayer::DrawStatusBar()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();

		// Play state.
		const SceneState state = m_Context.GetSceneState();
		const bool paused = m_Context.IsPaused();
		const char* stateIcon = paused ? Icons::Pause : (state == SceneState::Play ? Icons::Play : (state == SceneState::Simulate ? Icons::Atom : Icons::PencilRuler));
		const char* stateText = paused ? "Paused" : (state == SceneState::Play ? "Playing" : (state == SceneState::Simulate ? "Simulating" : "Editing"));
		const ImVec4& stateColor = paused ? colors.PlayState.Paused
			: (state == SceneState::Play ? colors.PlayState.Play : (state == SceneState::Simulate ? colors.PlayState.Simulate : colors.PlayState.Edit));

		// Every stream of work has a pill, and in a narrow window (about 1,000 points: 1600 pixels at 150%) their full texts do
		// not all fit side by side: then the pills show compact texts (the details go to their tooltips). Decided from the width
		// the full texts took in the previous frame; they grow back once the full texts fit with a margin.
		const float available = ImGui::GetContentRegionAvail().x;
		if (m_StatusBarFullWidth > available)
			m_StatusBarCompact = true;
		else if (m_StatusBarFullWidth <= available - ImGui::GetFontSize() * c_StatusBarMarginFontSizes)
			m_StatusBarCompact = false;
		m_StatusBarFullWidth = 0.0f;

		UI::Pill("Status.PlayState", stateIcon, StatusPillText(stateIcon, stateText), stateColor,
			"The scene's state: changes made while it runs are discarded when it stops");

		ImGui::SameLine();
		DrawFramePill();
		DrawAssetsPill();
		DrawSavingPill();
		DrawPhysicsPill();

		ImGui::SameLine();
		bool showConsole = DrawAutomationPill();
		ImGui::SameLine();
		showConsole |= DrawScriptsPill();

		// Keys a tool holds stay down after it disconnects: show them, with a way out that does not need the tool.
		if (const SimulatedInput& simulated = m_Context.GetSimulatedInput(); simulated.HasHolds())
		{
			ImGui::SameLine();
			const std::string held = simulated.DescribeHolds();
			const std::string text = "Holds " + held;
			const std::string compact = fmt::format("{} held", simulated.GetHeldCount());
			const bool clicked = UI::Pill("Status.InputHolds", Icons::Keyboard, StatusPillText(Icons::Keyboard, text, compact), colors.Warning);
			if (UI::IsTooltipDue())
			{
				ImGui::SetTooltip("Held: %s.\nKeys, buttons and input actions a tool holds down in the running game (input.* commands). They stay down until "
					"the tool releases them, also after it disconnected, or until play stops. Click to release them all.", held.c_str());
			}
			if (clicked)
				RunEditorCommand(m_Context, m_Commands, "input.releaseAll", { { "wait", false } });
		}

		// Unread errors.
		const ConsolePanel* console = m_Panels.Get<ConsolePanel>(EditorPanels::c_Console);
		if (const uint32_t errors = console ? console->GetUnreadErrors() : 0; errors > 0)
		{
			ImGui::SameLine();
			const std::string text = fmt::format("{} {}", errors, errors == 1 ? "error" : "errors");
			showConsole |= UI::Pill("Status.Errors", Icons::CircleAlert, StatusPillText(Icons::CircleAlert, text), colors.Error,
				"Errors logged since the Console was last viewed: click to open it");
		}
		if (showConsole)
			m_Panels.Focus(EditorPanels::c_Console);
	}

	std::string_view EditorLayer::StatusPillText(const char* icon, std::string_view text, std::string_view compact)
	{
		if (m_StatusBarFullWidth > 0.0f)
			m_StatusBarFullWidth += ImGui::GetStyle().ItemSpacing.x;
		m_StatusBarFullWidth += UI::CalcPillWidth(icon, text);
		return m_StatusBarCompact && !compact.empty() ? compact : text;
	}

	void EditorLayer::DrawFramePill()
	{
		// Frame time: what a frame takes to run, not the interval the idle frame rate cap stretches it to; then the jobs the
		// worker, I/O and background threads have in hand (averaged over the last second, so the count does not flicker).
		const PerformancePanel* performance = m_Panels.Get<PerformancePanel>(EditorPanels::c_Performance);
		const std::string compact = fmt::format("{:.1f} ms{}{:.0f} FPS", m_FrameStats.WorkMilliseconds, c_Separator, m_FrameStats.FramesPerSecond);
		std::string text = compact;
		if (performance)
		{
			const long jobs = std::lround(performance->GetAverageJobsInFlight());
			text += fmt::format("{}{} {}", c_Separator, jobs, jobs == 1 ? "job" : "jobs");
		}
		if (m_FrameStats.Idle)
			text += std::string(c_Separator) + "idle";
		const bool clicked = UI::Pill("Status.Frame", Icons::Gauge, StatusPillText(Icons::Gauge, text, compact), UI::GetThemeColors().TextSecondary);
		// Written only while it shows: the status bar is drawn every frame.
		if (UI::IsTooltipDue())
		{
			std::string tooltip = "Frame time (what a frame takes to run, without waiting for the next one) and frame rate. While nothing happens the "
				"editor redraws less often (idle) and returns to the full rate on input.";
			if (performance)
			{
				const JobSystemStats& stats = performance->GetJobStats();
				tooltip += fmt::format("\nJobs running or queued on the {} worker, {} I/O and {} background threads, averaged over the last {} frames (now {} "
					"running, {} queued). Click to see where the frames go (Performance).", stats.Workers.Threads, stats.IO.Threads, stats.Background.Threads,
					PerformancePanel::c_JobSamples, stats.Workers.RunningJobs + stats.IO.RunningJobs + stats.Background.RunningJobs,
					stats.Workers.QueuedJobs + stats.IO.QueuedJobs + stats.Background.QueuedJobs);
			}
			// What paces the frames, of the last two seconds' frames: summarized only while the tooltip shows.
			tooltip += "\n" + PerformancePanel::DescribeBound(FrameProfiler::Get().Summarize(c_FramePillBoundFrames));
			ImGui::SetTooltip("%s", tooltip.c_str());
			UI::ItemProbe::Record("Status.FrameTooltip", true, 0, tooltip);
		}
		if (clicked)
			m_Panels.Focus(EditorPanels::c_Performance);
	}

	void EditorLayer::DrawPhysicsPill()
	{
		// Only a world at its growth limit leaves bodies without collision (others grow before their next step): rare, and
		// something to fix. The Streaming panel reads the running game's capacity once per frame.
		StreamingPanel* streaming = m_Panels.Get<StreamingPanel>(EditorPanels::c_Streaming);
		if (!streaming || !streaming->GetPhysics() || streaming->GetPhysics()->WaitingForSlot == 0)
			return;
		const PhysicsCapacityInfo& physics = *streaming->GetPhysics();
		const std::string text = fmt::format("{} {} without collision", physics.WaitingForSlot, physics.WaitingForSlot == 1 ? "body" : "bodies");
		const std::string compact = fmt::format("{} no collision", physics.WaitingForSlot);
		ImGui::SameLine();
		const bool clicked = UI::Pill("Status.Physics", Icons::CircleAlert, StatusPillText(Icons::CircleAlert, text, compact), UI::GetThemeColors().Error);
		if (UI::IsTooltipDue())
		{
			ImGui::SetTooltip("%u bodies of the running game have no collision: its physics world is at its growth limit of %u bodies "
				"(PhysicsSettings::GrowthLimit), so they wait until bodies are removed. Click to see the world's capacity (Streaming, Physics).",
				physics.WaitingForSlot, physics.MaxBodies);
		}
		if (clicked)
		{
			streaming->RevealPhysics();
			m_Panels.Focus(EditorPanels::c_Streaming);
		}
	}

	void EditorLayer::DrawAssetsPill()
	{
		// The streaming panel takes the statistics once per frame, also while it is closed; without it, nothing to show.
		const StreamingPanel* streaming = m_Panels.Get<StreamingPanel>(EditorPanels::c_Streaming);
		if (!m_Context.GetAssetManager() || !streaming || !streaming->GetStats())
			return;
		const AssetManagerStats& stats = *streaming->GetStats();
		// The memory of the pool closest to (or furthest over) its budget, which also gives the pill its color; all loaded
		// memory when no pool has a budget (no device).
		const std::optional<AssetMemoryPool> shownPool = GetMostPressuredPool(stats);
		const BudgetPressure pressure = GetWorstPoolPressure(stats);
		std::string memory = FormatMegabytes(stats.LoadedMemory);
		if (shownPool)
		{
			memory = fmt::format("{} {:.0f}/{:.0f} MB", GetMemoryPoolShortName(*shownPool), static_cast<double>(GetPoolBytes(stats.Resident, *shownPool)) / c_BytesPerMB,
				static_cast<double>(stats.Budgets.GetPoolBudget(*shownPool)) / c_BytesPerMB);
		}
		// Compact, without the memory (the tooltip lists every pool; the color still says how full the budgets are).
		std::string compact = fmt::format("{} ready{}{} streaming", stats.LoadedAssets, c_Separator, stats.LoadingAssets);
		// Imports in the background (a project opened with assets to import): their assets stream in once they finish.
		const uint32_t importing = m_Context.GetAssetManager()->GetPendingImportCount();
		if (importing > 0)
			compact = fmt::format("{} importing{}{}", importing, c_Separator, compact);
		// A scene being opened (scene.open, a project's start scene) loads with everything it uses: the largest load while it
		// lasts. Its progress counts its document as it is read, then what it uses in bytes.
		if (m_Context.IsSceneLoading() && !m_Context.IsScanningAssets())
			compact = fmt::format("Opening scene {:.0f}%{}{}", std::floor(m_Context.GetSceneLoadStatus().Progress * 100.0f), c_Separator, compact);
		// The project's assets being scanned in the background come first: what needs them waits for it.
		const std::optional<AssetScanProgress> scan = m_Context.GetAssetScanProgress();
		if (scan)
			compact = FormatAssetScan(*scan) + c_Separator + compact;
		const std::string text = compact + c_Separator + memory;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// Failures matter most; then how full the budgets are (a comfortable budget is no news).
		const ImVec4& color = stats.FailedAssets > 0 ? colors.Error
			: (pressure == BudgetPressure::High || pressure == BudgetPressure::Over ? GetBudgetPressureColor(pressure) : colors.TextSecondary);
		ImGui::SameLine();
		const bool clicked = UI::Pill("Status.Assets", Icons::Package, StatusPillText(Icons::Package, text, compact), color);
		// Written only while it shows: the status bar is drawn every frame.
		if (UI::IsTooltipDue())
		{
			std::string tooltip = stats.FailedAssets > 0 ? fmt::format("{} assets failed to load (see the Console or the Streaming panel).", stats.FailedAssets)
				: fmt::format("Assets ready and streaming in; {} are registered.", stats.RegisteredAssets);
			if (scan)
			{
				tooltip += " The project's assets are being scanned: edits, asset commands and the start scene wait until every asset is "
					"registered.";
			}
			if (importing > 0)
			{
				tooltip += fmt::format(" {} {} importing in the background: {} once imported.", importing, importing == 1 ? "asset is" : "assets are",
					importing == 1 ? "it loads" : "they load");
			}
			if (m_Context.IsSceneLoading())
			{
				const EditorSceneLoadStatus& load = m_Context.GetSceneLoadStatus();
				tooltip += fmt::format(" A scene is being opened ({:.0f}% of it and what it uses is in; {}); it replaces the edited scene once all of it is.",
					std::floor(load.Progress * 100.0f), load.State == SceneLoadState::LoadingScene ? "its document is being read"
					: load.State == SceneLoadState::Preparing ? "its view is being prepared" : "what it uses is loading");
			}
			if (!shownPool)
			{
				tooltip += fmt::format(" Loaded assets use {}; the pools have no budgets (no graphics device).", FormatMegabytes(stats.LoadedMemory));
			}
			else
			{
				tooltip += fmt::format(" Shown: {}, the pool closest to its budget{}.", GetMemoryPoolName(*shownPool),
					pressure == BudgetPressure::Over ? " (over it: what is in use stays resident)" : (pressure == BudgetPressure::High ? " (close to it)" : ""));
				for (AssetMemoryPool pool : c_AssetMemoryPools)
				{
					const uint64_t used = GetPoolBytes(stats.Resident, pool);
					const uint64_t budget = stats.Budgets.GetPoolBudget(pool);
					tooltip += budget == AssetResidencyBudgets::c_Unlimited ? fmt::format("\n{}: {} (no budget)", GetMemoryPoolName(pool), FormatMegabytes(used))
						: fmt::format("\n{}: {} of {}", GetMemoryPoolName(pool), FormatMegabytes(used), FormatMegabytes(budget));
				}
			}
			tooltip += "\nClick to open the Streaming panel.";
			ImGui::SetTooltip("%s", tooltip.c_str());
		}
		if (clicked)
			m_Panels.Focus(EditorPanels::c_Streaming);
	}

	void EditorLayer::DrawSavingPill()
	{
		// The documents' saves: the project file's writes take a moment, and one that fails posts a notification.
		const std::vector<DocumentSaveStatus> saves = m_Context.GetSaveStatuses();
		if (saves.empty())
			return;
		const DocumentSaveStatus& first = saves.front();
		const std::string percent = fmt::format("{:.0f}%", std::floor(first.Progress * 100.0f));
		std::string text = fmt::format("Saving '{}' {}", first.Label, percent);
		if (saves.size() > 1)
			text += fmt::format("{}{} more queued", c_Separator, saves.size() - 1);
		const std::string compact = "Saving " + percent;
		ImGui::SameLine();
		const bool clicked = UI::Pill("Status.Saving", Icons::Save, StatusPillText(Icons::Save, text, compact), UI::GetThemeColors().Info);
		if (UI::IsTooltipDue())
		{
			ImGui::SetTooltip("Saving in the background: the editor goes on while the files are written; edits made meanwhile stay unsaved. "
				"Closing the project or the editor waits for the save. Click to see it in the Activity panel.");
		}
		if (clicked)
			m_Panels.Focus(EditorPanels::c_Activity);
	}

	void EditorLayer::DockNewToolPanels()
	{
		// Only windows the layout knows nothing about: a panel the user moved elsewhere stays there.
		const auto dockNew = [this](const char* id, const ImGuiWindow* beside)
		{
			const std::string name = m_Panels.GetWindowName(id);
			if (beside && beside->DockId != 0 && m_Panels.IsOpen(id) && !ImGui::FindWindowByName(name.c_str())
				&& !ImGui::FindWindowSettingsByID(ImHashStr(name.c_str())))
			{
				ImGui::DockBuilderDockWindow(name.c_str(), beside->DockId);
			}
		};
		const ImGuiWindow* console = ImGui::FindWindowByName(m_Panels.GetWindowName(EditorPanels::c_Console).c_str());
		for (const char* id : { EditorPanels::c_Problems, EditorPanels::c_Streaming, EditorPanels::c_Performance, EditorPanels::c_Activity })
			dockNew(id, console);
		// The Project Settings' pages need room: beside the Scene view.
		dockNew(EditorPanels::c_ProjectSettings, ImGui::FindWindowByName(m_Panels.GetWindowName(EditorPanels::c_Viewport).c_str()));
	}

	bool EditorLayer::DrawAutomationPill()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (m_Automation.IsRunning())
		{
			// Who is connected, by the names clients gave (informational: never trusted).
			if (m_AutomationPillVersion != m_Automation.GetClientsVersion() || m_AutomationPillPort != m_Automation.GetPort())
			{
				m_AutomationPillVersion = m_Automation.GetClientsVersion();
				m_AutomationPillPort = m_Automation.GetPort();
				m_AutomationClients = m_Automation.GetClients();
				const auto named = std::find_if(m_AutomationClients.begin(), m_AutomationClients.end(), [](const RpcClientInfo& client) { return !client.Name.empty(); });
				const std::string name = named != m_AutomationClients.end() ? fmt::format("{}{}", c_Separator, named->Name) : std::string();
				// Compact: who, without the port and the words around it (the tooltip has them).
				if (m_AutomationClients.empty())
				{
					m_AutomationPillText = fmt::format("port {}{}no agent", m_AutomationPillPort, c_Separator);
					m_AutomationPillCompactText = "No agent";
				}
				else if (m_AutomationClients.size() == 1)
				{
					m_AutomationPillText = "Agent connected" + name;
					m_AutomationPillCompactText = named != m_AutomationClients.end() ? named->Name : m_AutomationPillText;
				}
				else
				{
					m_AutomationPillText = fmt::format("{} agents connected{}", m_AutomationClients.size(), name);
					m_AutomationPillCompactText = fmt::format("{} agents", m_AutomationClients.size());
				}
			}
			UI::Pill("Status.Automation", Icons::Bot, StatusPillText(Icons::Bot, m_AutomationPillText, m_AutomationPillCompactText),
				m_AutomationClients.empty() ? colors.TextSecondary : colors.Info);
			if (UI::IsTooltipDue())
			{
				std::string tooltip = fmt::format("Tools and AI agents control this editor through StrataCLI (or its MCP server, StrataCLI mcp), which "
					"finds it through its session file (port {}).", m_AutomationPillPort);
				const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
				for (const RpcClientInfo& client : m_AutomationClients)
				{
					tooltip += fmt::format("\n{} (connected {})", client.Name.empty() ? std::string("A client without a name") : client.Name,
						UI::DescribeTimeAgo(client.ConnectedAt / 1000, now));
				}
				tooltip += fmt::format("\n{} pending requests, {} answered", m_Automation.GetPendingRequestCount(), m_Automation.GetCompletedRequestCount());
				ImGui::SetTooltip("%s", tooltip.c_str());
			}
			return false;
		}
		if (!m_Options.EnableAutomation)
		{
			UI::Pill("Status.Automation", Icons::Bot, StatusPillText(Icons::Bot, "Automation off"), colors.TextSecondary,
				"Started with --no-automation: tools cannot control this editor");
			return false;
		}
		// Asked for, but it could not start (a taken --automation-port, a session file that cannot be written).
		const std::string tooltip = fmt::format("Automation could not start, so tools cannot control this editor: {}\nClick to open the Console.",
			m_AutomationError.empty() ? std::string("see the Console") : m_AutomationError);
		return UI::Pill("Status.Automation", Icons::Bot, StatusPillText(Icons::Bot, "Automation failed"), colors.Error, tooltip.c_str());
	}

	bool EditorLayer::DrawScriptsPill()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ScriptBuilder& builder = m_Context.GetScriptBuilder();
		const Ref<ScriptEngine>& engine = m_Context.GetScriptEngine();
		if (builder.IsRunning())
		{
			// Clicking cancels: a build Play waits for, too (Play then does not start).
			const bool forPlay = m_Context.GetPlayBuildID() == builder.GetCurrentID();
			const std::string compact = fmt::format("Building {:.0f} s", builder.GetElapsedSeconds());
			const std::string text = forPlay ? fmt::format("Building to play {:.0f} s", builder.GetElapsedSeconds()) : compact;
			if (UI::Pill("Status.Scripts", Icons::LoaderCircle, StatusPillText(Icons::LoaderCircle, text, compact), colors.Info,
				forPlay ? "The scripts changed: they build before the game plays (the output goes to the Console). Click to cancel."
				: "The project's scripts are being built (the output goes to the Console). Click to cancel."))
			{
				m_Context.CancelScriptBuild();
			}
			return false;
		}
		if (engine && engine->IsFaulted())
		{
			if (UI::Pill("Status.Scripts", Icons::CircleAlert, StatusPillText(Icons::CircleAlert, "Scripts crashed"), colors.Error,
				"The scripts crashed: rebuild or reload them. Click to see where (the Problems panel)."))
			{
				m_Panels.Focus(EditorPanels::c_Problems);
			}
			return false;
		}
		// A build someone cancelled failed at nothing: the pill goes back to what the module is.
		if (builder.GetLastResult().ID != 0 && !builder.GetLastResult().Success && !builder.GetLastResult().Cancelled)
		{
			// Its errors as the Problems panel counts them.
			const size_t errors = CountScriptBuildErrors(builder.GetLastResult());
			const std::string text = errors == 0 ? std::string("Build failed")
				: fmt::format("Build failed{}{} {}", c_Separator, errors, errors == 1 ? "error" : "errors");
			if (UI::Pill("Status.Scripts", Icons::CircleAlert, StatusPillText(Icons::CircleAlert, text, "Build failed"), colors.Error,
				"The last script build failed. Click to see its errors (the Problems panel)."))
				m_Panels.Focus(EditorPanels::c_Problems);
			return false;
		}
		if (engine && engine->IsModuleLoaded())
		{
			const size_t classes = engine->GetClasses().size();
			const std::string compact = fmt::format("{} {}", classes, classes == 1 ? "class" : "classes");
			const std::string text = engine->GetModuleName() + c_Separator + compact;
			UI::Pill("Status.Scripts", Icons::FileCode, StatusPillText(Icons::FileCode, text, compact), colors.TextSecondary);
			if (UI::IsTooltipDue())
				ImGui::SetTooltip("The loaded script module, %s: %s", engine->GetModuleName().c_str(), compact.c_str());
			return false;
		}
		// The project has scripts that its scenes use but no module runs them: never built here (e.g. a fresh copy of a sample),
		// or built by another engine version and refused. Scripts no scene uses yet (a new project's example) are no reason to
		// ask for a build.
		if (HasScriptBuild() && m_Context.DoScenesUseScripts())
		{
			if (UI::Pill("Status.Scripts", Icons::Hammer, StatusPillText(Icons::Hammer, "Scripts not built"), colors.Warning,
				"The project has scripts, but no script module is loaded: they are not built yet, or their module could not be loaded "
				"(see the Console). Click to build them (Ctrl+B)."))
			{
				BuildScripts();
			}
			return false;
		}
		if (HasScriptBuild())
		{
			UI::Pill("Status.Scripts", Icons::FileCode, StatusPillText(Icons::FileCode, "Scripts unused"), colors.TextSecondary,
				"The project has scripts, but no scene uses one yet: add a Script component to an entity (Play builds them first)");
			return false;
		}
		UI::Pill("Status.Scripts", Icons::FileCode, StatusPillText(Icons::FileCode, "No scripts"), colors.TextSecondary,
			m_Context.HasProject() ? "The project has no scripts (Scripts > New Script... adds one)" : "No project is open");
		return false;
	}

	bool EditorLayer::HasScriptBuild()
	{
		const double now = m_Host->GetTime();
		if (!m_ScriptBuildCheckTime || now - *m_ScriptBuildCheckTime >= c_ScriptBuildCheckSeconds || now < *m_ScriptBuildCheckTime)
		{
			m_HasScriptBuild = m_Context.HasScriptBuild();
			m_ScriptBuildCheckTime = now;
		}
		return m_HasScriptBuild;
	}

	void EditorLayer::DrawDiscardQuestion()
	{
		DrawUnsavedChangesModal();
	}

	void EditorLayer::DrawUnwrittenSettingsQuestion()
	{
		// Written meanwhile (the retry, or a later change): nothing is lost any more, and the action goes on.
		if (!m_RetryingProjectWrite && !m_Context.HasUnwrittenProjectSettings() && m_Context.GetProjectFileState() != SettingsFileState::Writing)
		{
			ImGui::CloseCurrentPopup();
			RunPendingReleaseAction();
			return;
		}
		const std::string file = m_Context.HasProject() ? FileSystem::ToUTF8(m_Context.GetProject()->GetProjectFile().filename()) : std::string();
		ImGui::Text("The project file '%s' could not be written:", file.c_str());
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
		ImGui::TextColored(UI::GetThemeColors().Warning, "%s", m_Context.GetProjectFileError().c_str());
		ImGui::TextUnformatted("The project's settings changed since are only in the editor: going on discards them.");
		ImGui::PopTextWrapPos();
		UI::ItemProbe::Record("UnwrittenSettings.Dialog");
		ImGui::Spacing();
		const bool writing = m_RetryingProjectWrite || m_Context.GetProjectFileState() == SettingsFileState::Writing;
		ImGui::BeginDisabled(writing);
		if (UI::DialogButton("UnwrittenSettings.Retry", writing ? "Writing..." : "Retry", true))
		{
			// Through the runner, like every save the UI starts; the question closes once the file is written.
			m_RetryingProjectWrite = true;
			m_CommandRunner.Run(m_Context, m_Commands, "project.save", nlohmann::json::object(), [this](const EditorCommandResult&)
			{
				m_RetryingProjectWrite = false;
			}, CommandOrigin { CommandSource::UI });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (UI::DialogButton("UnwrittenSettings.Discard", "Don't Save"))
		{
			ImGui::CloseCurrentPopup();
			RunPendingReleaseAction();
		}
		ImGui::SameLine();
		if (UI::DialogButton("UnwrittenSettings.Cancel", "Cancel"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingReleaseAction = nullptr;
		}
	}

	void EditorLayer::DrawUnwrittenPreferencesQuestion()
	{
		// The answer is for the preferences and for the scene as it was when asked: changes of the scene made while the question
		// waited are asked about first (RequestQuit asks again), like RunPendingReleaseAction does.
		auto quit = [this]()
		{
			std::function<void()> action = std::exchange(m_PendingQuitAction, nullptr);
			if (!action)
				return;
			if (m_Context.IsSceneModified() && m_Context.GetUndoStack().GetVersion() != m_QuitQuestionVersion)
				RequestQuit(std::move(action));
			else
				action();
		};
		const EditorPreferences& preferences = m_Context.GetPreferences();
		// Written meanwhile (the retry, or a later change): nothing is lost any more, and the editor closes.
		if (!m_RetryingPreferencesWrite && !preferences.HasUnwrittenPreferences() && !preferences.IsWriting())
		{
			ImGui::CloseCurrentPopup();
			quit();
			return;
		}
		const std::string file = FileSystem::ToUTF8(preferences.GetFile());
		ImGui::Text("The preferences file '%s' could not be written:", file.c_str());
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
		ImGui::TextColored(UI::GetThemeColors().Warning, "%s", preferences.GetFileError().c_str());
		ImGui::TextUnformatted("The preferences changed since are only in the editor: quitting discards them.");
		ImGui::PopTextWrapPos();
		UI::ItemProbe::Record("UnwrittenPreferences.Dialog");
		ImGui::Spacing();
		const bool writing = m_RetryingPreferencesWrite || preferences.IsWriting();
		ImGui::BeginDisabled(writing);
		if (UI::DialogButton("UnwrittenPreferences.Retry", writing ? "Writing..." : "Retry", true))
		{
			// Through the runner, like every save the UI starts; the question closes once the file is written.
			m_RetryingPreferencesWrite = true;
			m_CommandRunner.Run(m_Context, m_Commands, "preferences.save", nlohmann::json::object(), [this](const EditorCommandResult&)
			{
				m_RetryingPreferencesWrite = false;
			}, CommandOrigin { CommandSource::UI });
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (UI::DialogButton("UnwrittenPreferences.Discard", "Don't Save"))
		{
			ImGui::CloseCurrentPopup();
			quit();
		}
		ImGui::SameLine();
		if (UI::DialogButton("UnwrittenPreferences.Cancel", "Cancel"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingQuitAction = nullptr;
		}
	}

	void EditorLayer::DrawUnsavedChangesModal()
	{
		// A dialog that asked draws the question over itself before the window's own call comes (popups belong to the level
		// they are opened at: opened over the whole window, the question would close that dialog).
		if (m_UnsavedChangesModalFrame == ImGui::GetFrameCount())
			return;
		m_UnsavedChangesModalFrame = ImGui::GetFrameCount();
		if (m_OpenUnsavedChangesModal)
		{
			UI::OpenModal(c_UnsavedChangesModal);
			m_OpenUnsavedChangesModal = false;
		}
		if (m_OpenUnwrittenSettingsModal)
		{
			UI::OpenModal(c_UnwrittenSettingsModal);
			m_OpenUnwrittenSettingsModal = false;
		}
		if (UI::BeginModal(c_UnwrittenSettingsModal, "Project Settings Not Written"))
		{
			DrawUnwrittenSettingsQuestion();
			UI::EndModal();
			return;
		}
		if (m_OpenUnwrittenPreferencesModal)
		{
			UI::OpenModal(c_UnwrittenPreferencesModal);
			m_OpenUnwrittenPreferencesModal = false;
		}
		if (UI::BeginModal(c_UnwrittenPreferencesModal, "Preferences Not Written"))
		{
			DrawUnwrittenPreferencesQuestion();
			UI::EndModal();
			return;
		}
		// While saves are in flight, the action that waits for them (RequestDiscardChanges) shows their progress.
		if (m_OpenSavingModal)
		{
			UI::OpenModal(c_SavingModal);
			m_OpenSavingModal = false;
		}
		if (UI::BeginModal(c_SavingModal, "Saving"))
		{
			if (!m_Context.HasSavesInFlight())
			{
				ImGui::CloseCurrentPopup();
				UI::EndModal();
				// Written: what replaces the scene now asks about the changes made since, if there are any.
				if (std::function<void()> action = std::exchange(m_ActionAfterSaves, nullptr))
					RequestDiscardChanges(std::move(action));
				return;
			}
			ImGui::TextUnformatted("Waiting for the saves to finish before going on:");
			for (const DocumentSaveStatus& save : m_Context.GetSaveStatuses())
			{
				const std::string label = fmt::format("Saving '{}'... {:.0f}%", save.Label, std::floor(save.Progress * 100.0f));
				UI::StrataProgressBar(fmt::format("Saving.Progress.{}", save.ID).c_str(), save.Progress, label);
			}
			if (m_Context.GetProjectFileState() == SettingsFileState::Writing)
				ImGui::Text("Writing '%s'...", FileSystem::ToUTF8(m_Context.GetProject()->GetProjectFile().filename()).c_str());
			// Quitting also waits for the preferences (HasSavesInFlight).
			if (m_Context.GetPreferences().IsWriting())
			{
				const std::string preferences = fmt::format("Writing '{}'...", FileSystem::ToUTF8(m_Context.GetPreferences().GetFile().filename()));
				ImGui::TextUnformatted(preferences.c_str());
				UI::ItemProbe::Record("Saving.Preferences", true, 0, preferences);
			}
			UI::ItemProbe::Record("Saving.Dialog");
			ImGui::Spacing();
			// Cancel gives up what was to happen next, never a save.
			if (UI::DialogButton("Saving.Cancel", "Cancel"))
			{
				ImGui::CloseCurrentPopup();
				m_ActionAfterSaves = nullptr;
			}
			UI::EndModal();
			return;
		}
		if (!UI::BeginModal(c_UnsavedChangesModal, "Unsaved Changes"))
			return;
		ImGui::Text("Save changes to '%s'?", m_Context.GetEditScene()->GetName().c_str());
		if (m_SceneChangedSinceQuestion)
		{
			ImGui::TextColored(UI::GetThemeColors().Warning, "The scene changed while you were asked (an agent or a script edited it).");
			UI::ItemProbe::Record("UnsavedChanges.Changed");
			ImGui::TextUnformatted("Don't Save discards those changes too.");
		}
		ImGui::Spacing();
		if (UI::DialogButton("UnsavedChanges.Save", "Save", true))
		{
			ImGui::CloseCurrentPopup();
			// The pending action runs once the save is written; it asks again about changes made meanwhile.
			SaveScene([this, action = std::exchange(m_PendingDiscardAction, nullptr)](bool saved)
			{
				if (saved && action)
					RequestDiscardChanges(action);
			});
		}
		ImGui::SameLine();
		if (UI::DialogButton("UnsavedChanges.DontSave", "Don't Save"))
		{
			// The answer is for the changes the person was asked about: changes made since (an agent's, a script's) are asked
			// about again rather than discarded unseen. Without unsaved changes left, nothing is discarded either way.
			const uint64_t version = m_Context.GetUndoStack().GetVersion();
			if (m_Context.IsSceneModified() && version != m_DiscardQuestionVersion)
			{
				m_DiscardQuestionVersion = version;
				m_SceneChangedSinceQuestion = true;
			}
			else
			{
				ImGui::CloseCurrentPopup();
				if (m_PendingDiscardAction)
					m_PendingDiscardAction();
				m_PendingDiscardAction = nullptr;
			}
		}
		ImGui::SameLine();
		if (UI::DialogButton("UnsavedChanges.Cancel", "Cancel"))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDiscardAction = nullptr;
		}
		UI::EndModal();
	}

}
