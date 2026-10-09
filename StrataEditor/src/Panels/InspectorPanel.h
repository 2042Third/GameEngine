#pragma once

#include <string>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	class Entity;

	// Properties of the primary selection, drawn from component reflection: every registered component is editable
	// without inspector code of its own. Property edits are undoable and merge while a value is being dragged.
	class InspectorPanel
	{
	public:
		void OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands);
	private:
		void DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, Entity entity);
	private:
		std::string m_ComponentFilter;
	};

}
