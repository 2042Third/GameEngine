#pragma once

#include "UI/EditorPanelRegistry.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Strata
{

	class Project;

	// The offer of recovery data (SceneRecovery): unsaved scene changes an editor left behind when it ended without closing
	// the scene, or that were discarded and kept anyway. It opens by itself once per opened project whose recovery files
	// include changes that still apply, and from File > Recover Unsaved Changes. Each entry offers Recover (scene.recover
	// "apply", after the question about the edited scene's own unsaved changes) and Discard ("discard"); Later closes it.
	// Drawn by EditorLayer. Main thread only.
	class RecoveryDialog
	{
	public:
		static constexpr const char* c_Popup = "Recover Unsaved Changes";

		// Opens the dialog at the next Draw.
		void Open() { m_OpenRequested = true; }
		// Every frame, inside the ImGui frame.
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Open; }
		// Whether the open project has recovery data to offer (the File menu item).
		static bool HasOffer(const EditorContext& context);
	private:
		// Runs scene.recover for an entry through the runner; failures show in the dialog.
		void Run(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner, const std::string& id, const char* action,
			bool force);
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		std::weak_ptr<Project> m_OfferedFor; // The opened project the dialog has looked at (and opened by itself for, if it had an offer)
		std::string m_Error;
		uint64_t m_Running = 0; // scene.recover commands this dialog started that have not finished
	};

}
