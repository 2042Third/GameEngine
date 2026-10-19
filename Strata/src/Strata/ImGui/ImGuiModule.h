#pragma once

#include "Strata/Core/EngineModule.h"

#include <array>
#include <filesystem>
#include <string_view>

namespace Strata
{

	class GraphicsModule;
	class ImGuiLayer;

	struct ImGuiModuleSpecification
	{
		std::filesystem::path LayoutFile; // Where ImGui keeps its docking layout and window state (empty: not kept)
	};

	// The engine module of Dear ImGui (editors and tools; games have none): it pushes the ImGuiLayer as the top overlay
	// (so it sees events before the program's layers) and draws every frame's UI in the Editor.UI stage: the layers'
	// OnImGuiRender, then the UI over the back buffer. It needs the Graphics module and a window.
	class ImGuiModule final : public EngineModule
	{
	public:
		static constexpr std::string_view c_Name = "ImGui";

		explicit ImGuiModule(ImGuiModuleSpecification specification = {});

		static EngineModuleFactory Factory(ImGuiModuleSpecification specification = {});

		std::string_view GetName() const override { return c_Name; }
		std::span<const EngineModuleDependency> GetDependencies() const override { return c_Dependencies; }
		[[nodiscard]] bool OnInit(EngineContext& context) override;
		void OnRenderUI(EngineFrame& frame) override;
		void OnRelease() override;

		// The layer (theme, fonts, UI scale); null before the module started and once the layers are gone.
		ImGuiLayer* GetLayer() const { return m_Layer; }
	private:
		static constexpr std::array<EngineModuleDependency, 1> c_Dependencies = { EngineModuleDependency { "Graphics" } };

		ImGuiModuleSpecification m_Specification;
		GraphicsModule* m_Graphics = nullptr;
		ImGuiLayer* m_Layer = nullptr; // Owned by the application's layer stack
	};

}
