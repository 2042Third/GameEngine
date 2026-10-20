# Strata documentation

Documentation of how Strata works. Rules for changing it live in [AGENTS.md](../AGENTS.md); this folder explains the
design and links to the code that implements it.

- [DeveloperGuide.md](DeveloperGuide.md): the Strata Developer Guide, the details of how to work in every area that
  AGENTS.md has rules for: the repository layout and engine modules, the build requirements and options, packages and
  releases, the test runners, the perf lab and the feature test, the asset pipeline, scripting, audio, animation, the
  editor and its commands, rendering, automation and the editor's fonts. Read the section of the area you work on before
  changing it.
- [Architecture.md](Architecture.md): the targets and their dependencies, the engine modules, the frame loop and the
  scene runtime lifecycle, the threading model, the asset pipeline, streaming and residency (memory budgets, eviction,
  the load queue), the render scene, animation, scripting, the editor, export and the runtime.
- [Roadmap.md](Roadmap.md): what Strata is meant to be (its pillars), the waves of workstreams that get it there, and
  the status: what each landed wave delivered and the open items later waves own.

## Manual

Guides for people making games in the editor:

- [Manual/Scripting.md](Manual/Scripting.md): creating, building, debugging and hot-reloading scripts, the Problems
  panel, crash reports with file and line, opening scripts in an IDE (CMake presets for IntelliSense) and attaching a
  debugger.
- [Manual/Worlds.md](Manual/Worlds.md): worlds of streamed cells: layers, editing by cell, the cell map, saving,
  cooking, playing and exporting, streaming sources and the script API, and their limits.
- [Manual/Prefabs.md](Manual/Prefabs.md): making prefabs, placing instances, overrides (the prefab bar, the marks,
  Apply and Revert), how prefab changes reach instances, and unpacking.
- [Manual/GameView.md](Manual/GameView.md): playing and testing in the editor: the Scene and Game views, resolutions,
  game input and the mouse, pausing and stepping.
- [Manual/GameUI.md](Manual/GameUI.md): menus and HUDs: canvases, anchors and scale modes, the look of images, texts and
  widgets, buttons and sliders in scripts, keyboard and gamepad navigation, testing UI with commands.

## Releases

What each release of Strata delivers, for the people who download it, and what the archives hold:

- [Releases/0.4.0.md](Releases/0.4.0.md): Strata 0.4.0 (wave 4: worlds streamed in cells, GPU-driven rendering, game
  UI, background saves, the Game view, prefab overrides, exact play-testing, engine modules).
- [Releases/0.3.0.md](Releases/0.3.0.md): Strata 0.3.0, the first release (waves 1 to 3).
- [Releases/PackageReadme.md](Releases/PackageReadme.md): the `README.md` of the release archives (requirements, first
  start, making a game, connecting an AI agent). How releases are cut: DeveloperGuide.md, "Releases".

## Elsewhere in the repository

- [AGENTS.md](../AGENTS.md): the source of truth for rules, which agents load into every session: the golden rules,
  building, code style, architecture rules, a digest of the hard rules of every area (testing and the feature test, the
  asset pipeline, the script ABI, audio, animation, the editor, rendering, automation) and the pre-commit review
  checklist. Their details are in [DeveloperGuide.md](DeveloperGuide.md).
- [README.md](../README.md): features, building, and making a game with the editor.
- [.claude/skills/](../.claude/skills/): task playbooks, see [Skills](#skills).
- [Samples/](../Samples/): example games as editor projects, see [Samples](#samples).
- [StrataTests/FeatureTest/](../StrataTests/FeatureTest/): the feature test project, a scene with every component
  and scripts that use the whole SDK (DeveloperGuide.md, "Feature test").
- [ThirdPartyNotices.md](../ThirdPartyNotices.md): third-party components and their licenses.
- [GameEngineDoc.md](../GameEngineDoc.md): the original project brief.

## Skills

Task playbooks for people and AI agents, in `.claude/skills/<name>/SKILL.md`:

| Skill | Use it to |
| --- | --- |
| [strata-build-test](../.claude/skills/strata-build-test/SKILL.md) | Build, run the test suites, verify a change. |
| [strata-add-component](../.claude/skills/strata-add-component/SKILL.md) | Add or change an ECS component. |
| [strata-scripting](../.claude/skills/strata-scripting/SKILL.md) | Write, build, debug and ship game scripts. |
| [strata-editor-automation](../.claude/skills/strata-editor-automation/SKILL.md) | Drive the editor (CLI, MCP). |
| [strata-make-a-game](../.claude/skills/strata-make-a-game/SKILL.md) | Make a whole game through the editor. |

## Samples

- [Samples/Tetris](../Samples/Tetris/): a complete game an AI agent made through editor commands only (`Tetris.stproj`,
  `Assets/` with `.meta` files, C++ scripts in `Scripts/`). Open a copy of it from the launcher (Open Sample, or
  `project.openSample`) and press Play, which builds its scripts first; `StrataEditor --project Samples/Tetris` opens the
  sample itself, to change it. `Samples/Samples.json` lists the samples. The CTest `StrataEditor.Tetris`
  (`StrataTests/Editor/TetrisSample.cmake`) plays a copy with simulated input, checks its HUD, exports it and runs the
  exported game headless.

## Keeping the documentation current

- A change that makes a statement here wrong updates it in the same commit (AGENTS.md, review checklist item 5).
  Statements name the files and types they describe, so they can be checked against the code.
- Rules and conventions belong in AGENTS.md, their details in DeveloperGuide.md (a rule AGENTS.md shortens is stated in
  full in the guide and changed in both, a rule AGENTS.md keeps whole is only there, and AGENTS.md stays under 40,000
  characters); they are linked from here, not copied.
- Markdown with lines of at most 120 columns. DeveloperGuide.md keeps the wrapping of the text it took over from
  AGENTS.md, so that `git blame -C` follows each of those lines into AGENTS.md's history.
