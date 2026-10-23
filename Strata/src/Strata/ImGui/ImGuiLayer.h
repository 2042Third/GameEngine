#pragma once

#include "Strata/Core/Layer.h"
#include "Strata/ImGui/ImGuiPlatform.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <string>

struct ImGuiStyle;

namespace Strata
{

	class Window;

	// Styles ImGui for a UI scale: colors and sizes (paddings, spacings, roundings) multiplied by the scale. It receives
	// ImGui's dark style at its unscaled defaults; fonts are scaled separately (ImGuiStyle::FontScaleDpi).
	using ImGuiStyleCallback = std::function<void(ImGuiStyle& style, float scale)>;

	// Owns the Dear ImGui context for a windowed application: input and platform services from the application's window
	// (ImGuiPlatform, fed with the window's events), rendering through NVRHI (ImGuiRenderer). The application calls Begin()
	// before and End() after the layers' OnImGuiRender().
	//
	// The UI follows the window's content scale (DPI): on attach, on every WindowContentScaleEvent and when the window's
	// pixels per unit change, the style is rebuilt for the UI scale (ToUIScale: the content scale less what window
	// coordinates in points apply already; ImGui's dark style, or the application's style callback) and the fonts' DPI
	// scale is set to it.
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
		// A UI scale that replaces the content scale (0: follow the content scale again), e.g. a command line's. Clamped and
		// validated like SetContentScale.
		void SetContentScaleOverride(float scale);
		// A factor of the content scale the UI is drawn at while no override is set (a person's choice relative to the
		// display's, 1.25 for 125%), so the UI keeps it on every display. 1 by default; zero, negative or non-finite factors
		// are ignored, and the product is clamped to [c_MinScale, c_MaxScale].
		void SetContentScaleFactor(float factor);
		// The scale the UI is styled for: the override, else the content scale times its factor.
		float GetUIScale() const;
		// The UI scale of a window's content scale: what the window system does not apply to window coordinates already.
		// Where windows are measured in points (macOS, Wayland: a Retina window's framebuffer has twice its size in pixels),
		// ImGui lays out in points and draws its fonts at the framebuffer's density, so the content scale of 2 is there
		// once without scaling the sizes; where windows are measured in pixels (Windows, X11) it is the content scale.
		static float ToUIScale(float contentScale, const glm::uvec2& windowSize, const glm::uvec2& framebufferSize);

		static constexpr float c_MinScale = 0.5f;
		static constexpr float c_MaxScale = 4.0f;
	private:
		// Rebuilds the style of the current context for the UI scale.
		void ApplyStyle();
	private:
		ImGuiPlatform m_Platform;
		ImGuiRenderer m_Renderer;
		nvrhi::CommandListHandle m_CommandList;
		std::filesystem::path m_LayoutFile;
		std::string m_LayoutFileUTF8;
		ImGuiStyleCallback m_StyleCallback;
		Window* m_Window = nullptr; // While attached
		float m_ContentScale = 1.0f;
		float m_ContentScaleOverride = 0.0f;
		float m_ContentScaleFactor = 1.0f;
		bool m_BlockEvents = true;
		bool m_Initialized = false;
	};

}
