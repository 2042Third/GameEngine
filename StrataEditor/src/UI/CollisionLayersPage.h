#pragma once

#include "UI/SettingsPages.h"

#include <Strata/Scene/PhysicsLayerSettings.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;

	// The Collision Layers page of the Project Settings window (the project section Scene.CollisionLayers): the names of the
	// project's layers, edited in place (a name applies when its field is left or Enter is pressed), and the matrix of which
	// layers collide, compact enough for the layers a game names to fit a laptop's width. Every edit is one
	// project.setCollisionLayers (one undo step); while the project is not ready for edits (EditorContext::IsWaitingForAssets)
	// an edit is refused with the reason. Probe keys, under "ProjectSettings.Scene.CollisionLayers.": Summary, Name.<layer>,
	// Clear.<layer>, Add, Label.<layer>, Cell.<a>.<b> (each pair once, a's row before b's) and Error (why the last edit was
	// refused).
	class CollisionLayersPage final : public SettingsPage
	{
	public:
		static constexpr const char* c_ProbePrefix = "ProjectSettings.Scene.CollisionLayers.";

		void Draw(EditorPanelContext& context, float width) override;

		// What the page and the Inspector say of a table ("2 named · 1 pair passes through") and the layers the matrix shows:
		// layer 0, the named ones and those with a pair that does not collide.
		static std::string Describe(const PhysicsLayerSettings& layers, std::vector<uint32_t>* outShown = nullptr);
	private:
		// The project's table as its settings hold it, read again when they change.
		void Refresh(const EditorContext& context);
		void DrawNames(EditorPanelContext& context);
		void DrawMatrix(EditorPanelContext& context);
		void Run(EditorPanelContext& context, const nlohmann::json& parameters);
	private:
		PhysicsLayerSettings m_Layers;
		uint64_t m_SettingsVersion = 0; // Of the project's settings m_Layers were read from (ProjectSettingsValues::GetVersion)
		bool m_Read = false;
		std::string m_Summary;
		std::vector<uint32_t> m_Shown;
		// The layer name being typed: the text, for the layer whose field it was started in.
		uint32_t m_NameLayer = c_PhysicsLayerCount;
		std::string m_NameText;
		std::string m_Error; // Why the last edit was refused
	};

}
