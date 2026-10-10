#include "EditorLayer.h"

#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ViewportPanel.h"
#include "UI/EditorFonts.h"
#include "UI/FileDialogs.h"
#include "UI/Icons.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageWriter.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		constexpr const char* c_EditorStatusSection = "editor";
		// Between the parts of a pill's text.
		constexpr const char* c_Separator = " \xC2\xB7 ";

		// The default layout, as fractions of the dock space: the viewport gets over 45% of a 3840 x 2054 window at 150%.
		constexpr float c_HierarchyWidth = 0.15f;
		constexpr float c_InspectorWidth = 0.20f;
		constexpr float c_BottomHeight = 0.22f;

		// Fields of the snap steps popup, in text heights.
		constexpr float c_SnapFieldWidthInFontSizes = 8.0f;

		EditorContextSpecification MakeContextSpecification(const EditorOptions& options)
		{
			EditorContextSpecification specification;
			specification.WatchAssetFiles = options.WatchFiles;
			specification.HotReloadScripts = options.WatchFiles;
			return specification;
		}

		glm::vec4 ToVec4(const ImVec4& color)
		{
			return glm::vec4(color.x, color.y, color.z, color.w);
		}

		std::string FormatMegabytes(uint64_t bytes)
		{
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		}

	}

	EditorLayer::EditorLayer(const EditorOptions& options, Scope<EditorHost> host)
		: Layer("EditorLayer"), m_Options(options), m_Host(std::move(host)), m_Context(MakeContextSpecification(options)),
		m_Automation(m_Context, m_Commands, m_CommandRunner), m_ShowImGuiDemo(options.ShowImGuiDemo)
	{
		ST_ASSERT(m_Host, "The editor layer needs a host");
		RegisterBuiltinPanels();
	}

	void EditorLayer::RegisterBuiltinPanels()
	{
		auto add = [this](const char* id, const char* title, const char* icon, std::function<Scope<EditorPanel>()> create)
		{
			EditorPanelDescriptor descriptor;
			descriptor.Id = id;
			descriptor.Title = title;
			descriptor.Icon = icon;
			descriptor.Create = std::move(create);
			std::string error;
			if (!m_Panels.Register(std::move(descriptor), &error))
				ST_ERROR("Could not register the panel '{}': {}", id, error);
		};
		// Drawn in this order; the Content Browser comes after the Console, which shares its dock node.
		add(EditorPanels::c_Viewport, "Viewport", Icons::Box, []() { return CreateScope<ViewportPanel>(); });
		add(EditorPanels::c_Hierarchy, "Hierarchy", Icons::ListTree, []() { return CreateScope<SceneHierarchyPanel>(); });
		add(EditorPanels::c_Inspector, "Inspector", Icons::SlidersHorizontal, []() { return CreateScope<InspectorPanel>(); });
		add(EditorPanels::c_Console, "Console", Icons::Terminal, []() { return CreateScope<ConsolePanel>(); });
		add(EditorPanels::c_ContentBrowser, "Content Browser", Icons::FolderOpen, []() { return CreateScope<ContentBrowserPanel>(); });

		if (ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
		{
			contentBrowser->SetOpenSceneHandler([this](AssetHandle scene)
			{
				RequestDiscardChanges([this, scene]() { RunEditorCommand(m_Context, m_Commands, "scene.open", { { "scene", UUIDToJson(scene) } }); });
			});
		}
	}

	void EditorLayer::OnAttach()
	{
		if (!m_Options.Headless && m_Host->HasWindow())
			FileDialogs::Init();
		if (ImGui::GetCurrentContext())
			m_Panels.InstallSettingsHandler();
		// The selection outline carries the accent, like selected rows.
		m_Context.GetViewport().GetSettings().SelectionColor = ToVec4(UI::GetThemeColors().Accent);

		if (!m_Options.ProjectPath.empty())
		{
			std::string error;
			if (!m_Context.OpenProject(m_Options.ProjectPath, &error))
				ST_ERROR("Could not open the project '{}': {}", FileSystem::ToUTF8(m_Options.ProjectPath), error);
		}

		m_Context.SetStatusProvider(c_EditorStatusSection, [this]()
		{
			return nlohmann::json {
				{ "headless", m_Options.Headless },
				{ "graphicsDevice", m_Host->HasGraphicsDevice() },
				{ "frame", m_Host->GetFrameCount() },
				{ "maxFrames", m_Options.MaxFrames ? nlohmann::json(*m_Options.MaxFrames) : nlohmann::json(nullptr) } };
		});
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
		if (m_Options.QuitAfterCommands)
			m_Host->Close();
	}

	void EditorLayer::OnDetach()
	{
		// Before the project closes: completions may still look at the editor state. Automation stops afterwards, so the
		// clients of cancelled commands still get their answers.
		m_CommandRunner.CancelAll("The editor is closing");
		if (m_UIDrawn)
		{
			EditorPanelContext panelContext { m_Context, m_Commands, m_CommandRunner };
			m_Panels.OnDetach(panelContext);
		}
		m_Panels.RemoveSettingsHandler();
		m_Automation.Stop();
		m_Context.SetStatusProvider(c_EditorStatusSection, nullptr);
		m_Context.CloseProject();
		FileDialogs::Shutdown();
	}

	void EditorLayer::OnUpdate(Timestep timestep)
	{
		m_Context.Update(timestep);
		m_CommandRunner.Update(m_Context);
		m_Automation.Update();
		UpdateCommandScript();
		UpdateWindowTitle();
		if (!m_Options.Headless)
		{
			EditorPanelContext panelContext { m_Context, m_Commands, m_CommandRunner };
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
			ST_WARN("No automation client for {} s (--idle-timeout): closing the editor{}", m_Options.IdleTimeout.count(),
				m_Context.IsSceneModified() ? " and discarding unsaved scene changes" : "");
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
		{
			m_Host->RequestScreenshot([path = m_Options.ScreenshotPath](const ReadbackImage& image)
			{
				std::string error;
				if (ImageWriter::SavePNG(image, path, true, &error))
					ST_INFO("Saved screenshot to {}", FileSystem::ToUTF8(path));
				else
					ST_ERROR("Screenshot failed: {}", error);
			});
		}
	}

	void EditorLayer::UpdateWindowTitle()
	{
		if (!m_Host->HasWindow())
			return;
		std::string title = "Strata Editor";
		if (m_Context.HasProject())
			title += " - " + m_Context.GetProject()->GetConfig().Name;
		title += " - " + m_Context.GetEditScene()->GetName();
		if (m_Context.IsSceneModified())
			title += " *";
		if (m_Context.IsPlaying())
			title += fmt::format(" [{}]", SceneStateToString(m_Context.GetSceneState()));
		if (title != m_WindowTitle)
		{
			m_Host->SetWindowTitle(title);
			m_WindowTitle = std::move(title);
		}
	}

	void EditorLayer::OnImGuiRender()
	{
		m_UIDrawn = true;
		HandleShortcuts();
		DrawDockspace();
		EditorPanelContext panelContext { m_Context, m_Commands, m_CommandRunner };
		m_Panels.OnImGuiRender(panelContext);
		DrawUnsavedChangesModal();
		if (m_ShowImGuiDemo)
			ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
	}

	void EditorLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowCloseEvent>([this](WindowCloseEvent&)
		{
			if (!m_Context.IsSceneModified())
				return false;
			// Keep running and ask first; closing happens from the dialog.
			RequestDiscardChanges([this]() { m_Host->Close(); });
			return true;
		});
		dispatcher.Dispatch<WindowFileDropEvent>([this](WindowFileDropEvent& drop)
		{
			if (!m_Context.HasProject())
			{
				ST_WARN("Open or create a project before importing files");
				return true;
			}
			if (ContentBrowserPanel* contentBrowser = m_Panels.Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser))
				contentBrowser->ImportFiles(m_Context, m_Commands, drop.GetPaths());
			return true;
		});
	}

	////////////////////////////////////////////////////////////////////////////////
	// Actions
	////////////////////////////////////////////////////////////////////////////////

	void EditorLayer::HandleShortcuts()
	{
		// Nothing changes behind a modal dialog (global routes ignore modality).
		if (ImGui::GetTopMostPopupModal())
			return;
		const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
		// Play mode toggles always: it is also the way out of a game that has the input.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global))
			RunEditorCommand(m_Context, m_Commands, m_Context.IsPlaying() ? "play.stop" : "play.start");
		// Keys the running game receives (e.g. Delete, or Ctrl+D with Ctrl to crouch) must not edit the scene.
		if (!m_Context.AcceptsEditShortcuts())
			return;
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global))
			m_Context.Undo();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global))
			m_Context.Redo();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global))
			SaveScene();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global))
			SaveSceneAs();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global))
			DuplicateSelection();
		// Like the menu item and the toolbar button, the shortcut does nothing while a build runs.
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global) && !m_Context.GetScriptBuilder().IsRunning())
			BuildScripts();
		if (ImGui::Shortcut(ImGuiKey_Delete, global))
			DeleteSelection();
	}

	void EditorLayer::RequestDiscardChanges(std::function<void()> action)
	{
		if (m_Context.IsSceneModified())
		{
			m_PendingDiscardAction = std::move(action);
			m_OpenUnsavedChangesModal = true;
			return;
		}
		action();
	}

	bool EditorLayer::SaveScene()
	{
		if (m_Context.IsPlaying())
		{
			ST_WARN("Stop playing before saving: changes made while playing are discarded");
			return false;
		}
		if (!m_Context.GetSceneHandle().IsValid())
			return SaveSceneAs();
		return !RunEditorCommand(m_Context, m_Commands, "scene.save").is_null();
	}

	bool EditorLayer::SaveSceneAs()
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
		return !RunEditorCommand(m_Context, m_Commands, "scene.saveAs", { { "path", FileSystem::ToUTF8(relative) } }).is_null();
	}

	void EditorLayer::NewProject()
	{
		std::optional<std::filesystem::path> directory = FileDialogs::PickFolder();
		if (!directory)
			return;
		RequestDiscardChanges([this, directory = *directory]()
		{
			RunEditorCommand(m_Context, m_Commands, "project.create",
				{ { "directory", FileSystem::ToUTF8(directory) }, { "name", FileSystem::ToUTF8(directory.filename()) } });
		});
	}

	void EditorLayer::OpenProject()
	{
		std::optional<std::filesystem::path> file = FileDialogs::OpenFile({ { "Strata Project", "stproj" } });
		if (!file)
			return;
		RequestDiscardChanges([this, file = *file]()
		{
			RunEditorCommand(m_Context, m_Commands, "project.open", { { "path", FileSystem::ToUTF8(file) } });
		});
	}

	void EditorLayer::DeleteSelection()
	{
		if (m_Context.GetSelection().empty() || ImGui::GetIO().WantTextInput)
			return;
		nlohmann::json ids = nlohmann::json::array();
		for (UUID id : m_Context.GetSelection())
			ids.push_back(UUIDToJson(id));
		RunEditorCommand(m_Context, m_Commands, "entity.delete", { { "entities", ids } });
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
		});
	}

	void EditorLayer::DuplicateSelection()
	{
		Entity primary = m_Context.GetPrimarySelection();
		if (!primary)
			return;
		const nlohmann::json copy = RunEditorCommand(m_Context, m_Commands, "entity.duplicate", { { "entity", UUIDToJson(primary.GetUUID()) } });
		if (copy.is_object())
			m_Context.Select(*UUIDFromJson(copy["id"]));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Interface
	////////////////////////////////////////////////////////////////////////////////

	void EditorLayer::BuildDefaultLayout(unsigned int dockspaceId)
	{
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetContentRegionAvail());

		// The viewport first: the side panels take a fixed share of the width, the bottom area a share of the height under
		// the viewport only.
		ImGuiID center = dockspaceId;
		const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, c_HierarchyWidth, nullptr, &center);
		const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, c_InspectorWidth / (1.0f - c_HierarchyWidth), nullptr, &center);
		const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, c_BottomHeight, nullptr, &center);

		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Hierarchy).c_str(), left);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Inspector).c_str(), right);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Console).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_ContentBrowser).c_str(), bottom);
		ImGui::DockBuilderDockWindow(m_Panels.GetWindowName(EditorPanels::c_Viewport).c_str(), center);
		ImGui::DockBuilderFinish(dockspaceId);
		// The Content Browser is the bottom area's visible tab on first run.
		m_Panels.SelectTab(EditorPanels::c_ContentBrowser, bottom);
	}

	void EditorLayer::DrawDockspace()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const ImGuiStyle& style = ImGui::GetStyle();
		// The status bar uses the caption size.
		const float statusBarHeight = UI::GetTextSize(UI::TextSize::Caption) * style.FontScaleMain * style.FontScaleDpi + style.FramePadding.y * 2.0f;
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

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Project..."))
				NewProject();
			if (ImGui::MenuItem("Open Project..."))
				OpenProject();
			ImGui::Separator();
			if (ImGui::MenuItem("New Scene", nullptr, false, !m_Context.IsPlaying()))
				RequestDiscardChanges([this]() { RunEditorCommand(m_Context, m_Commands, "scene.new"); });
			if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_Context.IsPlaying()))
				SaveScene();
			if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, m_Context.HasProject() && !m_Context.IsPlaying()))
				SaveSceneAs();
			if (ImGui::MenuItem("Set as Start Scene", nullptr, false, m_Context.HasProject() && m_Context.GetSceneHandle().IsValid()))
				RunEditorCommand(m_Context, m_Commands, "project.setStartScene", { { "scene", UUIDToJson(m_Context.GetSceneHandle()) } });
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				RequestDiscardChanges([this]() { m_Host->Close(); });
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			const std::string undo = m_Context.GetUndoStack().GetUndoName();
			const std::string redo = m_Context.GetUndoStack().GetRedoName();
			if (ImGui::MenuItem(undo.empty() ? "Undo" : ("Undo " + undo).c_str(), "Ctrl+Z", false, !undo.empty() && !m_Context.IsPlaying()))
				m_Context.Undo();
			if (ImGui::MenuItem(redo.empty() ? "Redo" : ("Redo " + redo).c_str(), "Ctrl+Y", false, !redo.empty() && !m_Context.IsPlaying()))
				m_Context.Redo();
			ImGui::Separator();
			if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_Context.GetPrimarySelection().IsValid()))
				DuplicateSelection();
			if (ImGui::MenuItem("Delete", "Del", false, !m_Context.GetSelection().empty()))
				DeleteSelection();
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

		if (ImGui::BeginMenu("Scripts"))
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
			if (engine && engine->IsModuleLoaded())
				ImGui::TextDisabled("%s: %zu classes, loaded %llu times", engine->GetModuleName().c_str(), engine->GetClasses().size(),
					static_cast<unsigned long long>(engine->GetLoadCount()));
			else
				ImGui::TextDisabled("No script module loaded");
			ImGui::EndMenu();
		}

		// ImGui's demo is a reference for UI work, not part of the product: only with --imgui-demo.
		if (m_Options.ShowImGuiDemo && ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
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

		// Play is the primary action while editing; while a mode runs, its button shows it in the mode's color.
		UI::ButtonStyle play;
		play.Enabled = !playing;
		play.Primary = !playing;
		play.Active = state == SceneState::Play;
		play.ActiveColor = colors.PlayState.Play;
		if (UI::ToolbarButton("Toolbar.Play", Icons::Play, nullptr, playing ? "Playing" : "Play: run the scene with scripts, physics and audio (Ctrl+P)", play))
			RunEditorCommand(m_Context, m_Commands, "play.start");
		ImGui::SameLine();

		UI::ButtonStyle simulate;
		simulate.Enabled = !playing;
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
			RunEditorCommand(m_Context, m_Commands, "play.step");
		ImGui::SameLine();

		UI::ButtonStyle stop;
		stop.Enabled = playing;
		if (UI::ToolbarButton("Toolbar.Stop", Icons::Square, nullptr, "Stop and return to the edited scene (Ctrl+P)", stop))
			RunEditorCommand(m_Context, m_Commands, "play.stop");
	}

	void EditorLayer::DrawBuildControls()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const ScriptBuilder& builder = m_Context.GetScriptBuilder();
		const bool building = builder.IsRunning();
		const bool failed = !building && builder.GetLastResult().ID != 0 && !builder.GetLastResult().Success;

		const std::string label = building ? fmt::format("Building {:.0f} s", builder.GetElapsedSeconds()) : std::string("Build Scripts");
		const char* icon = building ? Icons::LoaderCircle : (failed ? Icons::CircleAlert : Icons::Hammer);
		UI::ButtonStyle buildStyle;
		buildStyle.Enabled = m_Context.HasProject() && !building;
		buildStyle.Active = building || failed;
		buildStyle.ActiveColor = building ? colors.Info : colors.Error;
		const char* tooltip = failed ? "The last build failed (see the Console): build the scripts again (Ctrl+B)"
			: "Build the project's scripts and hot-reload them (Ctrl+B)";

		// Right-aligned.
		const float width = style.FramePadding.x * 2.0f + ImGui::CalcTextSize(icon).x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label.c_str()).x;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetWindowWidth() - style.WindowPadding.x - width, ImGui::GetCursorPosX()));
		if (UI::ToolbarButton("Toolbar.BuildScripts", icon, label.c_str(), tooltip, buildStyle))
			BuildScripts();
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
		UI::Pill("Status.PlayState", stateIcon, stateText, stateColor, "The scene's state: changes made while it runs are discarded when it stops");

		// Frame time.
		const float framerate = ImGui::GetIO().Framerate;
		const std::string frame = fmt::format("{:.1f} ms{}{:.0f} FPS", framerate > 0.0f ? 1000.0f / framerate : 0.0f, c_Separator, framerate);
		ImGui::SameLine();
		UI::Pill("Status.Frame", Icons::Gauge, frame, colors.TextSecondary, "Frame time and rate");

		// Assets.
		if (const EditorAssetManager* assets = m_Context.GetAssetManager())
		{
			const AssetManagerStats stats = assets->GetStats();
			const std::string text = fmt::format("{} ready{}{} loading{}{}", stats.LoadedAssets, c_Separator, stats.LoadingAssets, c_Separator,
				FormatMegabytes(stats.LoadedMemory));
			const std::string tooltip = stats.FailedAssets > 0
				? fmt::format("{} assets failed to load (see the Console). Loaded assets use {}.", stats.FailedAssets, FormatMegabytes(stats.LoadedMemory))
				: fmt::format("Loaded assets and the memory they use; {} are registered.", stats.RegisteredAssets);
			ImGui::SameLine();
			UI::Pill("Status.Assets", Icons::Package, text, stats.FailedAssets > 0 ? colors.Error : colors.TextSecondary, tooltip.c_str());
		}

		// Automation.
		ImGui::SameLine();
		if (m_Automation.IsRunning())
		{
			const uint32_t clients = m_Automation.GetClientCount();
			const std::string text = fmt::format("port {}{}{} {}", m_Automation.GetPort(), c_Separator, clients, clients == 1 ? "client" : "clients");
			const std::string tooltip = fmt::format("Tools and AI agents control this editor through StrataCLI (or its MCP server, StrataCLI mcp), which "
				"finds it through its session file.\n{} pending requests, {} answered", m_Automation.GetPendingRequestCount(), m_Automation.GetCompletedRequestCount());
			UI::Pill("Status.Automation", Icons::Bot, text, clients > 0 ? colors.Info : colors.TextSecondary, tooltip.c_str());
		}
		else
		{
			UI::Pill("Status.Automation", Icons::Bot, "Automation off", colors.TextDisabled, "Started with --no-automation: tools cannot control this editor");
		}

		// Scripts.
		const ScriptBuilder& builder = m_Context.GetScriptBuilder();
		const Ref<ScriptEngine>& engine = m_Context.GetScriptEngine();
		ImGui::SameLine();
		bool showConsole = false;
		if (builder.IsRunning())
		{
			UI::Pill("Status.Scripts", Icons::LoaderCircle, fmt::format("Building {:.0f} s", builder.GetElapsedSeconds()), colors.Info,
				"The project's scripts are being built (the output goes to the Console)");
		}
		else if (engine && engine->IsFaulted())
		{
			showConsole = UI::Pill("Status.Scripts", Icons::CircleAlert, "Scripts crashed", colors.Error, "The scripts crashed: rebuild or reload them (see the Console)");
		}
		else if (builder.GetLastResult().ID != 0 && !builder.GetLastResult().Success)
		{
			showConsole = UI::Pill("Status.Scripts", Icons::CircleAlert, "Build failed", colors.Error, "The last script build failed (see the Console)");
		}
		else if (engine && engine->IsModuleLoaded())
		{
			const std::string text = fmt::format("{}{}{} classes", engine->GetModuleName(), c_Separator, engine->GetClasses().size());
			UI::Pill("Status.Scripts", Icons::FileCode, text, colors.TextSecondary, "The loaded script module");
		}
		else
		{
			UI::Pill("Status.Scripts", Icons::FileCode, "No scripts", colors.TextDisabled, "No script module is loaded (Scripts > Build Scripts)");
		}

		// Keys a tool holds stay down after it disconnects: show them, with a way out that does not need the tool.
		if (const SimulatedInput& simulated = m_Context.GetSimulatedInput(); simulated.HasHolds())
		{
			ImGui::SameLine();
			if (UI::Pill("Status.InputHolds", Icons::Keyboard, "Holds " + simulated.DescribeHolds(), colors.Warning,
				"Keys and mouse buttons a tool holds down in the running game (input.* commands). They stay down until the tool releases "
				"them, also after it disconnected, or until play stops. Click to release them all."))
			{
				RunEditorCommand(m_Context, m_Commands, "input.releaseAll", { { "wait", false } });
			}
		}

		// Unread errors.
		const ConsolePanel* console = m_Panels.Get<ConsolePanel>(EditorPanels::c_Console);
		if (const uint32_t errors = console ? console->GetUnreadErrors() : 0; errors > 0)
		{
			ImGui::SameLine();
			showConsole |= UI::Pill("Status.Errors", Icons::CircleAlert, fmt::format("{} {}", errors, errors == 1 ? "error" : "errors"), colors.Error,
				"Errors logged since the Console was last viewed: click to open it");
		}
		if (showConsole)
			m_Panels.Focus(EditorPanels::c_Console);
	}

	void EditorLayer::DrawUnsavedChangesModal()
	{
		if (m_OpenUnsavedChangesModal)
		{
			ImGui::OpenPopup("Unsaved Changes");
			m_OpenUnsavedChangesModal = false;
		}
		if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		ImGui::Text("Save changes to '%s'?", m_Context.GetEditScene()->GetName().c_str());
		ImGui::Spacing();
		if (ImGui::Button("Save", ImVec2(100.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			if (SaveScene() && m_PendingDiscardAction)
				m_PendingDiscardAction();
			m_PendingDiscardAction = nullptr;
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't Save", ImVec2(100.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			if (m_PendingDiscardAction)
				m_PendingDiscardAction();
			m_PendingDiscardAction = nullptr;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
			m_PendingDiscardAction = nullptr;
		}
		ImGui::EndPopup();
	}

}
