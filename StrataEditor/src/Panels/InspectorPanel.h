#pragma once

#include "Editor/PropertyEdit.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/MaterialEditor.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Core/UUID.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Scene.h>

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
	private:
		void DrawHeader(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities);
		void DrawComponent(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities, const ComponentInfo& info);
		void DrawComponentMenu(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities, const ComponentInfo& info);
		void DrawProperties(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info);
		// An edit of a rotation shared by several entities: continues the edit in progress, or begins one from the angles the
		// widget showed (RotationAnglesEdit keeps each entity's own angles through a drag).
		bool UpdateRotationEdit(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const PropertyInfo& property,
			const PropertyEditResult& result, std::string* outError);
		void DrawMaterialSection(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities);
		// While the scene plays: what the entity's animator plays (state, time, blending) and its parameters, read-only.
		void DrawAnimatorState(EditorContext& context, Entity entity);
		void DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities);
		// The scripts of the entity: fields edited through the same undoable path as script.setField, scripts added and
		// removed through script.add/script.remove. Without a loaded module the stored overrides are shown read-only.
		void DrawScripts(EditorContext& context, const EditorCommandRegistry& commands, Entity entity);
		void DrawSceneSettings(EditorContext& context, const EditorCommandRegistry& commands);
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
		std::unordered_map<const ComponentInfo*, std::vector<std::optional<PropertyValue>>> m_Defaults;
		MaterialEditor m_MaterialEditor;
		// The rotation of several entities being edited, until the widget reports the edit finished.
		std::optional<RotationAnglesEdit> m_RotationEdit;
	};

}
