#pragma once

#include <Strata.h>

#include <filesystem>
#include <optional>

namespace Strata
{

	struct EditorOptions
	{
		std::filesystem::path ProjectPath;
		std::filesystem::path ScreenshotPath; // Saves the editor window to this PNG on the last frame (with --frames)
		std::optional<uint64_t> MaxFrames;
		bool ShowImGuiDemo = false;
	};

	class EditorLayer : public Layer
	{
	public:
		explicit EditorLayer(const EditorOptions& options);
		~EditorLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;
	private:
		void DrawDockspace();
		void DrawMenuBar();
	private:
		EditorOptions m_Options;
		bool m_ShowImGuiDemo = false;
	};

}
