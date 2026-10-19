#include "stpch.h"
#include "Strata/ImGui/ImGuiModule.h"

#include "Strata/Core/Application.h"
#include "Strata/Core/LayerStack.h"
#include "Strata/Core/Profiling.h"
#include "Strata/ImGui/ImGuiLayer.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/GraphicsModule.h"
#include "Strata/Renderer/Renderer.h"

namespace Strata
{

	ImGuiModule::ImGuiModule(ImGuiModuleSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	EngineModuleFactory ImGuiModule::Factory(ImGuiModuleSpecification specification)
	{
		return [specification]() -> Scope<EngineModule> { return CreateScope<ImGuiModule>(specification); };
	}

	bool ImGuiModule::OnInit(EngineContext& context)
	{
		m_Graphics = context.Modules.Find<GraphicsModule>();
		if (!context.MainWindow || !m_Graphics || !m_Graphics->GetDevice())
		{
			ST_CORE_ERROR("The ImGui module needs a window and the Graphics module");
			m_Graphics = nullptr;
			return false;
		}
		// The layer stack owns it from here on; a layer that could not initialize stays there inert (its Begin and End do
		// nothing) until the program, which cannot run, ends.
		ImGuiLayer* layer = new ImGuiLayer(m_Specification.LayoutFile);
		context.App.PushOverlay(layer);
		if (!layer->IsInitialized())
		{
			m_Graphics = nullptr;
			return false;
		}
		m_Layer = layer;
		return true;
	}

	void ImGuiModule::OnRenderUI(EngineFrame& frame)
	{
		ST_PROFILE_STAGE("Editor.UI");
		m_Layer->Begin();
		for (Layer* layer : frame.Layers)
			layer->OnImGuiRender();

		// The back buffer: the GPU waits for the display only from here on, after the frame's uploads and offscreen rendering.
		Renderer::PrepareBackBuffer();
		m_Layer->End(m_Graphics->GetDevice()->GetBackBufferFramebuffer());
	}

	void ImGuiModule::OnRelease()
	{
		// The application deleted the layer with the others.
		m_Layer = nullptr;
		m_Graphics = nullptr;
	}

}
