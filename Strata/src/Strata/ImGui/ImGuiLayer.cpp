#include "stpch.h"
#include "Strata/ImGui/ImGuiLayer.h"

#include "Strata/Core/Application.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Window.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Renderer/Renderer.h"

#include <imgui.h>

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

		m_Window = window;
		SetContentScale(ToUIScale(window->GetContentScale(), glm::uvec2(window->GetWidth(), window->GetHeight()), window->GetFramebufferSize()));

		if (!m_Renderer.Init(Renderer::GetDevice(), Renderer::GetShaderLibrary()))
		{
			ST_CORE_ERROR("ImGuiLayer: failed to initialize the ImGui renderer; ImGui is disabled");
			ImGui::DestroyContext();
			return;
		}
		m_Platform.Init(*window);
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
		m_Platform.Shutdown();
		ImGui::DestroyContext();
		m_Initialized = false;
		m_Window = nullptr;
	}

	void ImGuiLayer::OnEvent(Event& event)
	{
		// Not handled: other layers may lay out differently at another scale too.
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowContentScaleEvent>([this](WindowContentScaleEvent& scaleEvent)
		{
			SetContentScale(m_Window ? ToUIScale(scaleEvent.GetScale(), glm::uvec2(m_Window->GetWidth(), m_Window->GetHeight()), m_Window->GetFramebufferSize())
				: scaleEvent.GetScale());
			return false;
		});

		if (!m_Initialized)
			return;
		// The overlay sees events first: ImGui's input comes from them.
		m_Platform.OnEvent(event);
		if (!m_BlockEvents)
			return;

		const ImGuiIO& io = ImGui::GetIO();
		event.Handled |= event.IsInCategory(EventCategoryMouse) && io.WantCaptureMouse;
		event.Handled |= event.IsInCategory(EventCategoryKeyboard) && io.WantCaptureKeyboard;
	}

	void ImGuiLayer::Begin()
	{
		if (!m_Initialized)
			return;

		// The window's size in pixels may follow its content scale event (moved to a display of another density): the scale
		// is checked again once both are known (not while the window is minimized and has no size).
		const glm::uvec2 windowSize(m_Window->GetWidth(), m_Window->GetHeight());
		const glm::uvec2 framebufferSize = m_Window->GetFramebufferSize();
		if (windowSize.x > 0 && framebufferSize.x > 0)
		{
			const std::optional<float> scale = ValidateScale(ToUIScale(m_Window->GetContentScale(), windowSize, framebufferSize));
			if (scale && *scale != m_ContentScale)
				SetContentScale(*scale);
		}

		m_Platform.NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiLayer::End(nvrhi::IFramebuffer* framebuffer)
	{
		if (!m_Initialized)
			return;

		ImGui::Render();
		if (!framebuffer)
			return;

		// Drawn over whatever the layers rendered into the framebuffer this frame. Its GPU stage is registered here, by the
		// first program that draws UI, so programs without ImGui (games) get no empty column for it.
		static const FrameStageID s_GPUStage = FrameProfiler::Get().RegisterStage("GPU.ImGui", false, FrameStageKind::GPU);
		m_CommandList->open();
		GPUPassTimer timer(m_CommandList.Get(), s_GPUStage);
		m_Renderer.Render(m_CommandList, ImGui::GetDrawData(), framebuffer);
		timer.End();
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

	float ImGuiLayer::ToUIScale(float contentScale, const glm::uvec2& windowSize, const glm::uvec2& framebufferSize)
	{
		if (windowSize.x == 0 || framebufferSize.x == 0)
			return contentScale;
		const float pixelsPerUnit = static_cast<float>(framebufferSize.x) / static_cast<float>(windowSize.x);
		if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 1.0f)
			return contentScale;
		return contentScale / pixelsPerUnit;
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

	void ImGuiLayer::SetContentScaleFactor(float factor)
	{
		if (!std::isfinite(factor) || factor <= 0.0f)
		{
			ST_CORE_WARN("ImGuiLayer: ignoring the UI scale factor {}", factor);
			return;
		}
		if (factor == m_ContentScaleFactor)
			return;
		m_ContentScaleFactor = factor;
		ApplyStyle();
	}

	float ImGuiLayer::GetUIScale() const
	{
		if (m_ContentScaleOverride > 0.0f)
			return m_ContentScaleOverride;
		return std::clamp(m_ContentScale * m_ContentScaleFactor, c_MinScale, c_MaxScale);
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
