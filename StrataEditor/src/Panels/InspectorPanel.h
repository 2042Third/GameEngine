#pragma once

#include <string>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	class Entity;

	// Properties of the primary selection, drawn from component reflection: every registered component is editable
	// without inspector code of its own. Property edits are undoable and merge while a value is being dragged. The Script
	// component has its own drawer: the attached script classes with their fields (from the loaded module).
	class InspectorPanel
	{
	public:
		void OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands);
	private:
		void DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, Entity entity);
		// The scripts of the entity: fields edited through the same undoable path as script.setField, scripts added and
		// removed through script.add/script.remove. Without a loaded module the stored overrides are shown read-only.
		void DrawScripts(EditorContext& context, const EditorCommandRegistry& commands, Entity entity);
	private:
		std::string m_ComponentFilter;
	};

}
