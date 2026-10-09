# Strata documentation

Documentation of how Strata works. Rules for changing it live in [AGENTS.md](../AGENTS.md); this folder explains the
design and links to the code that implements it.

- [Architecture.md](Architecture.md): the targets and their dependencies, the engine modules, the frame loop and the
  scene runtime lifecycle, the threading model, the asset pipeline, scripting, the editor, export and the runtime.

## Elsewhere in the repository

- [AGENTS.md](../AGENTS.md): the development guide and the source of truth for rules: building, testing and the
  feature test, code style, architecture rules, the asset pipeline, the script ABI, audio, editor commands, rendering,
  automation and the pre-commit review checklist.
- [README.md](../README.md): features, building, and making a game with the editor.
- [.claude/skills/](../.claude/skills/): task playbooks, see [Skills](#skills).
- [Samples/](../Samples/): example games as editor projects, see [Samples](#samples).
- [StrataTests/FeatureTest/](../StrataTests/FeatureTest/): the feature test project, a scene with every component
  and scripts that use the whole SDK (AGENTS.md, "Testing").
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
  `Assets/` with `.meta` files, C++ scripts in `Scripts/`). Open it with `StrataEditor --project Samples/Tetris`, build
  its scripts (Ctrl+B) and press Play. The CTest `StrataEditor.Tetris` (`StrataTests/Editor/TetrisSample.cmake`) plays
  a copy with simulated input, checks its HUD, exports it and runs the exported game headless.

## Keeping the documentation current

- A change that makes a statement here wrong updates it in the same commit (AGENTS.md, review checklist item 5).
  Statements name the files and types they describe, so they can be checked against the code.
- Rules and conventions belong in AGENTS.md and are linked from here, not copied.
- Markdown with lines of at most 120 columns.
