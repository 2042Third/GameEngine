#pragma once

#include "Strata/Core/Layer.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include <filesystem>

namespace Strata
{

	// Owns the Dear ImGui context for a windowed application: platform input through GLFW, rendering through
	// NVRHI. The application calls Begin() before and End() after the layers' OnImGuiRender().
	class ImGuiLayer : public Layer
	{
	public:
		// layoutFile: where docking layout and window state are persisted (empty disables persistence).
		explicit ImGuiLayer(std::filesystem::path layoutFile = {});
		~ImGuiLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnEvent(Event& event) override;

		void Begin();
		// Renders the UI into the framebuffer (clearing it first) and submits the commands. With a null
		// framebuffer the frame is finalized without rendering.
		void End(nvrhi::IFramebuffer* framebuffer);

		// When enabled (default), mouse/keyboard events ImGui wants to capture are not passed to lower layers.
		void BlockEvents(bool block) { m_BlockEvents = block; }
		void SetDarkThemeColors();
	private:
		ImGuiRenderer m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::filesystem::path m_LayoutFile;
		std::string m_LayoutFileUTF8;
		bool m_BlockEvents = true;
		bool m_Initialized = false;
	};

}
