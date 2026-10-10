#include "stpch.h"
#include "Strata/ImGui/ImGuiLayer.h"

#include "Strata/Core/Application.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Renderer/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Strata
{

	namespace
	{

		std::optional<float> ValidateScale(float scale)
		{
			if (!std::isfinite(scale) || scale <= 0.0f)
				return std::nullopt;
			return std::clamp(scale, ImGuiLayer::c_MinScale, ImGuiLayer::c_MaxScale);
		}

	}

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

		SetContentScale(window->GetContentScale());

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
		// Not handled: other layers may lay out differently at another scale too.
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowContentScaleEvent>([this](WindowContentScaleEvent& scaleEvent)
		{
			SetContentScale(scaleEvent.GetScale());
			return false;
		});

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

	void ImGuiLayer::SetStyleCallback(ImGuiStyleCallback callback)
	{
		m_StyleCallback = std::move(callback);
		ApplyStyle();
	}

	void ImGuiLayer::SetContentScale(float scale)
	{
		const std::optional<float> valid = ValidateScale(scale);
		if (!valid)
		{
			ST_CORE_WARN("ImGuiLayer: ignoring the content scale {}", scale);
			return;
		}
		m_ContentScale = *valid;
		ApplyStyle();
	}

	void ImGuiLayer::SetContentScaleOverride(float scale)
	{
		if (scale == 0.0f)
		{
			m_ContentScaleOverride = 0.0f;
		}
		else if (const std::optional<float> valid = ValidateScale(scale))
		{
			m_ContentScaleOverride = *valid;
		}
		else
		{
			ST_CORE_WARN("ImGuiLayer: ignoring the UI scale {}", scale);
			return;
		}
		ApplyStyle();
	}

	void ImGuiLayer::ApplyStyle()
	{
		if (!ImGui::GetCurrentContext())
			return;

		// Built from the unscaled defaults every time, so scaling twice never compounds.
		const float scale = GetUIScale();
		ImGuiStyle style;
		ImGui::StyleColorsDark(&style);
		if (m_StyleCallback)
			m_StyleCallback(style, scale);
		else
			style.ScaleAllSizes(scale);
		style.FontScaleDpi = scale;
		ImGui::GetStyle() = style;
	}

}
