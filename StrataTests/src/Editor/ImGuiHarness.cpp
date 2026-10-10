#include "Editor/ImGuiHarness.h"

#include "UI/EditorFonts.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/Layer.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Events/Event.h>

#include <imgui_internal.h>

namespace Strata::Tests
{

	ImGuiHarness::ImGuiHarness(const Specification& specification)
	{
		IMGUI_CHECKVERSION();
		m_Context = ImGui::CreateContext();
		ImGui::SetCurrentContext(m_Context);
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		// The harness stands in for a renderer that supports ImGui's texture protocol (see HonorTextureRequests).
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
		io.ConfigDebugHighlightIdConflicts = true;
		io.DisplaySize = specification.DisplaySize;
		io.DeltaTime = c_DeltaTime;

		UI::ItemProbe::Clear();
		m_FontsLoaded = UI::EditorFonts::Load();
		m_ImGuiLayer.SetContentScale(specification.ContentScale);
		m_ImGuiLayer.SetStyleCallback(UI::ApplyTheme);
	}

	ImGuiHarness::~ImGuiHarness()
	{
		UI::ItemProbe::Clear();
		ImGui::DestroyContext(m_Context);
	}

	void ImGuiHarness::Frame(const std::function<void()>& draw, float deltaTime)
	{
		ImGui::GetIO().DeltaTime = deltaTime;
		ImGui::NewFrame();
		draw();
		ImGui::Render();
		HonorTextureRequests();
	}

	void ImGuiHarness::Frame(Layer& layer, float deltaTime)
	{
		layer.OnUpdate(Timestep(deltaTime));
		Frame([&layer]() { layer.OnImGuiRender(); }, deltaTime);
	}

	void ImGuiHarness::DispatchEvent(Event& event, Layer& layer)
	{
		m_ImGuiLayer.OnEvent(event);
		if (!event.Handled)
			layer.OnEvent(event);
	}

	void ImGuiHarness::MoveMouse(const ImVec2& position)
	{
		ImGui::GetIO().AddMousePosEvent(position.x, position.y);
	}

	void ImGuiHarness::SetMouseButton(ImGuiMouseButton button, bool down)
	{
		ImGui::GetIO().AddMouseButtonEvent(button, down);
	}

	void ImGuiHarness::SetKey(ImGuiKey key, bool down)
	{
		ImGui::GetIO().AddKeyEvent(key, down);
	}

	bool ImGuiHarness::ClickItem(std::string_view probeKey, Layer& layer)
	{
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(probeKey);
		if (!item || item->Duplicate || !item->Enabled)
			return false;
		// Hover first (ImGui decides what is hovered from the previous frame), then press and release: buttons act on the
		// release.
		MoveMouse(item->GetCenter());
		Frame(layer);
		SetMouseButton(ImGuiMouseButton_Left, true);
		Frame(layer);
		SetMouseButton(ImGuiMouseButton_Left, false);
		Frame(layer);
		return true;
	}

	int ImGuiHarness::GetHoveredItemIdCount() const
	{
		return GImGui->HoveredIdPreviousFrameItemCount;
	}

	ImGuiID ImGuiHarness::GetPreviouslyHoveredId() const
	{
		return GImGui->HoveredIdPreviousFrame;
	}

	void ImGuiHarness::HonorTextureRequests()
	{
		// What a renderer does with ImGui's texture list after Render, without the GPU: every texture is ready at once.
		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
			{
				if (texture->Status == ImTextureStatus_WantCreate)
					texture->SetTexID(static_cast<ImTextureID>(texture->UniqueID) + 1);
				texture->SetStatus(ImTextureStatus_OK);
				m_TextureRequests++;
			}
			else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
			{
				texture->SetTexID(ImTextureID_Invalid);
				texture->SetStatus(ImTextureStatus_Destroyed);
			}
		}
	}

}
