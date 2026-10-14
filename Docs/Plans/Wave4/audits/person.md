# Person-making-a-game audit — running notes

Scratch: this directory. Binaries: <repo>\.worktrees\verify\build\windows\bin\Release (main 685aef7).

## Plan
1. Launcher (150% and --ui-scale 1), New Project per template, Open Sample.
2. Build a scene: primitives, textured glTF, skinned+animated glTF, materials, lights, sky, post, prefabs,
   character controller + animated char, New Script + build, play/pause/step/stop, undo, save, reopen, export, run.
3. Judge every screen; write findings.md with screenshots.

## Log
- 23:48 launcher 150%/100% (shots/01,02), dialogs newProject/openSample/about (shots/03_*). Only 2 templates.
- 23:49 projects/Basic (basic3d "Meadow") and projects/Empty created (shots/04,05,06). basic3d game view = grey slab (06_basic_scenecam.png).
- Downloads in downloads/: Fox, CesiumMan, DamagedHelmet (glb), Avocado (gltf+bin+png), PolyHaven rocks 1k + kloofendal hdr.
- Helper: scripts/run_editor.ps1 -Name <shot> [-Project] [-Commands] [-Frames] [-UiScale] [-QuitAfter]
- 23:51 Tetris sample: opened (07), Play builds scripts 5.2 s and plays (08). OK.
- 23:52 projects/Grove (basic3d) + imports (10). Scene built (11). Fox glTF huge (cm). CesiumMan LIES DOWN in edit mode
  (bind pose without skeleton root orientation) -> shots/12,13_man_side_edit vs 13_man_side_play, 25_focus_man. Thumbnail too.
- Prefabs (14,16): apply reaches only unmodified instances (whole-instance granularity): r2 with only CastShadows changed
  kept old material. Inspector has no Apply/Revert (only Hierarchy context menu); no per-property override marks.
- Import Scale applies to model ROOT transform (GltfImporter.cpp:1867) -> changing it does nothing to placed instances (18).
- Material: no separate roughness map slot (MetallicRoughnessMap only, G=rough B=metal); PolyHaven rough map misuse.
- Script build error (19): Problems panel good; status pill says "3 errors" for 1 compile error.
- Build OK + toasts (20): "Building scripts" toast stays next to "Scripts built"; toasts cover Inspector Add Component.
- Play with input (21): fox walks/runs; animator states OK. Ball physics OK.
- Export (22,23) OK; exported game runs (26). Runtime ignores --help: ran the game windowed forever (killed it).
- Viewport always uses scene camera in Play (ViewportRenderer.cpp:22): no scene view while playing.
- 00:10 screens (30_*, crops/): texture/material/clip/prefab/sky/post tabs. Normal map nor_gl_1k imported as Usage Color
  (TextureImporter.cpp:248-283 suffix match fails on "_1k"); no fix prompt on Normal Map slot.
- Animation preview only via command (ViewportPanel.cpp:381-410 chip; no UI starts it). Clip tab: channel table, no playback.
- Recovery (31) works; but every project open re-offers "discarded on purpose" entries (35, 37; RecoveryDialog.cpp:42-77).
- Script crash contained, located Crasher.cpp(18) (35 log).
- NEW SCRIPT BUG (critical): in an already-built project, script.create (New Script) or a hand-added .cpp + one
  script.build -> class missing; 2nd build has it. logs: shots/newscript_build.log, newfile.log, 34_script_crash.log.
  Cause: ScriptBuild.cpp:921-927 configures only on toolchain stamp change; StrataScriptModule.cmake:33 GLOB CONFIGURE_DEPENDS
  + VS generator builds stale project. Test ScriptBuildPackageTests.cpp:295 covers only first build.
- Undo through 10 mixed steps restores clean scene (undo_all.log). UI scale 1 (32), 1600x900 (33) OK.
- Launcher with recents (39). No GPU timer queries anywhere (Performance panel CPU only).
- NEXT: write findings.md and return StructuredOutput.
- 00:25 The harness refused writing findings.md ("subagents return findings as text"); the 16 ranked findings and the
  "works well" list went into the StructuredOutput instead. Ranking: 1 New Script class missing after build (critical,S),
  2 skinned bind pose in edit mode (critical,M), 3 prefab per-property propagation + inspector bar (major,L),
  4 import Scale vs placed instances (major,S), 5 normal map usage detection + fix prompt (major,S), 6 in-game UI (major,XL),
  7 scene view while playing / game view aspect (major,M), 8 animation preview UI (major,M), 9 project settings/input
  actions/project-wide layers (major,L), 10 onboarding templates+docs (major,M), 11 particles (major,XL),
  12 fog/terrain/probes (major,XL), 13 GPU timings (major,M), 14 primitives without colliders (minor,S),
  15 recovery nag (minor,S), 16 feedback polish (minor,S).
