# Writing scripts

Game logic in Strata is C++: script classes that the editor builds into a *script module* (a shared library) and runs
while the game plays. This guide is for people writing scripts in the editor. The SDK's headers,
`StrataScriptCore/Include/StrataScript/*.h` in the engine's folder, document every function; the
[strata-scripting skill](../../.claude/skills/strata-scripting/SKILL.md) has recipes (movement, character controllers,
gamepads, save files), and [Architecture.md](../Architecture.md) ("Scripting") explains how the engine runs them.

## Create a script

**Scripts > New Script...** asks for a class name and writes `Scripts/<Class>.cpp` in the project:

```cpp
#include "StrataScript/StrataScript.h"

using namespace Strata;

class DoorOpener : public Script
{
public:
	float Speed = 1.0f;

	// When the entity starts playing (its fields hold the scene's values by now).
	void OnCreate() override
	{
	}

	// Every frame; deltaTime is the frame's game time in seconds.
	void OnUpdate([[maybe_unused]] float deltaTime) override
	{
	}
};

ST_SCRIPT_CLASS(DoorOpener)
{
	ST_SCRIPT_FIELD(Speed);
}
```

The name must be a C++ class name that the project's scripts and the SDK do not use yet (the dialog says why when it
cannot). The file opens in your code editor (see [Your code editor](#your-code-editor)), and the scripts start building:
the status bar shows "Building", and a class can be attached once a build holds it. Every `.cpp` and `.h` file in
`Scripts/` (and its folders) belongs to the module; you can also add files there by hand. Tools and agents create scripts
with the command `script.create {"class", "fields", "template"}`.

Fields registered with `ST_SCRIPT_FIELD` (bool, int32_t, float, glm vectors and quaternions, std::string, Entity,
AssetHandle) show in the Inspector, are set per entity, and keep their values when the module is rebuilt while the game
plays.

## Attach it and play

Once the build that New Script started finished, select an entity, **Add Component > Script** in the Inspector, and pick
the class: the Inspector offers the classes of the built scripts, so a class you added by hand needs **Scripts > Build
Scripts** (Ctrl+B) first. Then press **Play** (Ctrl+P): when the scripts changed since they were last built (a file
edited, added or removed), or were never built, Play builds them first and starts the game once the build succeeded. The
status bar shows "Building to play"; click it to cancel.

A project whose scenes use no script yet is not asked to build: its status bar says "Scripts unused" until an entity has
a Script component.

## When the build fails

The status bar's pill turns to "Build failed"; click it, or open **View > Problems**: the Problems panel lists every
error and warning with its file (relative to the project) and line. **Double-click** one to open the file at that line
in your code editor; hover it to see the whole message and path. A play whose build failed shows the Problems panel by
itself. The Console has the whole build output; messages that name a file and line open it when clicked.

## Your code editor

Strata opens script files in the editor you choose under **Scripts > External Editor...**: a command with placeholders,
for example

```text
code -g {file}:{line}:{column}
"C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/devenv.exe" /Edit {file}
clion --line {line} {file}
```

`{file}`, `{line}`, `{column}` and `{directory}` (the project's `Scripts` folder) are filled in; without `{file}` the file
is passed last. The program is started directly, not through a shell: name an executable, not a `.cmd` or `.bat` (for
VS Code on Windows, `Code.exe` in its installation folder). Without a command, files open in the application your system
uses for them. The setting is yours (kept in your user data, not in the project); the environment variable
`STRATA_EXTERNAL_EDITOR` overrides it.

### IntelliSense and building in the IDE

The editor writes `Scripts/CMakePresets.json` with the engine's compiler, configuration and folder, so IDEs resolve the
SDK's headers:

- **Visual Studio**: File > Open > Folder... on the project's `Scripts` folder; it picks the "strata" configure preset.
- **VS Code** with the CMake Tools and C/C++ extensions: open the `Scripts` folder and select the "strata" preset.
- **clangd** and other tools that read `compile_commands.json`: the Ninja and Makefile generators write it into the build
  folder (`.strata/Scripts/Build`, and `.strata/Scripts/IDE` for the preset).

A build from the IDE writes the module where the editor loads it, so the editor hot reloads it like its own build. The
presets hold paths of your machine: the `.gitignore` the editor writes next to them keeps them out of version control,
and the editor rewrites them when the engine or its compiler changes (unless you removed the `strata.dev/editor` vendor
entry to keep your own).

## Hot reload

Build while the game plays and the new code takes over: every script instance is recreated with its fields kept, and
`OnReload` runs instead of `OnCreate`. Members that are not fields start over. A build the IDE makes reloads the same
way.

## When a script crashes

A crash in a script (a null pointer, a division by zero, an exception that escapes) stops play mode and disables the
scripts until they are rebuilt or reloaded; the editor keeps running. The Console and the Problems panel say where it
happened:

```text
Script crash in DoorOpener::OnUpdate on entity 'Door' (...): Access violation writing address 0x0000000000000000 ...
The script crash in DoorOpener::OnUpdate happened at C:/Games/Castle/Scripts/DoorOpener.cpp(18) in DoorOpener::OnUpdate
```

On Windows the file and line come from the module's program database (the PDB next to it, which every configuration
writes); the Problems panel also lists the stack's frames with their files. In an optimized build the compiler copies
small functions into their callers ("inlining"): the report still names the line that crashed, and the next frame is the
call it was copied into (frames of copied functions are marked "(inlined)"). On Linux and macOS the report names the
function and the offset in the module (file and line are not read there). Fix the code and press Play: it builds first.

## Debugging with a debugger

Attach your debugger to the running editor (Visual Studio: Debug > Attach to Process..., `StrataEditor.exe`; VS Code:
"Attach" with the C++ debugger) and set breakpoints in your script files. The editor runs the module from a private copy
(so builds can replace the original while it runs), in a folder under your user data (`Strata/Runtime`); the copy still
names its PDB, which stays in `.strata/Scripts/Bin`, so breakpoints bind. If your debugger does not find the symbols, add
`.strata/Scripts/Bin` to its symbol search path. Pausing in the debugger pauses the whole editor.

## Where builds happen

Builds use the engine's own CMake, compiler and configuration, in `.strata/Scripts/Build` of the project (the module goes
to `.strata/Scripts/Bin`). With Visual Studio, a project in a deep folder would make MSBuild's file tracker fail (it
cannot create files at paths over 259 characters): such projects build in a folder of your user data instead
(`Strata/Builds/<hash of the project's folder>`), and the build's first log line says so. Only when even that is too deep
is the build refused, with a shorter folder to move the project to.
