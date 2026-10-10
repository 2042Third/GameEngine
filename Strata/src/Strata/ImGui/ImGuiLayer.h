#pragma once

#include "Strata/Core/Layer.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include <filesystem>
#include <functional>
#include <string>

struct ImGuiStyle;

namespace Strata
{

	// Styles ImGui for a UI scale: colors and sizes (paddings, spacings, roundings) multiplied by the scale. It receives
	// ImGui's dark style at its unscaled defaults; fonts are scaled separately (ImGuiStyle::FontScaleDpi).
	using ImGuiStyleCallback = std::function<void(ImGuiStyle& style, float scale)>;

	// Owns the Dear ImGui context for a windowed application: platform input through GLFW, rendering through
	// NVRHI. The application calls Begin() before and End() after the layers' OnImGuiRender().
	//
	// The UI follows the window's content scale (DPI): on attach and on every WindowContentScaleEvent the style is rebuilt
	// for the scale (ImGui's dark style, or the application's style callback) and the fonts' DPI scale is set to it.
	// Style changes apply to the current ImGui context: the one OnAttach creates, or one a test created.
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
		// Whether the context, the platform backend and the renderer are up (false before OnAttach and when it failed).
		bool IsInitialized() const { return m_Initialized; }

		// Installs the function that styles the UI (null: ImGui's dark style with its sizes scaled) and restyles the UI.
		void SetStyleCallback(ImGuiStyleCallback callback);
		// The window's content scale (set on attach and by WindowContentScaleEvent): without an override it is the UI scale,
		// so the style is rebuilt for it and the fonts' DPI scale set to it. Scales outside [c_MinScale, c_MaxScale] are
		// clamped; zero, negative or non-finite ones are ignored.
		void SetContentScale(float scale);
		// A UI scale that replaces the content scale (0: follow the content scale again), e.g. a user's choice. Clamped and
		// validated like SetContentScale.
		void SetContentScaleOverride(float scale);
		// The scale the UI is styled for.
		float GetUIScale() const { return m_ContentScaleOverride > 0.0f ? m_ContentScaleOverride : m_ContentScale; }

		static constexpr float c_MinScale = 0.5f;
		static constexpr float c_MaxScale = 4.0f;
	private:
		// Rebuilds the style of the current context for the UI scale.
		void ApplyStyle();
	private:
		ImGuiRenderer m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::filesystem::path m_LayoutFile;
		std::string m_LayoutFileUTF8;
		ImGuiStyleCallback m_StyleCallback;
		float m_ContentScale = 1.0f;
		float m_ContentScaleOverride = 0.0f;
		bool m_BlockEvents = true;
		bool m_Initialized = false;
	};

}
