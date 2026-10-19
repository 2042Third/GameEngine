#pragma once

#include "Editor/PropertyEdit.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/MaterialEditor.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Core/UUID.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;

	// The selected entities, drawn from component reflection: every registered component is editable without inspector code
	// of its own.
	// - A header with the entity's icon, its name (renamed in place), its active toggle and its ID (with a copy button); for
	//   several entities their count and a shared active toggle.
	// - One section per component that every selected entity has. Property edits are undoable and merge while a value is
	//   dragged; with several entities one edit changes all of them in one step, values that differ show as mixed, and only
	//   the components (axes) an edit changes are written. Vectors and rotations have axis labels that reset their axis.
	//   A section's menu (its ... button or a right click on its header) has Reset, Copy Values and Paste Values (through
	//   the clipboard, component.set: one step for all entities) for components with settable properties, and Remove,
	//   which components the engine adds do not offer; a prefab link, with neither, has no menu.
	// - The mesh renderer's material: New Material (material.create, then component.set) and the material's properties in
	//   place (shared by every entity that uses it, see MaterialEditor).
	// - The Script component has its own drawer: the attached script classes with their fields (from the loaded module).
	// - With nothing selected: the scene's settings (scene.setSettings, one step per edit) and its active sky.
	// - While playing, a banner says that changes are discarded when play stops.
	class InspectorPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;
		// How often the Overrides menu walked an instance for its rows (tests): once per list, not per frame.
		uint64_t GetOverrideRowsBuildCount() const { return m_OverrideRowsBuilds; }
	private:
		// The selected entities' components, or the scene's settings with nothing selected.
		// A single selected entity of a prefab instance: its prefab entity id, its instance's overrides and whether the prefab is
		// an imported model (which is never applied to).
		// Copies: commands the Inspector runs while it draws may move or remake the instance's components.
		struct PrefabView
		{
			UUID Root = UUID::Null();
			UUID SceneEntity = UUID::Null();
			UUID PrefabEntity = UUID::Null();
			AssetHandle Prefab = UUID::Null();
			std::vector<PrefabOverride> Overrides;
			bool Model = false;

			bool IsOverridden(std::string_view component, std::string_view property = {}) const;
		};
		std::optional<PrefabView> FindPrefabView(std::span<const Entity> entities) const;
		// prefab.apply and prefab.revert chosen in the Inspector's menus run once the selection is drawn: they can remake the
		// instance's entities, which the rest of the frame's drawing still reads.
		void SchedulePrefabCommand(std::string name, nlohmann::json parameters);

		void DrawSelection(EditorPanelContext& panelContext);
		void DrawHeader(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities);
		// Under the header for an entity of a prefab instance: the prefab (a click inspects it), Select (the instance's root),
		// Open (the prefab in the Asset tab and the Content Browser) and the Overrides menu (Apply All, Revert All, each
		// override).
		void DrawPrefabBar(EditorPanelContext& panelContext, const PrefabView& view);
		void DrawComponent(EditorPanelContext& panelContext, std::span<const Entity> entities, const ComponentInfo& info, const PrefabView* prefab);
		void DrawComponentMenu(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities, const ComponentInfo& info,
			const PrefabView* prefab);
		void DrawProperties(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info,
			const PrefabView* prefab);
		// An edit of a rotation shared by several entities: continues the edit in progress, or begins one from the angles the
		// widget showed (RotationAnglesEdit keeps each entity's own angles through a drag).
		bool UpdateRotationEdit(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const PropertyInfo& property,
			const PropertyEditResult& result, std::string* outError);
		void DrawMaterialSection(EditorPanelContext& panelContext, std::span<const Entity> entities);
		// While the scene plays: what the entity's animator plays (state, time, blending) and its parameters, read-only.
		void DrawAnimatorState(EditorContext& context, Entity entity);
		void DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities);
		// The scripts of the entity: fields edited through the same undoable path as script.setField, scripts added and
		// removed through script.add/script.remove. Without a loaded module the stored overrides are shown read-only.
		// The entity's scripts and their fields; an instance's overridden fields are marked (PropertyLabelOptions::Overridden)
		// with Apply to Prefab and Revert on a right-click.
		void DrawScripts(EditorContext& context, const EditorCommandRegistry& commands, Entity entity, const PrefabView* prefab);
		void DrawSceneSettings(EditorContext& context, const EditorCommandRegistry& commands);
		// The scene's collision layers: their names and the matrix of which collide (scene.setSettings, one step per edit).
		void DrawCollisionLayers(EditorContext& context, const EditorCommandRegistry& commands);
		void DrawSky(EditorContext& context, const EditorCommandRegistry& commands);
		// The values new components of the type have (what axis labels reset to), computed once per type.
		const std::vector<std::optional<PropertyValue>>& GetDefaults(const ComponentInfo& info);
	private:
		std::string m_ComponentFilter;
		// The header's name field: the text being typed for the entity it was started on.
		UUID m_NameEntity = UUID::Null();
		std::string m_NameText;
		// Scene settings being dragged: applied with one scene.setSettings when the drag ends.
		std::optional<SceneSettings> m_SettingsDraft;
		// The physics section's summary, formatted again only when the values it shows change.
		struct PhysicsSummary
		{
			glm::vec3 Gravity = glm::vec3(0.0f);
			float FixedTimestep = 0.0f;
			std::string Text;
		};
		std::optional<PhysicsSummary> m_PhysicsSummary;
		// The streaming section's summary, likewise.
		struct StreamingSummary
		{
			uint32_t MaxEntitiesInstantiatedPerFrame = 0;
			std::string Text;
		};
		std::optional<StreamingSummary> m_StreamingSummary;
		// The collision layers section's summary and the layers its matrix shows (layer 0, the named ones and those with a pair
		// that does not collide), computed again only when the layers change.
		struct LayersSummary
		{
			PhysicsLayerSettings Layers;
			std::string Text;
			std::vector<uint32_t> Shown;
		};
		std::optional<LayersSummary> m_LayersSummary;
		// The layer name being typed: the text, for the layer whose field it was started in.
		uint32_t m_LayerNameLayer = c_PhysicsLayerCount;
		std::string m_LayerNameText;
		std::unordered_map<const ComponentInfo*, std::vector<std::optional<PropertyValue>>> m_Defaults;
		MaterialEditor m_MaterialEditor;
		// The rotation of several entities being edited, until the widget reports the edit finished.
		std::optional<RotationAnglesEdit> m_RotationEdit;
		struct PendingCommand
		{
			std::string Name;
			nlohmann::json Parameters;
		};
		std::optional<PendingCommand> m_PendingPrefabCommand;
		// The Overrides menu's rows: each override's entity in the scene and label, found by walking the instance once for
		// the list they were made from, and again only when the history, the hierarchy or the instance change.
		struct OverrideRow
		{
			UUID Target = UUID::Null(); // Null: the instance removed the entity
			std::string Label;
			nlohmann::json Parameters;  // prefab.apply and prefab.revert of the override alone
		};
		struct OverrideRows
		{
			const Scene* ForScene = nullptr;
			UUID Root = UUID::Null();
			uint64_t UndoVersion = UINT64_MAX;
			uint64_t HierarchyVersion = UINT64_MAX;
			std::vector<PrefabOverride> Overrides;
			std::vector<OverrideRow> Rows;
		};
		OverrideRows m_OverrideRows;
		uint64_t m_OverrideRowsBuilds = 0;
		const std::vector<OverrideRow>& GetOverrideRows(EditorContext& context, const PrefabView& view);
	};

}
