#include "stpch.h"
#include "Strata/ImGui/ImGuiLayer.h"

#include "Strata/Core/Application.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace Strata
{

	ImGuiLayer::ImGuiLayer(std::filesystem::path layoutFile)
		: Layer("ImGuiLayer"), m_LayoutFile(std::move(layoutFile))
	{
	}

	void ImGuiLayer::OnAttach()
	{
		Window* window = Application::Get().GetWindow();
		if (!window || !Renderer::IsInitialized())
		{
			ST_CORE_ERROR("ImGuiLayer requires a window and an initialized renderer");
			return;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		if (m_LayoutFile.empty())
		{
			io.IniFilename = nullptr;
		}
		else
		{
			FileSystem::CreateDirectories(m_LayoutFile.parent_path());
			m_LayoutFileUTF8 = FileSystem::ToUTF8(m_LayoutFile);
			io.IniFilename = m_LayoutFileUTF8.c_str();
		}

		const float contentScale = window->GetContentScale();
		ImGuiStyle& style = ImGui::GetStyle();
		ImGui::StyleColorsDark();
		SetDarkThemeColors();
		style.WindowRounding = 2.0f;
		style.FrameRounding = 2.0f;
		style.ScaleAllSizes(contentScale);
		style.FontScaleDpi = contentScale;

		if (!m_Renderer.Init(Renderer::GetDevice(), Renderer::GetShaderLibrary()))
		{
			ST_CORE_ERROR("ImGuiLayer: failed to initialize the ImGui renderer; ImGui is disabled");
			ImGui::DestroyContext();
			return;
		}
		ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(window->GetNativeWindow()), true);
		m_CommandList = Renderer::GetDevice()->createCommandList();
		m_Initialized = true;
	}

	void ImGuiLayer::OnDetach()
	{
		if (!m_Initialized)
			return;

		Renderer::GetGraphicsDevice().WaitForIdle();
		m_CommandList = nullptr;
		m_Renderer.Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		m_Initialized = false;
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		if (!m_Initialized || !m_BlockEvents)
			return;

		const ImGuiIO& io = ImGui::GetIO();
		event.Handled |= event.IsInCategory(EventCategoryMouse) && io.WantCaptureMouse;
		event.Handled |= event.IsInCategory(EventCategoryKeyboard) && io.WantCaptureKeyboard;
	}

	void ImGuiLayer::Begin()
	{
		if (!m_Initialized)
			return;

		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiLayer::End(nvrhi::IFramebuffer* framebuffer)
	{
		if (!m_Initialized)
			return;

		ImGui::Render();
		if (!framebuffer)
			return;

		// Drawn over whatever the layers rendered into the framebuffer this frame.
		m_CommandList->open();
		m_Renderer.Render(m_CommandList, ImGui::GetDrawData(), framebuffer);
		m_CommandList->close();
		Renderer::GetDevice()->executeCommandList(m_CommandList);
	}

	void ImGuiLayer::SetDarkThemeColors()
	{
		ImVec4* colors = ImGui::GetStyle().Colors;
		colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.105f, 0.11f, 1.0f);

		// Headers
		colors[ImGuiCol_Header] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		// Buttons
		colors[ImGuiCol_Button] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		// Frame backgrounds
		colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);

		// Tabs
		colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.3805f, 0.381f, 1.0f);
		colors[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.2805f, 0.281f, 1.0f);
		colors[ImGuiCol_TabDimmed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);

		// Titles
		colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
	}

}
