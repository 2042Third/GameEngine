#include "EditorLayer.h"

#include <Strata/Renderer/ImageWriter.h>

#include <imgui.h>

namespace Strata
{

	EditorLayer::EditorLayer(const EditorOptions& options)
		: Layer("EditorLayer"), m_Options(options), m_ShowImGuiDemo(options.ShowImGuiDemo)
	{
	}

	void EditorLayer::OnAttach()
	{
	}

	void EditorLayer::OnDetach()
	{
	}

	void EditorLayer::OnUpdate(Timestep)
	{
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

	void EditorLayer::OnImGuiRender()
	{
		DrawDockspace();
		if (m_ShowImGuiDemo)
			ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
	}

	void EditorLayer::OnEvent(Event&)
	{
	}

	void EditorLayer::DrawDockspace()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("EditorDockspace", nullptr, windowFlags);
		ImGui::PopStyleVar(3);

		ImGui::DockSpace(ImGui::GetID("EditorDockspaceID"), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
		DrawMenuBar();
		ImGui::End();
	}

	void EditorLayer::DrawMenuBar()
	{
		if (!ImGui::BeginMenuBar())
			return;

		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Exit"))
				Application::Get().Close();
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

}
