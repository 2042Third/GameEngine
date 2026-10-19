#pragma once

namespace Strata::Tests
{

	// The editor launch tests start the test executable itself as the "editor": with the environment variable
	// STRATA_TEST_FAKE_EDITOR=1 and at least one, and only, editor launch arguments ([--project <dir>] [--headless]
	// [--no-gpu] [--idle-timeout <s>]), main() runs a minimal editor instead of the tests. STRATA_TEST_FAKE_EDITOR=silent
	// makes it an editor that never becomes reachable (no session; it sleeps for its lifetime). It serves automation on loopback with a fresh token (methods editor.info,
	// editor.status and editor.quit), publishes its session files like the real editor, and exits on editor.quit or after a minute. Its
	// editor.status reports an asset scan as STRATA_TEST_FAKE_SCAN says: "<milliseconds>" long from the start, or "stall" (stuck),
	// and, in its sceneLoad section, a start scene's load "<milliseconds>" long from the start as STRATA_TEST_FAKE_SCENE_LOAD says.
	bool IsFakeEditorLaunch(int argc, char** argv);
	int RunFakeEditor(int argc, char** argv);

}
