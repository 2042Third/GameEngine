#include "EditorLayer.h"

#include "UI/FileDialogs.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageWriter.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace Strata
{

	EditorLayer::EditorLayer(const EditorOptions& options)
		: Layer("EditorLayer"), m_Options(options), m_ShowImGuiDemo(options.ShowImGuiDemo)
	{
	}

	void EditorLayer::OnAttach()
	{
		if (!m_Options.Headless)
			FileDialogs::Init();
		m_ContentBrowser.SetOpenSceneHandler([this](AssetHandle scene)
		{
			RequestDiscardChanges([this, scene]() { RunEditorCommand(m_Context, m_Commands, "scene.open", { { "scene", UUIDToJson(scene) } }); });
		});
		if (!m_Options.ProjectPath.empty())
		{
			std::string error;
			if (!m_Context.OpenProject(m_Options.ProjectPath, &error))
				ST_ERROR("Could not open the project '{}': {}", FileSystem::ToUTF8(m_Options.ProjectPath), error);
		}
		// A failed startup script fails the process, so scripted runs (CI, automation) notice.
		if (!m_Options.CommandScript.empty() && !RunCommandScript(m_Options.CommandScript))
			Application::Get().SetExitCode(1);
	}

	bool EditorLayer::RunCommandScript(const std::filesystem::path& path)
	{
		std::optional<std::string> text = FileSystem::ReadText(path);
		std::string error;
		std::optional<nlohmann::json> script = text ? JsonUtils::Parse(*text, &error) : std::nullopt;
		if (!script || !script->is_array())
		{
			ST_ERROR("Command script '{}' must be a JSON array of {{\"command\": ..., \"parameters\": {{...}}}}: {}", FileSystem::ToUTF8(path),
				text ? error : "cannot read the file");
			return false;
		}
		bool success = true;
		for (const nlohmann::json& step : *script)
		{
			const std::string name = step.is_object() ? JsonUtils::GetString(step, "command") : std::string();
			const nlohmann::json* parameters = step.is_object() ? JsonUtils::Find(step, "parameters") : nullptr;
			const EditorCommandResult result = m_Commands.Execute(m_Context, name, parameters ? *parameters : nlohmann::json::object());
			if (result.Success)
			{
				ST_INFO("{} -> {}", name, result.Value.is_null() ? std::string("ok") : JsonUtils::Dump(result.Value));
			}
			else
			{
				ST_ERROR("{} failed: {}", name.empty() ? std::string("(missing \"command\")") : name, result.Error);
				success = false;
			}
		}
		return success;
	}

	void EditorLayer::OnDetach()
	{
		m_Context.CloseProject();
		FileDialogs::Shutdown();
	}

	void EditorLayer::OnUpdate(Timestep timestep)
	{
		m_Context.Update(timestep);
		UpdateWindowTitle();

		Application& application = Application::Get();
		const bool lastFrame = m_Options.MaxFrames && application.GetFrameCount() + 1 == *m_Options.MaxFrames;
		if (lastFrame && !m_Options.ScreenshotPath.empty())
		{
			application.RequestBackBufferCapture([path = m_Options.ScreenshotPath](const ReadbackImage& image)
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
		Window* window = Application::Get().GetWindow();
		if (!window)
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
			window->SetTitle(title);
			m_WindowTitle = std::move(title);
		}
	}

	void EditorLayer::OnImGuiRender()
	{
		HandleShortcuts();
		DrawDockspace();
		DrawToolbar();
		DrawViewport();
		m_Hierarchy.OnImGuiRender(m_Context, m_Commands);
		m_Inspector.OnImGuiRender(m_Context, m_Commands);
		m_ContentBrowser.OnImGuiRender(m_Context, m_Commands);
		m_Console.OnImGuiRender();
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
			RequestDiscardChanges([]() { Application::Get().Close(); });
			return true;
		});
		dispatcher.Dispatch<WindowFileDropEvent>([this](WindowFileDropEvent& drop)
		{
			if (!m_Context.HasProject())
			{
				ST_WARN("Open or create a project before importing files");
				return true;
			}
			m_ContentBrowser.ImportFiles(m_Context, m_Commands, drop.GetPaths());
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
		if (ImGui::Shortcut(ImGuiKey_Delete, global))
			DeleteSelection();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, global))
		{
			if (m_Context.IsPlaying())
				m_Context.Stop();
			else
				RunEditorCommand(m_Context, m_Commands, "play.start");
		}
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
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

		ImGuiID center = dockspaceId;
		const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
		const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
		const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.3f, nullptr, &center);
		const ImGuiID top = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.05f, nullptr, &center);
		if (ImGuiDockNode* toolbar = ImGui::DockBuilderGetNode(top))
			toolbar->LocalFlags |= static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoTabBar) | static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoResize);

		ImGui::DockBuilderDockWindow("Hierarchy", left);
		ImGui::DockBuilderDockWindow("Inspector", right);
		ImGui::DockBuilderDockWindow("Content Browser", bottom);
		ImGui::DockBuilderDockWindow("Console", bottom);
		ImGui::DockBuilderDockWindow("Toolbar", top);
		ImGui::DockBuilderDockWindow("Viewport", center);
		ImGui::DockBuilderFinish(dockspaceId);
	}

	void EditorLayer::DrawDockspace()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float statusBarHeight = ImGui::GetFrameHeight();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
			| ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("EditorDockspace", nullptr, windowFlags);
		ImGui::PopStyleVar(3);

		DrawMenuBar();
		const ImGuiID dockspaceId = ImGui::GetID("EditorDockspaceID");
		// Without a saved arrangement (first run) the panels get the default one; checked once, so a user who undocks
		// every panel keeps that choice.
		if (!m_LayoutChecked)
		{
			const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
			m_ResetLayout |= !node || (node->IsLeafNode() && node->Windows.Size == 0);
			m_LayoutChecked = true;
		}
		if (m_ResetLayout)
		{
			BuildDefaultLayout(dockspaceId);
			m_ResetLayout = false;
		}
		ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
		ImGui::End();

		// The status bar is a separate strip below the dock space.
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
		ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
		ImGui::Begin("StatusBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing);
		ImGui::PopStyleVar(2);
		DrawStatusBar();
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
				RequestDiscardChanges([]() { Application::Get().Close(); });
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

		if (ImGui::BeginMenu("Entity"))
		{
			SceneHierarchyPanel::DrawCreateMenu(m_Context, m_Commands, UUID::Null());
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Window"))
		{
			if (ImGui::MenuItem("Reset Layout"))
				m_ResetLayout = true;
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	void EditorLayer::DrawToolbar()
	{
		ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		const SceneState state = m_Context.GetSceneState();
		const float width = ImGui::GetFrameHeight() * 4.0f;
		ImGui::SetCursorPosX(std::max((ImGui::GetContentRegionAvail().x - width * 4.0f) * 0.5f, 0.0f));

		if (state == SceneState::Edit)
		{
			if (ImGui::Button("Play", ImVec2(width, 0.0f)))
				RunEditorCommand(m_Context, m_Commands, "play.start");
			ImGui::SameLine();
			if (ImGui::Button("Simulate", ImVec2(width, 0.0f)))
				RunEditorCommand(m_Context, m_Commands, "play.simulate");
		}
		else
		{
			if (ImGui::Button("Stop", ImVec2(width, 0.0f)))
				m_Context.Stop();
			ImGui::SameLine();
			const bool paused = m_Context.IsPaused();
			if (ImGui::Button(paused ? "Resume" : "Pause", ImVec2(width, 0.0f)))
				m_Context.SetPaused(!paused);
			ImGui::SameLine();
			ImGui::BeginDisabled(!paused);
			if (ImGui::Button("Step", ImVec2(width, 0.0f)))
				m_Context.Step();
			ImGui::EndDisabled();
		}
		ImGui::End();
	}

	void EditorLayer::DrawViewport()
	{
		ImGui::Begin("Viewport");
		ImGui::TextDisabled("Scene: %s (%zu entities)", m_Context.GetActiveScene()->GetName().c_str(), m_Context.GetActiveScene()->GetEntityCount());
		ImGui::End();
	}

	void EditorLayer::DrawStatusBar()
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", SceneStateToString(m_Context.GetSceneState()));
		ImGui::SameLine();
		ImGui::TextDisabled("|  %.1f FPS", ImGui::GetIO().Framerate);
		if (EditorAssetManager* assets = m_Context.GetAssetManager())
		{
			const AssetManagerStats stats = assets->GetStats();
			ImGui::SameLine();
			ImGui::TextDisabled("|  %u assets loaded, %u loading", stats.LoadedAssets, stats.LoadingAssets);
		}
		if (const uint32_t errors = m_Console.GetUnreadErrors(); errors > 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "|  %u new errors (see Console)", errors);
		}
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
