# Architecture audit STATE (wave 4 planning)
Source: <repo>/.worktrees/verify (main 685aef7). Read-only.
## Done
- Read Roadmap.md, Architecture.md (1-540), AGENTS.md (verify copy).
## Findings so far
## Evidence files (this dir)
- Layers.json + allowlist read (8 entries: 7 Application.cpp, 1 AssetMeshProvider->Mesh).
- Application.cpp hard-codes: RegisterBuiltinModules, JobSystem, Window, Input, GraphicsDevice/Renderer, AudioEngine, ImGuiLayer; dtor releases AssetManager/Project active.
- Active globals: AssetManager::GetActive used in AudioSystem(5), SceneLoader, SceneRenderer(3), RenderScene, ScriptHostAPI, AssetMeshProvider(5), AnimationSystem(3), AnimationPreview; ScriptEngine::GetActive in ScriptingRegistration (ScriptSystem factory), GameRuntime.
- runtime_size_breakdown.txt: StrataRuntime.exe Release 7.33 MB; imgui.lib 401 KiB + ImGui layer 24 KiB; ComponentRegistration.obj 554 KiB(!); EmbeddedShaderRegistry 366 KiB; nvrhi validation 131 KiB; vulkan-raytracing 51 KiB; Jolt 952 KiB; miniaudio 507 KiB.
- longfuncs.txt: Register*Commands 900-line functions; 18 functions >200 lines, 100 >100.
- CTest Debug (verify-2 final): 658 s; StrataTests.Core alone 474.7 s (serial). Release Core 122 s.
- CI (ci_durations.txt): Windows ~20-24 min, Linux 12-20, macOS 9-16; Release only, no cache.
- !!! CI on main 685aef7: macOS Build FAILED (ci_685aef7_annotations.txt): Mesh.cpp:465 size_t vs uint64_t initializer list; AnimationAssetViews.cpp:64 from_chars(float) deleted on Apple libc++; ScriptFaultTests.cpp:60 unused function. Linux/Windows were in progress at 06:40Z.
- BUILD (out-of-source Ninja MC + MSVC, Debug, 24 jobs, scratch dir b/; build.ps1; rc patched via patch_rc.py since Ninja MC rc fails RC1107 here):
  clean phases: vendor 12.7 s, Strata 33.4 s, runtime 7.5, editor 29.4, rest 87.3, links 3.1 => ~173 s wall; CPU sum 3,099 s
  (build_debug_breakdown.txt): StrataTests 52%, EditorCore 12%, Strata 10%, EditorUI 6.5%. No PCH outside Strata.
  incremental (incremental.ps1, deletes dependents): Scene.cpp 4.7 s, InspectorPanel.cpp 5.8 s, EditorContext.h (99 objs) 42.7 s, Components.h (251 objs) 86.2 s.
  fanin_debug.txt: Components.h 251 objs, Scene.h 245, AssetManager.h 236, Renderer.h 163, SceneRenderer.h 140, ScriptEngine.h 135, EngineServices.h 117.
  tu_summary_debug.txt (single TU alone, /Bt+ /d1reportTime): front end 85-95%; ComponentRegistration 6.4 s (function defs 4.3 s = templates);
  EditorUITests 5.8 s (729 headers, 3.05 s includes); EditorSceneCommands 5.1 s (CommandUtils.h 1.67 s inclusive, json 0.66).
  Release Ninja build in scratch fails on path length (glslang obj path >250 chars), not an engine issue; skipped.
- Components of every module live in Scene/Components.h + Scene/ComponentRegistration.cpp (27 components). Audio reads RigidBodyComponent (AudioSystem.cpp:650), Renderer gizmos read collider comps, ScriptHostAPI reads RigidBody/AudioSource/Animator comps.
- Docs: doccheck.txt -> identifiers in AGENTS/Architecture all exist (good). AGENTS.md 169 KB / 23k words (60 KB on 10-09).
- TextRenderer.cpp:151-160 screen-space text in raw pixels (no reference resolution).
- CI 685aef7 final: Windows success, Linux FAIL (ScriptFaultTests.cpp:60 unused fn; -Warray-bounds AnimationAssetTests.cpp:29 via vector push_back), macOS FAIL (3 errors). 6b23652 pushed after (CI on ci/ branches; no code fix). ci_history.txt: 15 of 22 recent runs failed.
- core_durations_summary.txt: Core = 1,672 cases, 201 s Release in one process; 27 cases >=1 s = 81 s; 'Version 1 packs that are corrupt are still refused' 26.4 s.
- shots/tetris_1280x720.png vs tetris_2560x1440.png: HUD FontSize 40 px fixed -> half relative size at 1440p.
- smoke/: minimal game (2 assets) windowed 1280x720: 335 MB private (device 177 MB), frame 0 294 ms (Input 276 ms), median frame 0.15-0.2 ms; headless 15.7 MB; 48 threads (23 workers+2 IO+23 background).
- SceneSystem.cpp:96/197: Audio After={Physics} -> Audio fails to register if Physics absent.
- GameExport.cpp:182 ships the runtime next to the editor (editor's config: Debug editor => Debug game). GameRenderer uses default SceneRendererSpecification (no quality settings).
- No crash handler outside scripts (no SetUnhandledExceptionFilter/minidump); log flush_on(warn).
- README: features stale (no animation/controllers/streaming), `ctest --preset windows-release` runs perf label.
- NEXT: write findings.md + StructuredOutput.
- 00:43 writing findings.md (all measurements done; Dist: runtime builds, editor+tests fail EditorIcon.h:53 C4189; Dist runtime runs Tetris/Smoke fine)
- 00:47 DONE: the harness refused writing findings.md (subagents may not write report files); the full report went into the StructuredOutput (18 findings). Evidence files listed above stay in this directory.
