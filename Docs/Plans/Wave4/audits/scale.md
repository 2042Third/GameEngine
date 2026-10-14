# Scale & streaming audit (wave 4) - STATE
Started 2026-10-12. Read-only on repo. Scratch: this directory.
## Done
- (nothing yet)
## Findings so far
## Evidence locations
## 2026-10-12 23:55 - Experiment A: world of cubes in the editor (Release, windowed, perf lock)
- gen_world.py -> World/ (World1M: 1,000,404 entities, 263 MB compact JSON; World250k 250,104, 66 MB; 10% static box colliders, cubes under 2,500-cube districts; Empty start scene).
- run_editor.ps1 + cmdsXXX.json; logs run250k.log / run1M.log (summarize.py prints per command).
- 1M: scene.open 8.2 s (max frame 95 ms), edit frame 15.3 ms (Render.Extract 14.7, editor camera frames whole world), play.start frame 599 ms (Render.Extract 360, Physics 157), play frames 4.0 ms, private 1.44 GB editing / 2.88 GB playing, peak private 3.95 GB during open, 8.76 GB at save.
- scene.save 1M: 22 s on main thread (EditorContext::SaveScene serializes DOM + Dump(1,'\t') synchronously), file 263 MB -> 541 MB. 250k: 4 s, 66 -> 135 MB.
- export 1M: 26.8 s, pack 21 MB (scene JSON 541+135 MB stored), max frame 53 ms.
- 250k: open 2.06 s (max 30.7 ms), edit 3.86 ms, play start 168 ms (Extract 95.7, Physics 35.7), play 1.57 ms.
- First scan of a project with these scenes: 6.4 s (reference search parse), 204 ms afterwards.
## 23:58 Experiment B: exported world runtime (export1M/World1M.stgame, World250k.stgame; run_runtime.ps1, perflog.py)
- 1M headless: start scene plays at 8.5 s (frame 508), peak private 3.96 GB during load (JSON DOM), 775 MB after; first play frame physics 147 ms (100k static bodies in one frame). Windowed --frames 900 ended before load; quitting during the load took 8 s (parse not cancellable).
- 250k windowed: scene prep frames Scene.Load 4-15 ms (16 frames), swap frame 62 ms (Physics 33.6, Render.Submit 25.1); play frame 1.2 ms; peak 1.28 GB.
## 23:58 Experiment C: cells via sub-scenes (Cells/ project, gen_cells.py 4 x 25k cubes, every cube a static body; runcells.log)
- each cell 0.52-0.70 s, max frame 12-17 ms (Render.Extract up to 9.7, Render.Submit 6.5, Physics 4-5).
- 3rd cell: "Physics: cannot create a body for 'Block' and 1359 more entities: the world's limit of 65536 bodies is reached; ... next play start sizes the world" -> streamed bodies lose collision.
- with 34k bodies waiting: frames median 13.6 ms, p95 51 ms (Scene.Physics avg 13.5 max 54).
- unload 1 cell: 56 ms frame (Physics 50.9); unload 3 cells: 51 ms frame. Not sliced.
## 00:05 Experiment D: import of 24 x 4096^2 noise PNGs (bigtex/, 892 MB; ImportTest/ project; runimport2.log)
- asset.import folder (wait:false): done in ~38-41 s; editor frames median 0.4 ms, max 1.5 ms, except one 92.6 ms frame at the start (Assets.Update 91.5 ms).
- peak private 3.74 GB (plateau 2.83 GB while 23 imports decode at once), back to 500 MB after. Cache 534 MB (22.4 MB BC7 each).
## 00:08 Experiment H: contention (contend_*.log) and import registration hitch (runsmall.log, runsmall600.log)
- scene.open World250k alone 2.21/2.28/2.25 s (max frame 19.4/20.8/18.8 ms); during 24-texture import 4.61/3.51/4.25 s (max 81.8/95.9/210.9 ms). Interleaved, 3 rounds.
- asset.import of a folder: one frame of 1,707 ms for 300 small PNGs, 3,605 ms for 600 (Assets.Update; ~6 ms per file of main-thread registration: RegisterAddedFile -> RegisterSourceFile writes .meta etc.).
## 00:12 Experiment F: mip streaming (stress game runs flight_*.csv/topdown*.csv; gallery sweep in editor rungallery.log, gen_gallery.py)
- Stress game: Flight settles at frame 6 (288 ms incl. 270 ms first frame), peak 468 MB; TopDown needs only tails (0.023 MB textures), peak 466 MB; budget 8 MB changes nothing (only tails needed).
- Gallery: 24 x 4096^2 BC7 (534 MB) under 64 MB texture budget, 24 camera cuts low over each plane: every cut settles within 60 frames (MipsBelowWanted 0), ~21.7 MB streamed per cut, textures 43-44 MB resident, levels dropped for budget 59, max frame 1.6-2.9 ms (Assets.Finalize <= 2.2 ms) apart from 35 ms capture frames. WORKS WELL.
## 00:23 more
- stats overlay 1M world (stats1M.png): walking camera 246,128 visible of 1,000,001 proxies, extract 2.71 + sort 1.44 ms, 2 draws; whole world in view: Render.Extract 16.2 ms.
- 1.1M-entity scene (Over/ project): scene.open fails after ~7 s parse: "Cannot add 1100445 entities to a scene with 0: a scene holds at most 1048575 entities"; first scan 6.8 s; peak 3.56 GB.
## 00:26 Experiment G: grid drive (Grid/ project, gen_grid.py: 10x10 cells of 2,502 entities, 501 bodies; rungrid.log)
- each cell loads in 19-23 ms over ~30 frames (sequential scene.loadAdditive), unload of 3 cells per step; max frames 7.7-8.5 ms over the drive (Render.Extract 2.4, Scene.Load 2.4, Physics 2.2), p95 0.7-0.9 ms. Small cells stream fine with today's sub-scenes (WORKS WELL).
- one 306 ms frame at the first step was in the Input stage (277 ms) - window event, not streaming.
## 00:53 Experiment I: many assets (Many/ 30,000 materials + scene of 30k cubes; Many10k*)
- MACHINE NOTE: another agent's headless StrataEditor (pid 27860, since 23:50:55) used ~4.4 cores and the CPU was at 100% at 00:34; earlier timings may be inflated. Large effects re-checked.
- first scan 30k fresh: 233 s (loaded machine); 10k fresh: 97.7 s loaded, 26.4 s at 17% CPU (2.6 ms/asset); warm 30k scan 13.3 s.
- StrataCLI launch --headless on fresh 30k copy: "Timed out after 60000 ms waiting for the editor (process 14168): no session file was written; the editor was stopped" (exit 2). 10k fresh launch: ok, startupSeconds 27.9.
- export 30k: 2.8 s, pack 122.5 MB for 11.5 MB stored (4 KiB alignment per entry: 10.7x).
- 30k unique-material cubes frame: 10.6 ms (Render.Extract 9.95) first run; 22.7 ms under load.
- runtime of 30k pack headless: mount + start fast, peak 147 MB.
- save frame 250k: editor.perf shows a 4,799 ms frame (runsave.log). Machine at ~97% CPU from other agents at 00:55 (StrataTests since 00:43 + editor).
- 1M reopen (runopen1M.log, loaded machine): open 16.3 s, max frame 38 ms (Scene.Load 37.4, Render.Submit 13.3); play start 639 ms (Extract 418, Physics 155, untimed 53).
## 01:00 Analysis done; writing findings.md (report) and returning StructuredOutput.
Code refs used: EditorContext.cpp:663 SaveScene; PhysicsWorld.cpp:99 c_MinAutomaticBodies; SceneSubScenes.cpp UnloadSubScene; SceneRendererExtraction.cpp:1080;
EditorAssetManager.cpp:2691/3622 RegisterAddedFile, :1729 background import submit; AssetPack.h:96; EditorSceneCommands.cpp:588; JobSystem.cpp; Mesh.h:79,110; AudioClip.h:13; EditorLauncher.h:26.
