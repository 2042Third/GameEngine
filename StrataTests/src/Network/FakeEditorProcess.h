#pragma once

namespace Strata::Tests
{

	// The editor launch tests start the test executable itself as the "editor": with the environment variable
	// STRATA_TEST_FAKE_EDITOR=1 and editor-style arguments ("--project <dir>" [--headless]), main() runs a minimal
	// editor instead of the tests. It serves automation on loopback with a fresh token (methods editor.info and
	// editor.quit), publishes its session files like the real editor, and exits on editor.quit or after a minute.
	bool IsFakeEditorLaunch(int argc, char** argv);
	int RunFakeEditor(int argc, char** argv);

}
