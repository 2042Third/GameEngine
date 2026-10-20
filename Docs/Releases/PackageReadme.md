# Strata

Strata is a 3D game engine with an editor that people and AI agents drive the same way. You build scenes in the editor,
write the game logic as C++ scripts that the editor compiles for you, play the game in the editor, and export it as a
game that runs on its own. Every editor operation is also a command that an AI agent can run, so an agent can make a
whole game through the editor.

This folder is a complete installation: run it from where you unpacked it. The release notes of this version are on the
project's releases page: https://github.com/2042Third/GameEngine/releases

## What is in this folder

| Path | What it is |
| --- | --- |
| `StrataEditor` | The editor. Start it to get the launcher: new project, open a project, the samples, connecting an AI agent. |
| `StrataRuntime` | Plays exported games. The editor copies it into every game it exports, renamed after the game. |
| `StrataCLI` | Command-line client of a running editor, and an MCP server for AI agents (`StrataCLI mcp`). |
| `Samples/` | Finished games to learn from (Tetris). The editor opens copies of them (Open Sample). |
| `StrataScriptCore/` | The script SDK: the headers your scripts include and the CMake package their build uses. |
| `Docs/Manual/` | Guides for making games: `Scripting.md`, `Worlds.md`, `Prefabs.md`, `GameView.md` and `GameUI.md`. |
| `Skills/` | Guides for AI agents that make games with the editor (see "Connect an AI agent"). |
| `StrataInstall.json` | Marks this folder as a Strata installation. Keep it next to the executables. |
| `ThirdPartyNotices.md` | The third-party components Strata contains and their licenses. |

The executables are `.exe` files on Windows. Keep the folder together: the editor finds the runtime, the SDK and the
samples next to itself.

## Requirements

Every platform needs a GPU with a Vulkan 1.2 driver to show the editor and play games. Started without one, the editor
(and a game) shows a message that says so, what to install and where its log is. Without a GPU the editor still runs
headless (`StrataEditor --no-gpu`, for automation and exports) and games run with `--headless`.

- **Windows 10 or 11, x64.** The Vulkan runtime comes with the graphics driver of NVIDIA, AMD and Intel GPUs; update the
  driver if the editor reports that Vulkan is not available. Nothing else is needed to run Strata.
- **Linux, x64: Ubuntu 24.04 or newer** (or another distribution with glibc 2.39 and the C++ library of GCC 14). The
  editor needs the Vulkan loader, a Vulkan driver, GTK 3 and the X11 or Wayland libraries (NVIDIA GPUs get Vulkan from
  NVIDIA's driver); exported games need the same without GTK. Desktop Ubuntu has most of them; to be sure:

  ```sh
  sudo apt install libvulkan1 mesa-vulkan-drivers libgtk-3-0t64 libx11-6 libxrandr2 libxinerama1 libxcursor1 libxi6 \
    libxkbcommon0 libwayland-client0
  ```

- **macOS 14 or newer, Apple silicon.** Vulkan runs on Metal through MoltenVK, which is not in this folder: install the
  Vulkan SDK from https://vulkan.lunarg.com/sdk/home#mac (choose "System Global Installation"), or run
  `brew install molten-vk vulkan-loader`. The macOS build has been run on one Mac so far, an Apple M2 Max (see the
  release notes).

**Building scripts** needs a C++ compiler and CMake 3.25 or later on your machine. The editor looks for them when it
first builds scripts and says exactly what is missing:

- Windows: Visual Studio 2022 (17.10 or later) or 2026 with the "Desktop development with C++" workload, or the Build
  Tools for Visual Studio with the C++ build tools, from https://visualstudio.microsoft.com/downloads/. Its C++ CMake
  tools (installed by default with the workload) provide CMake; a CMake from https://cmake.org/download/ works too.
- Linux: `sudo apt install g++ cmake ninja-build`.
- macOS: `xcode-select --install` (the Command Line Tools), and `brew install cmake ninja` or CMake.app from
  https://cmake.org/download/.

## First start

The executables are not signed, so the system asks before it runs them the first time.

- **Windows:** before unpacking, right-click the downloaded `.zip`, choose Properties and tick **Unblock**, so the
  unpacked files (and the games you export, which copy the runtime) are not marked as downloaded. Otherwise SmartScreen
  shows "Windows protected your PC" when you start the editor: choose **More info**, then **Run anyway**.
- **macOS:** remove the quarantine mark from the unpacked folder once, in Terminal:
  `xattr -dr com.apple.quarantine ~/Downloads/Strata-<version>-macos-arm64` (use the folder's real path), then start
  `./StrataEditor` from that folder.
- **Linux:** start `./StrataEditor` from the folder.

The editor and exported games are windowed programs: they open no console window, and a window started from a terminal
leaves the terminal alone (closing the terminal or pressing Ctrl+C in it does not close the window). Their logs are in
your user data folder: `Strata/Logs/StrataEditor.log` for the editor and `<Game>/Logs/Game.log` for a game, under
`%LOCALAPPDATA%` on Windows, `~/.local/share` on Linux and `~/Library/Application Support` on macOS. `--version`,
`--help` and runs without a window or with a fixed number of frames (`--headless`, `--no-gpu`, `--frames`, `--commands`)
print into the terminal, and Ctrl+C ends them in order. PowerShell does not wait for windowed programs: to wait for one
and get its exit code, pipe its output, e.g. `.\StrataEditor --version | Write-Output`.

## Make a game

1. Start `StrataEditor`. In the launcher choose **New Project**, pick the **Basic 3D** template (a lit scene with a
   camera, a sun, a sky and a ground), a name and a folder. Or choose **Open Sample** and open a copy of Tetris.
2. Build the scene with the Scene view, the Hierarchy and the Inspector. Drop models, textures and sounds onto the
   window to import them. Menus and HUDs are UI elements (**Hierarchy > + > UI**, see `Docs/Manual/GameUI.md`); a level
   too large to keep loaded is a world of streamed cells (**New > World** in the Content Browser, see
   `Docs/Manual/Worlds.md`).
3. Write scripts in the project's `Scripts` folder (**Scripts > New Script...** starts one) and build them with
   **Scripts > Build Scripts** (Ctrl+B). Build errors are listed in the Problems panel; a double click opens the line.
   Attach a script in the Inspector (**Add Component > Script**) and press **Play**: Play builds changed scripts first,
   and rebuilding while the game plays reloads them without stopping it. The Game view shows the game as a player sees
   it, at the resolution you pick (`Docs/Manual/GameView.md`). `Docs/Manual/Scripting.md` describes the SDK.
4. Choose the start scene (**File > Set as Start Scene**) and export with **File > Export Game...**. The export folder
   holds everything the game needs: run the game's executable there.

## Connect an AI agent

The launcher's "Connect an AI agent" section shows the exact command. For Claude Code it is:

```sh
claude mcp add strata -- "<this folder>/StrataCLI" mcp
```

The agent then sees the editor's most used commands as tools (the `core` toolset; it can turn on the others, such as
`scene`, `ui` or `world`, or you start the server with `--toolsets all`), and can start an editor itself (headless or
with a window). The guides in `Skills/` teach an agent the editor's commands (`strata-editor-automation`), the script
SDK (`strata-scripting`) and the whole path from an empty project to an exported game (`strata-make-a-game`). For
Claude Code, copy the three folders into `~/.claude/skills/` (every project) or into your game project's
`.claude/skills/`.

## When something goes wrong

- The editor's log is `%LOCALAPPDATA%\Strata\Logs\StrataEditor.log` on Windows, `~/.local/share/Strata/Logs` on Linux
  and `~/Library/Application Support/Strata/Logs` on macOS. An exported game logs to `Logs/Game.log` in a folder named
  after the game beside `Strata` (`%LOCALAPPDATA%\<Game>\Logs\Game.log` on Windows).
- Help > About Strata shows the version, the GPU and the driver; `StrataEditor --version` prints the version.
- Report problems at https://github.com/2042Third/GameEngine/issues with the version, your system, what you did and the
  log.
