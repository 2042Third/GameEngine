# Wave 3's finishing round: review results

Three independent reviews (correctness, tests and perf evidence, portability and docs) of `agent/wave3-finish` as
merged into main (649d357), each finding then checked by an adversarial verifier. Status as of the handoff: *fixed*
names the commit on `agent/integration`; *open* findings are still to do; *refuted* ones were judged not real.

## correctness

### F1 fixed in 6dbf7bf: BackgroundLogSink flushes (warnings, errors, Log::Flush) and Stop wait for an empty queue, so they also wait for messages other threads log after them, without bound

`Strata/src/Strata/Core/BackgroundLogSink.cpp:110`

Verifier: Confirmed from the code and reproduced. In Run(), the flush/stop branch (line 124) only runs when the queue is empty at the end of a batch. While the queue is non-empty, line 110 takes the next batch and continues. So flush() (target = m_Queued, line 67) completes only when the writer finds the queue empty, not once the messages before the call are written. Stop works the same way.

Reproduction: repro/ in the notes dir compiles the merged BackgroundLogSink.cpp unchanged (stub stpch.h, Base.h and Platform.h, vendored spdlog header-only, MinGW -O2). The inner sink takes 200 us per line; output is in repro/repro_run1.txt.
- One producer at 1.33x the writer's rate: the messages queued before a warning were written 70 ms after the warning call. The warn() call itself returned after 2465 ms, only once the producer had stopped (at 2000 ms) and the backlog had drained.
- Stop called at 101 ms returned at 2007 ms, after the producer stopped at 1500 ms.
- Below saturation the wait stays bounded but stretched: 3.5 ms at 0.9x (backlog written 0.3 ms after the call), 0.9 ms at 0.67x.

So the unbounded case needs a saturated writer. In practice that means a slow console sink in a Console run printing to conhost; the file sink is buffered and fast. Under that condition, a warning or error from any thread blocks until the other producers pause. Any lock the caller holds is held that long too; for example, BindlessTextureTable::Release logs ST_CORE_WARN under m_Mutex. Before the change, the cost was one line write.

BackgroundLogSinkTests.cpp has no test with a second logging thread.

### F2 fixed in de218ca: Queued console and log-file lines are lost when the process ends without exit handlers (second Ctrl+C, console-close timeout, crash)

`Strata/src/Platform/Windows/WindowsPlatform.cpp:674`

Verifier: The executables link the static CRT (CMake/StrataConfigurations.cmake: CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded), so StopWriterAtExit, registered with std::atexit, runs only through exit() or a return from main. These endings lose the writer's queue:
- A second Ctrl+C or Ctrl+Break: OnConsoleControl returns FALSE (WindowsPlatform.cpp:674), and the default handler calls ExitProcess, which terminates the writer thread with its queue.
- A second SIGINT or SIGTERM on POSIX: the handler restores SIG_DFL and re-raises.
- CTRL_CLOSE_EVENT: the process is terminated after the 4.5 s handler returns, if the main loop has not finished by then.
- An unhandled crash.

Asserts are covered: Assert.cpp calls Log::Shutdown before aborting, and errors flush.

The practical regression is the console. Before the change, the wincolor console sink wrote each line unbuffered. The log file's info lines already sat in the CRT FILE buffer (spdlog file_helper uses fwrite) and were lost on these endings before too. Normally the window is the few milliseconds the writer lags; it grows only when the console is slow.

### F3 refuted: editor.wait reserves memory for every requested frame up front (up to 8 MB per pending call)

`StrataEditor/src/Editor/EditorStateCommands.cpp:694`

Verifier: The reservation is exactly what the wait's own frames fill. The old code's vector grew to the same size by the end of the wait: about 8 MB for 1,000,000 frames, as capacity 2^20 after doubling.

A pending editor.wait is not cancelled early; a disconnecting client only loses the answer. So the new code holds no memory the old code would not also hold during that same wait, only sooner. An allocation failure would have thrown from push_back in a measured frame before, too.

The memory-exhaustion scenario needs an authenticated client to issue about a thousand 4.6-hour waits, which the old code would also have paid for. Reserving the full count up front is the stated purpose of 5c6372f: no allocation in measured frames, because heap stalls there were the measured problem. Capping the reserve would reintroduce allocations in the frames the command measures. The large allocation is committed lazily by the OS, so the working set still grows only with the frames written.

### F4 fixed in d222f75: Activity panel caches Result widths keyed only by font size; the CJK fallback font merged later makes cached widths too narrow, so those results are clipped

`StrataEditor/src/Panels/ActivityPanel.cpp:189`

Verifier: Every step of the mechanism checks out:
- Inter has no U+FFFD (cmap maps it to glyph 0, checked with font_advance.py in the notes dir), so ImGui's FallbackChar is '?', 0.511 em in Inter Regular. CJK glyphs are about 1 em.
- ImGui 1.92.9's AddFont with MergeMode calls ImFontAtlasFontDiscardBakes, so after EditorFonts::UpdateFallback, CalcTextSize returns the real CJK advances.
- ActivityPanel caches finished entries' Result widths by id (GetResultWidth) and clears the cache only when ImGui::GetFontSize() changes, which the merge does not change. m_RowsKey does not change either.

So an entry measured before the merge (an undo step such as "Create '壁'", or an error quoting a CJK path) keeps a width about 0.49 em too small per CJK character. When it is the widest result, the fixed, non-resizable column clips its end for as long as it stays in the journal.

The trigger window is narrow:
- the Activity panel is open from the saved layout (it is closed by default);
- entries with CJK results finish in the first frames of a windowed session, before the I/O read of msyh.ttc and malgun.ttf is merged. Example: a windowed --commands run without --project.

Headless editors never merge and draw no panel. The details pane still shows the full text, so the effect is cosmetic.

## tests-perf

### F1 refuted: ImportSettings4kMaxFrame10kCubes: Measured comes from isolated runs; the perf-label gate runs at 90-92% of the new budget

`StrataTests/Perf/Budgets.json:84`

Verifier: Refuted by the round's own full-suite data. The finding uses only the three PerfStability runs (1.718, 2.794, 2.837). The reuse_ab runs (wave4/finish/ab/reuse_ab_compare.txt) are whole PerfGPU.* runs, and both of their builds contain e57138c (bins/base_full = 7390c22, reuse_full = fa298e0). They measured 2.982, 2.049, 1.875 and 2.064, 1.738, 1.822. Across all nine gate-like runs after the change the median is 2.05 ms (Measured 2.06) and the maximum 2.98 ms; no first attempt exceeded 3.09. So Measured describes the gate's median, and the budget is 1.5 x that median, as the _comment's rule requires. The commit computed its value from isolated runs, but the gate data agrees with it. Only 3 of the 9 runs are in the 2.8-3.0 ms mode, which confirmation covers.

### F2 OPEN (minor): RuntimeMaxFinalizeStage left as a gate whose budget is below the engine's designed frame-0 cost (half the final attempts were over)

`StrataTests/Perf/Budgets.json:123`

The round diagnosed frame 0's Assets.Finalize in the exported stress game at 3.68, 3.89 and 4.17 ms. That frame ends at the engine's own 4 ms FinalizeMsPerFrame limit, which the round calls by design ('explain, no change'), yet the budget stays 3.723 ms. The round's final PerfStability runs show the result: first attempts over budget in 2 of 3 runs, and 3 of 6 attempts over in total, including two in a row in run 2 (3.771, 4.022, then 3.153). With about half the attempts over, all three confirmations fail roughly once in 8 runs. The roadmap documents this, but the gate stays flaky by construction.

Evidence: perfstability/run2.json: Attempts [3.7707, 4.0222, 3.1528]; run3.json: [3.932, 3.5819]; run1.json: [3.5912]; Budget 3.723. STATE.md item 5: 'frame 0's Assets.Finalize 3.68/3.89/4.17 ms ... -> the 4 ms FinalizeMsPerFrame deadline'. Docs/Roadmap.md: 'its first attempts exceed it now and then'.

Suggested fix: Make the metric measure what its budget can bound, without raising the budget. For example, report the frame-0 finalization (the start scene's backlog) as its own metric, and judge RuntimeMaxFinalizeStage on frames after the first, as Mips.MaxFrameRatio leaves out the first frame. Or bound frame 0's finalization below 3.7 ms in the runtime (a smaller first-frame finalize budget or earlier mip-tail texture creation).

Verifier: The mechanism is real. AssetManagerBase::ProcessCompletions (AssetManager.cpp, about line 1067) stops only once now >= deadline, with FinalizeMsPerFrame = 4 ms. A frame-0 backlog therefore runs to at least 4 ms plus the last item, above the 3.723 ms budget. On the final code, 3 of the 6 PerfStability attempts were over (3.77, 4.02, 3.93). Counting the reuse_ab first attempts, about 1 attempt in 3 is over, so all three attempts fail together in roughly 4% of runs. This is not the round's defect, though. The budget dates from 1f44d52, before the round, the round did not touch it, the 0.3.0 roadmap already reported the same rate ('first attempts exceeded it in 2 of 4 runs'), and the round's roadmap entry documents that the budget lies below the engine's own limit. It is a known, tracked flake, not a regression from this merge, and it should not block landing. Fix guidance: Do not fix it in this landing by excluding frame 0 from the metric or by splitting the metric so that frame 0 gets a higher budget. Either one weakens the gate or raises a budget in disguise (AGENTS.md golden rule 2, the Budgets.json ratchet). Keep it on wave 4's list with the engine-side fix the roadmap names: cheaper texture creation for the 17 mip tails (suballocated GPU memory), or creating mip-tail textures before the first frame's finalization. Until then, record its outright-failure rate from PerfStability --runs 5 so that a regression can be told from the known mode.

### F3 fixed in 6336981: No test guards that frames destroy within a budget: reverting Renderer::BeginFrame to Collect() passes everything

`Strata/src/Strata/Renderer/Renderer.cpp:191`

Verifier: Confirmed. The only test that goes through Renderer::BeginFrame ('The renderer drops deferred releases at the frame after the GPU finished them') releases a single texture and checks that one frame drops it, which a budget-less Collect() does as well. The budgeted Collect is tested only on a local DeferredReleaseQueue. EditorPerfTests' startCold calls Renderer::GetDeferredReleases().Collect() and REQUIREs an empty queue before every attempt, so ThumbnailsMaxFrame no longer measures a frame with a backlog. Nothing else uses GetCompletedCount or c_FrameDestroyBudget (grep). Changing Renderer.cpp:191 back to Collect() would pass every test, and the bug this change fixed (one frame destroying every completed resource) has no regression test.

### F4 fixed in 814f89b: FrameTimeRecorder's reserve is smaller than the 1M scene.open the perf test runs, so a measured frame still reallocates

`StrataEditor/src/Editor/CommandUtils.h:103`

Verifier: Confirmed but harmless. In the nested_rec logs, every 1M scene.open recorded 10,040-12,992 frame times, more than c_FrameTimeReserve (8,192), so the recorder's vector grows once inside a measured frame. The commit message's claim that the polls 'allocate nothing in the frames they measure' is therefore false for that open. The header comment's '(past that it grows by doubling)' is also wrong for MSVC, which grows by 1.5x (the diag3 logs show capacities 5395, 8092, 12138, 18207). The impact on the gate is nil. The open the round targeted (SceneOpenOver1MMaxFrame, 100k over 1M) records 988-1,386 frames and never grows. The growth of the 1M open cost 0.03-0.04 ms, while that metric's longest frame is 14-16 ms against a 20.56 ms budget. editor.wait reserves its own frame count, at most 1M frames (8 MB), which is fine.

### F5 fixed in 6dbf7bf: BackgroundLogSink::flush can wait without bound while other threads keep logging (untested)

`Strata/src/Strata/Core/BackgroundLogSink.cpp:124`

Verifier: The logic is confirmed. In BackgroundLogSink::Run, the flush branch runs only when m_Queue is empty after a batch; otherwise it continues and writes the next batch. flush() waits for m_Flushed >= target, and m_Flushed is set only in that branch. If other threads keep the queue non-empty at every check, a warning or error logged on any thread (loggers flush_on(warn)) waits until they pause, even though everything up to its target has long been written. Stop() has the same property: it ends only on an empty queue. This needs sustained logging from other threads at about the writer's rate, which is rare in the engine, but nothing bounds it and no test covers flush under concurrent logging. The synchronous design before the change bounded a warning at one write.

### F6 fixed in 8c93610: BackgroundLogSink tests: a regression of case 1 hangs instead of failing; case 4's detection depends on machine speed

`StrataTests/src/Core/BackgroundLogSinkTests.cpp:105`

Verifier: Case 1 is confirmed: if logging became synchronous again, REQUIRE(logged.wait_for(c_Timeout) == ready) throws before inner->Open(). Unwinding then destroys the std::async future, whose destructor joins the task blocked on the closed gate, so StrataTests.Core hangs until CTest's default timeout (no TIMEOUT is set for it) instead of reporting a failure. Case 4's speed dependence is also real, but the finding states its direction backwards. Without the queue bound, the CHECK catches the regression only if the 131,073 calls finish within 200 ms, which needs a fast machine (Release). On a slower run (Debug, a loaded machine), the producer is still running at 200 ms, the timeout status passes the CHECK, and the missing bound goes unnoticed. With the bound present, the test is deterministic.

### F7 OPEN (minor): ThumbnailsMaxFrame was redefined (cold first attempt) without re-measuring Measured; it now runs at 72-83% of budget with occasional outright failures

`StrataTests/Perf/Budgets.json:87`

7390c22's startCold also unloads every project asset before the first attempt (before, the warm-up had loaded the front material's textures), so the metric now measures a heavier state: 61-64 frames instead of 59-61. Measured stayed 0.766. The redefined metric measured 0.829, 0.949 and 0.959 in the final perf-label runs and about 0.80-0.85 (median) in isolated runs. Outright failure (all three attempts over) happened in 1 of 14 thumbfix runs, and 3 of 28 isolated first attempts exceeded the budget (1.31, 1.44, 2.56). The budget did not go up, so the ratchet holds; the stated Measured just no longer describes the metric.

Evidence: perfstability.out: ThumbnailsMaxFrame min 0.8293, median 0.9491, max 0.9588 (83%). ab/thumbfix/test_1.log: 1.2005, 2.2138, 1.1953 ('over the budget in all 3 attempts'). ab/thumb_ab2/base_1.log: 2.5613; base_3: 1.3113; thumb_ab/test_1: 1.4423.

Suggested fix: Re-measure the redefined metric with ctest -L perf and record the new Measured value in Budgets.json, keeping the budget at 1.149, which the ratchet allows. The commit or roadmap should say the first attempt's state changed.

Verifier: Confirmed. 7390c22's startCold also unloads the project's assets, so every attempt streams the textures anew (61-64 frames instead of 59-61), but Measured stayed 0.766. Gate-like runs of the redefined metric (reuse_ab full PerfGPU.*: 0.925, 0.740, 0.750, 1.002, 0.762, 1.046; PerfStability: 0.959, 0.829, 0.949) have a median of 0.925 ms. The isolated thumbfix/thumb_ab runs have a median of about 0.84, with first attempts over budget in 3 of 28 and one thumbfix run over in all three attempts (1.20, 2.21, 1.20). The budget 1.149 is still legal as an acceptance limit (below 1.5 x 0.925 = 1.39), and the ratchet holds, but Measured no longer describes the metric. Defining the metric as the colder state effectively tightened the gate from 1.5x to about 1.24x its median. Fix guidance: Re-measure ThumbnailsMaxFrame with ctest --preset windows-release -L perf (PerfStability --runs 5 on the merge) and set Measured to that median (about 0.9), keeping Budget at 1.149. The ratchet allows that, since the budget does not go up. State in the commit message (or in the roadmap's entry for the round) that the first attempt's state changed (assets unloaded, a colder start), so that the stale 0.766 is not read as a regression.

### F8 refuted: ExportMaxFrameStress shifted about 1.34x in isolated A/B runs; e57138c's own runs showed it but the message omits it

`StrataTests/Perf/Budgets.json:100`

Verifier: Not substantiated. Across the 10 interleaved isolated pairs the medians differ (base 3.99, test 5.35), but the base has more spikes (10.57, 13.68 against the test's 6.81, 8.94). The means are equal (5.71 base, 5.61 test), both builds have 2 of 10 first attempts over budget, and a rank test gives U = 68/100 (z of about 1.36, p of about 0.17), which is not significant. The earlier review's export runs also took place minutes before the machine's 0x1A bugcheck. The gate itself shows no shift: after e57138c, full-suite first attempts were 6.64, 3.19, 3.49, 3.56, 4.25, 3.60 (reuse_ab) and 4.39, 4.20, 6.83 (PerfStability), a median of 4.20 against Measured 4.254. e57138c made no claim about this metric, so leaving it out of the message is not a false claim.

### F9 OPEN (minor): SceneOpenOver1MMaxFrame still fails the gate outright in some runs after the fixes

`StrataTests/Perf/Budgets.json:96`

The round's claims for this metric hold: first attempts over budget went from 5 of 6 to 1 of 6 in the nested_rec pairs, and the roadmap says 6 of 24 overall. But with the fixes, one run still exceeded the budget in all three attempts (3.96, 3.45, 3.13 ms), and the earlier review's runs show 2 of 4 test first attempts over (3.03, 3.65; both confirmed). The roadmap records this as open for wave 4, so the PerfGPU gate remains flaky at a few percent per run from this metric.

Evidence: finish/ab/nested_rec/test_2.log: 3.9575, 3.4502, 3.1276 'over the budget in all 3 attempts'. finish-review/ab/nested test firsts 0.6272, 0.6486, 3.0278, 3.6524.

Suggested fix: Keep it on wave 4's list (scene and document memory off the process heap). Until then, record its outright-failure rate from PerfStability --runs 5 so a regression can be told from the known mode.

Verifier: The flake exists: nested_rec test_2 exceeded the budget in all three attempts (3.96, 3.45, 3.13 ms). It predates the round, though. The 0.3.0 roadmap records all three attempts failing in 2 of 4 full runs, and the round improved it (first attempts over: 5 of 6 to 6 of 24). The round's roadmap entry already records the residual rate, the failed run, the remaining cause (command recording blocked while the parsed document or play copy is freed) and the remedy for later waves (scene and document memory off the process heap). The finding's suggested actions are already in place, so this does not block landing. Fix guidance: No change for this landing. When the merge's PerfStability --runs 5 runs, add its outright-failure count for SceneOpenOver1MMaxFrame to the roadmap entry, and keep the heap work (a registry allocator, an arena per load) on wave 4's list.

### F10 refuted: Verification evidence predates the merge; the release's console and dialog work and the new log writer were never run together

`Strata/src/Strata/Core/Log.cpp:216`

Verifier: This was true of the round's own evidence, but it no longer holds for the merge. The landing workflow built 649d357 (land3/build: Debug, Release and Dist, exit 0) and ran ctest Debug on it from 09:27 to 09:39: 48 of 48 passed (land3/build/LastTest_debug.log). That run includes StrataEditor.Automation (--test-suite=EndToEnd*, 12 cases, among them EndToEnd.StandardStreams) and StrataTests.GPU (GPU*, which includes GPU.Startup), so the release's console, Ctrl+C and dialog tests ran against the background log writer and passed. ReportFatalError logs critical (which flushes synchronously through the writer) and calls Log::Flush() before writing the dialog's text, so the writer does not change what the dialog reads. Release -LE perf and PerfStability on the merge are the workflow's remaining steps, not a defect in the code.

### F11 refuted: Unexplained crash of the round's StrataTests in the earlier review (likely hardware)

`StrataTests/src/Editor/EditorFramePerfTests.cpp:124`

Verifier: No defect can be tied to the code. The SIGSEGV hit the test runner (StrataTests) about 1 s into the nested case, after 5 passing assertions, while it was still writing the generated scenes (Project::Create, WriteNestedScene, WriteCubeScene), before any editor started. The round's code on that path is only the log writer and the job pool size. The pre-round build's editor crashed as well (0xC0000005 at startup in base_1), and the machine bugchecked with 0x1A (MEMORY_MANAGEMENT) minutes later. The finding itself attributes it to hardware, and no dump or repeat implicates BackgroundLogSink or the other changes.

## portability-docs

### D1 OPEN (minor): Docs say the background pool leaves four hardware threads; on machines with fewer than 8 it leaves fewer

`AGENTS.md:461`

AGENTS.md:461 says the background threads 'leave four hardware threads free'. Docs/Architecture.md:841 says 'by default as many as workers but leaving four hardware threads'. The code also keeps at least half of the hardware threads for the background pool. On fewer than 8 hardware threads it therefore leaves fewer than four. On CI's 4-vCPU runners it keeps 2 and leaves 2. JobSystem.h and Docs/Roadmap.md state the rule correctly.

Evidence: JobSystem.cpp:544-549: `half = hardwareThreads / 2; leftToFrames = hardwareThreads > c_FrameHardwareThreads ? hardwareThreads - c_FrameHardwareThreads : 0; return std::max(1u, std::max(half, leftToFrames));` JobSystemTests: GetDefaultBackgroundThreadCount(4) == 2 and (8) == 4.

Suggested fix: Say 'leave four hardware threads to the frame, but keep at least half of them for themselves (JobSystem::GetDefaultBackgroundThreadCount)' in AGENTS.md:461 and Docs/Architecture.md:841.

Verifier: JobSystem.cpp:544-549 sizes the default background pool as max(1, max(hw/2, hw-4)), capped at the worker count (hw-1). With 4 hardware threads that keeps 2 and leaves 2. With 6 it keeps 3 and leaves 3. Only at 8 or more does it leave four. AGENTS.md:461 says the pool leaves 'four hardware threads free', and Docs/Architecture.md:841 says 'leaving four hardware threads'. Both are wrong below 8 hardware threads, which includes CI's 4-vCPU runners. JobSystem.h:128-129 states the rule correctly. Docs/Roadmap.md:488 ('leaves four hardware threads, and at least half, to the frame') reads backwards: it is the background pool that keeps at least half. The code is correct and so are the tests (GetDefaultBackgroundThreadCount(4)==2, (8)==4); only the docs are imprecise. Fix guidance: AGENTS.md:461: write 'which leave four hardware threads to the frame but keep at least half of them (JobSystem::GetDefaultBackgroundThreadCount)'. Docs/Architecture.md:841: write 'by default every hardware thread but four, at least half of them and at most as many as workers'. Docs/Roadmap.md:488: write 'It now leaves four hardware threads to the frame, keeping at least half for itself'. Keep all the information in AGENTS.md, only make it precise.

### D2 OPEN (minor): The threading section does not list the new log writer thread

`Docs/Architecture.md:866`

The 'Threading model' section lists every thread besides the main one (job workers, background, I/O, file watchers, audio, RPC network, script watchdog, process output readers, MCP input). The merge adds BackgroundLogSink's own thread ('Strata Log Writer'), which writes the console and the log file, but the list does not mention it.

Evidence: Docs/Architecture.md:833 'Other threads:' with bullets up to :866 '**MCP input**'. There is no entry for BackgroundLogSink. BackgroundLogSink.cpp:18-26 starts the thread and names it "Strata Log Writer".

Suggested fix: Add a bullet such as '- **Log writer** (`Core/BackgroundLogSink`, started by `Log::Init` when there is a console or log file): writes the console and the log file in the order messages were logged; warnings and errors wait for it (flush); stopped by `Log::Shutdown` and at exit.'

Verifier: Docs/Architecture.md's 'Threading model' section (833-866) lists every thread other than the main one: job workers, background, I/O, file watchers, audio, RPC, script watchdog, process output readers and MCP input. BackgroundLogSink.cpp:18-26 starts a 'Strata Log Writer' thread in every process that logs to a console or a file, and that list does not include it. Only the Core services paragraph (Architecture.md:129-131) mentions the sink's thread. This is a documentation omission; behavior is not affected. Fix guidance: Add a bullet after 'Process output readers' or 'MCP input': '- **Log writer** (`Core/BackgroundLogSink`, started by `Log::Init` when it writes a console or a log file): writes them in the order messages were logged. Warnings and errors wait for it (flush), and so does a thread that logs while 65,536 messages are queued. `Log::Shutdown` and process exit stop it.'

### D3 OPEN (minor): Roadmap gives one value for ImportSettings4kMaxFrame10kCubes; the final runs measured it at up to 92% of the new budget

`Docs/Roadmap.md:489`

The roadmap records the metric as 'measured 2.06 ms, its budget lowered from 4.819 to 3.09'. The finishing round's own final PerfStability run (perfstability.out) measured 1.718, 2.794 and 2.837 ms, the last being 92% of the new budget. The commit's diagnosis with the default pool of 20 threads measured 2.30-2.79 ms. The 3.09 budget, 1.5 times 2.06, leaves about 9% above this second mode near 2.8 ms. The 'Perf to watch' record does not mention it, although it lists metrics at 85-93% of their budgets.

Evidence: perfstability/run1-3.json: Value 1.7176, 2.7939, 2.837 (Budget 3.09). perfstability.out:98 lists max/budget 92%. ab/bgdiag.txt: 'threads 20' gives 2.7912 and 2.2964 ms. The e57138c commit message: 'pool sized ... 20 ... 2.30-2.79'.

Suggested fix: Add the metric to the roadmap's list of perf to watch, with the final runs' 1.7-2.8 ms and 92% of its budget. Alternatively, set its Measured value from the 20-thread runs, since budgets may only go down.

Verifier: Budgets.json:84 has Measured 2.06 and Budget 3.09. With the adopted 20-thread pool the round measured 2.11, 2.04 and 2.03 ms (A/B first attempts, the basis of 2.06), then 2.79 and 2.30 (ab/bgdiag.txt, 'threads 20'), then 1.718, 2.794 and 2.837 in the final PerfStability runs (perfstability.out:98, 92% of the budget). Roadmap.md:486-490 records only 'measured 2.06 ms'. The 'Perf to watch' list ('Unchanged: ... at 85-90% ... up to 93%') leaves this metric out, although 2 of the 3 final runs put it at 90-92% of the budget the round set itself. Architecture.md already says 2.0-2.8 ms, so the record is incomplete rather than wrong. Every run passed on its first attempt, and confirmations protect the gate, so nothing fails. Fix guidance: In Docs/Roadmap.md, either extend the ImportSettings4kMaxFrame10kCubes bullet ('measured 2.0-2.1 ms in the A/B, 1.7-2.8 ms in the final PerfStability runs, up to 92% of the new budget') or add the metric to the 'Unchanged'/watch sentence. Do not raise Budget. If Measured is corrected to a representative value, such as about 2.3, Budget may stay at 3.09, which remains within 1.5x.

### D4 OPEN (minor): Log docs say a log call never waits, but it does when the queue is full

`AGENTS.md:424`

AGENTS.md:424 and Docs/Architecture.md:130 say 'a log call never waits for them, except warnings and errors'. BackgroundLogSink::log also blocks the calling thread while c_MaxQueuedMessages (65,536) messages are waiting. This is deliberate backpressure, documented in BackgroundLogSink.h:31-33 and tested in 'A full queue makes the thread that logs wait until the writer has room'.

Evidence: BackgroundLogSink.cpp:40: `m_ProgressMade.wait(lock, [this]() { return m_Queue.size() < c_MaxQueuedMessages || !m_Running; });`

Suggested fix: Add 'or while 65,536 messages wait to be written (a burst the writer cannot keep up with)' to both sentences.

Verifier: BackgroundLogSink.cpp:40 blocks the logging thread (m_ProgressMade.wait) while m_Queue holds c_MaxQueuedMessages (65,536) messages. This backpressure is deliberate: BackgroundLogSink.h:31-33 documents it and a test covers it ('A full queue makes the thread that logs wait until the writer has room'). AGENTS.md:424 and Docs/Architecture.md:130 nevertheless say a log call 'never waits for them, except warnings and errors', so the docs overstate the guarantee. Fix guidance: In both places write: 'a log call waits for them only for warnings and errors (flushed before the call returns) and while 65,536 messages are queued (a burst the writer cannot keep up with)'.

### C1 fixed in 2c25d45: WriteToSinks' comment cites a closed pipe, which never throws there

`Strata/src/Strata/Core/BackgroundLogSink.cpp:155`

Verifier: The catch in WriteToSinks (BackgroundLogSink.cpp:153-158) cannot be reached by a closed pipe. The Windows stdout_color_sink is wincolor_sink, which ignores WriteFile's result (wincolor_sink-inl.h:157). The WriteConsoleW conversion path that can throw needs SPDLOG_UTF8_TO_WCHAR_CONSOLE, which the build does not define. On POSIX, ansicolor_sink ignores fwrite_bytes' result, and a write to a closed pipe raises SIGPIPE, which the engine does not ignore process-wide (only per call in Process::WriteInput and per socket), so the process ends without an exception. Only the file sink throws (file_helper-inl.h:73, 104: 'Failed writing/flush to file'). The comment's example is therefore wrong; the catch itself is right.

### C2 fixed in 814f89b: FrameTimeRecorder's comment says the vector grows by doubling

`StrataEditor/src/Editor/CommandUtils.h:79`

Verifier: CommandUtils.h:79 says the vector 'grows by doubling' past its reservation. MSVC's std::vector, the one used on the reference platform the measurements come from, grows by 1.5x. libstdc++ and libc++ do double. Code and tests are unaffected: the test checks only that the capacity stays unchanged within the reservation.

### L1 fixed in f2497fd: Logging from a static destructor after Log::Shutdown starts a writer nothing stops

`Strata/src/Strata/Core/Log.cpp:158`

Verifier: The sequence holds but is latent. Here is how it goes wrong. (1) Inside main, the first Log::Init registers StopWriterAtExit with atexit. (2) EntryPoint.h:29 calls Log::Shutdown, which stops the writer, resets data.Writer and sets Initialized=false. (3) Static destruction runs in reverse order of registration, so namespace-scope statics constructed before main (for example ScriptEngine.cpp's s_ActiveEngine) are destroyed after StopWriterAtExit. (4) If such a destructor logs, GetInitializedLogData calls Log::Init() with the default spec (ConsoleOutput=true). That creates a new BackgroundLogSink thread, and since ExitHandlerRegistered is already true nothing ever stops it. Its queued info lines race the process exit (ExitProcess or _exit), whereas before the merge they were written synchronously. Warnings flush and wait for the writer, which is still alive during exe static destruction, so they get through and nothing hangs (the engine is never in a DLL). So the comment 'messages logged later (static destructors) are written by the threads that log them' holds only when Log::Shutdown was not called, as in test processes. No current destructor logs on that path: EditorContext and GameRuntime clear s_ActiveEngine, and ~ScriptEngine does not log. Hence minor.

### S1 OPEN (minor): Doc insertions were not re-wrapped to the paragraphs' 120-column width

`AGENTS.md:462`

Three lines the merge edited were left far beyond the roughly 120 columns their paragraphs wrap at: AGENTS.md:462 (155 characters, 'the frame, whose own work runs on the normal-priority workers). A thread that waits ...'), AGENTS.md:1513 (150, 'across frames: `SceneRenderer`'s environment cache ...') and Docs/Architecture.md:129 (147, 'their own and ignore it). Services: `Log` (spdlog; ...'). Other lines are already ragged in both files, but these come from the merge's own edits.

Evidence: Over-120 lines (tables excluded): AGENTS.md 161 before the merge, 168 after; Docs/Architecture.md 316 before, 319 after; Docs/Roadmap.md 2 before, 5 after.

Suggested fix: Re-wrap the three edited paragraphs at 120 columns. The roadmap's three new 121-character lines can be re-wrapped too.

Verifier: The merge's edits left AGENTS.md:462 at 155 characters, AGENTS.md:1513 at 150 and Docs/Architecture.md:129 at 147, inside paragraphs wrapped at about 120 columns. Counting lines over 120 (tables excluded): AGENTS.md went from 161 to 168, Architecture.md from 316 to 319 and Roadmap.md from 2 to 5. No .md line-length rule exists (.editorconfig has none and clang-format's ColumnLimit is 0), and both files were already ragged, so this is purely cosmetic. Fix guidance: Re-wrap the three edited paragraphs (AGENTS.md threading rule and render-scene caches paragraph, Architecture.md Core services paragraph) at about 120 columns, and optionally the new 121-122-character Roadmap lines. Keep every word of AGENTS.md. Only re-flow it, in line with the user's wish to keep the information there.

## Build and tests of the merge (649d357, before the fixes), Windows

- configure: cmake --preset windows -> exit 0
- build Debug -> exit 0, 0 warnings (113 s)
- build Release -> exit 0, 0 warnings (203 s)
- build Dist -> exit 0, 0 warnings (256 s)
- ctest --preset windows-debug (validation layer on) -> 48/48 passed (668 s)
- ctest --preset windows-release -LE perf -> 47/48 passed (246 s); failed: StrataTests.Core, 1 doctest case of 1692: Asset.PartialResidency 'Cancelled range reads release their place in the streaming queue', AssetPartialResidencyTests.cpp(211) CHECK(InFlightBytes == 0) got 8
- rerun ctest --preset windows-release -R ^StrataTests.Core$ -> 1/1 passed (122 s): flaky
- ctest --preset windows-dist -LE "gpu|perf" -> 35/35 passed (208 s)
- flaky case repeated: unconfined 0/200 alone, 0/400 in 4 parallel loops, Asset.* suites 0/12; confined to 1 CPU 40/40 failed (merge build) and 20/20 failed (main's pre-merge build); confined to 2 CPUs 15/40 failed

The flaky case it found is fixed in bf8bc13.

## AGENTS.md split: author's report

I split AGENTS.md into two files and nothing was lost. The rules stay in AGENTS.md, which is now 33,951 characters instead of 178,635 (the limit is 40,000). All the detail went into a new file, Docs/DeveloperGuide.md (170,639 characters), which agents read when they need it. It is on branch agent/agents-guide in two commits: 2c27374 (the split and the references) and 62af3f1 (the test). I did not build the engine. The new test file needs a configure run before the integration build, and the suite has not been run inside the engine's own build yet.

**The guide.** The moved text is copied line for line, with its original wrapping. The only edits are link targets that had to change because the file is in Docs/. It starts with a short introduction and a table of contents linking all 85 section headings (## and ###), and keeps AGENTS.md's order. The ### headings are new: the old sections had only bold lead-ins, so I added them so each digest can link an exact section.

**AGENTS.md.** These stay whole: the golden rules, the build table, configurations, requirements, options, "Parallel agents", "CI before main", the code style, the review checklist and "Working with third-party code". Each moved section is replaced by a short digest under the same heading: its hard rules, one sentence each, with a link to the guide's section. A "Where the details are" table near the end points common tasks (component, host function, asset type, importer setting, editor command, panel, perf test, shader, release) to the right section.

**Architecture rules are slightly shortened.** To land near 34,000 characters I shortened four rules: threading, NVRHI pitfalls, module registration and the transform contract. Their full original wording is now in the guide's own "Architecture rules" section.

**Proof nothing was lost.** `lossless.py` finds every one of the old file's 1,664 non-blank lines in one of the two new files; it lists none. I kept the old intro lines as they were and added a new paragraph after them. For the checklist I kept item 5's original lines and added a continuation line, rather than rewording them.

**The test.** A new suite, Docs.AgentGuide, runs in StrataTests.Core and fails when:
- AGENTS.md is over 40,000 characters (the message says to move details into Docs/DeveloperGuide.md);
- a link in either file names a file that doesn't exist or is outside the repository;
- a link to a heading in either file names no such heading (GitHub's anchor rules).

It also fails when AGENTS.md links into the guide fewer than 10 times or fewer than 50 links are checked, so it cannot pass by finding nothing. The Python prototype `check_docs.py` passes on the final files (140 links, 37 from AGENTS.md into the guide). I compiled the test file alone with GCC (no warnings, including the conversion and shadowing checks), and ran it alone against a stand-in that copies the behaviour of the engine's file helpers:
- all 6 test cases pass;
- deliberately broken documents produce the right messages;
- a missing file reports its path.

**References.** Every mention of a moved section in the skills, workflows, Docs/Architecture.md, Docs/README.md, README.md, code comments, CMake comments, Budgets.json and PerfStability.py now names the guide section. Mentions of the architecture rules, "CI before main" and the checklist still point to AGENTS.md, since those stayed there.

**Long lines.** Docs/README.md asks for markdown lines of at most 120 columns. Some moved lines are longer, exactly as they were in AGENTS.md; I kept them that way so the move can be checked line by line.

Scratch tools are in `scratchpad/land3/guide/`: `build_docs.py` with its two templates (rebuilds both files from the old AGENTS.md), `lossless.py`, `check_docs.py` and `selftest.py`.

- Added ### headings to the guide (85 ## and ### headings, all in the contents). The old sections had only bold lead-ins, so digests could not otherwise link exact sections. This repeats some lead-ins ('### ABI rules' followed by 'ABI rules:', '### Windows subsystem' followed by '**Windows subsystem:**') because the moved text stays exactly as it was.
- Shortened four architecture rules in AGENTS.md (threading, NVRHI pitfalls, module registration, transform contract) to land near 34,000 characters. Their full original wording is in a new guide section 'Architecture rules', between Testing and Asset pipeline. Renderer conventions, shaders, scripting isolation, assets and layers are unchanged.
- Ended at 33,951 characters rather than ~32,000: cutting further would have meant dropping hard rules the task asked the digests to keep, or shortening code style or layers.
- The guide's 'Repository layout' section holds only the table header and the full rows AGENTS.md shortens (StrataEditor, StrataRuntime, Samples, Tools, Docs). The Docs row is kept as it was, so it does not list the guide; the section's intro sentence says Docs/ also holds the guide.
- Link changes in moved text touch only the target, never the link text: [Docs/Architecture.md](Architecture.md); [Architecture rules](../AGENTS.md#architecture-rules). Links to #testing, #editor, #asset-pipeline and #automation-editor-rpc--mcp resolve inside the guide and are unchanged.
- AGENTS.md keeps its title 'Strata Engine — Agent & Developer Guide'. The guide is titled 'Strata Developer Guide'.
- 'Where the details are' is its own ## section after the subsystem digests, just before the checklist.
- Test file is StrataTests/src/Docs/AgentGuideTests.cpp, suite 'Docs.AgentGuide', found through STRATA_SOURCE_DIR like Architecture.Layering. Character count = UTF-8 code points excluding '\r'. Anchors: ASCII lowercased; ASCII punctuation and the Unicode punctuation and symbol ranges dropped; other non-ASCII letters kept as written (GitHub would lowercase them; no heading here has any). Links are found as '](' plus a target up to ')' outside code spans and fences; web and mailto links are skipped; '/'-rooted targets resolve from the repository root; anchors are checked only for links to AGENTS.md and the guide.
- Commit 1 does not mention the test; commit 2 adds 'Docs.AgentGuide fails beyond' to both intros along with the test, so each commit is consistent on its own.
- Moved text keeps lines over 120 columns, exactly as they were in AGENTS.md, despite Docs/README.md's 120-column rule; a line-for-line move cannot rewrap them. Lines I wrote stay within 120.
- Budgets.json: the first _comment string was split into two array entries to fit the new reference; the file still parses as JSON.
- References to sections that stayed in AGENTS.md (Architecture rules, CI before main, the checklist, the Linux packages in README.md) were left alone. The original brief, GameEngineDoc.md, was not touched.
- Integration: StrataTests/src/Docs/AgentGuideTests.cpp is a new source file, so re-run configure before building.

## AGENTS.md split: fixes after two checks

I applied 29 of the 30 findings in full or in part to branch agent/agents-guide (worktree <repo>/.worktrees/agents-guide) and rejected one, M6(d). All three checks pass: lossless.py finds every one of the 1,664 non-blank lines of the old AGENTS.md in the new AGENTS.md or Docs/DeveloperGuide.md (0 missing); check_docs.py passes (146 links, 39 from AGENTS.md into the guide); selftest.py passes. Nothing was built. The only C++ change is a comment in StrataTests/src/Docs/AgentGuideTests.cpp.

AGENTS.md is 34,965 characters (limit 35,000) and the guide is 174,858. Adding the missing rules would have taken AGENTS.md to about 38,800 characters. To stay under 35,000, I moved more text into the guide, word for word: the build requirements and CMake options, the engine module list, the full .claude/skills/ row, the full Layers rule (AGENTS.md now has a short version), and the editor fonts upgrade steps (new guide section "Working with third-party code"). I also shortened the wording of several digests.

I went over the complete diff against the pre-commit checklist:
- New lines are at most 120 columns.
- No whitespace errors.
- Every new identifier named in AGENTS.md exists in the code (checked with git grep).
- The re-wrap commit changes no words: the word diff shows only `//` comment markers moving.
- Every commit ends with the two trailer lines.

- M1 and F23 (the same issue): applied. The AGENTS.md intro now points to checklist item 5. Item 5, the guide's intro and Docs/README.md all say the same thing: a rule AGENTS.md shortens is stated in full in the guide and a change edits both files; a rule AGENTS.md keeps whole (golden rules, code style, review checklist, the rest of building, architecture and third-party) appears only in AGENTS.md; details go only into the guide.
- M2 and F19 (the same issue): applied. The test rule now reads: use Tests::CreateTemporaryDirectory() for files, never the source tree; never change the process environment for a child; depend on no program outside the build, with the test binary's helper modes as stand-ins.
- M3: applied in its own commit, b86d46e. Each paragraph is re-wrapped only up to the line where it matches its old wrapping again. Long lines that remain were already there before the split.
- M4: applied the alternative fix, not the re-wrap. Docs/README.md now says the guide keeps the wrapping of the text it took from AGENTS.md, so that `git blame -C` follows those lines back into AGENTS.md's history. Re-wrapping would break that and would make lossless.py report every moved line as missing.
- M5: applied as a comment on CheckLinks that states the limitation. The check itself is unchanged: CI's Linux job catches links with the wrong case, and nothing may be built in this round.
- M6 (a), (b), (c): applied. (a) The threading digest now says the four hardware threads are left to the main thread, the driver and the frame's helpers. (b) Start-Process -Wait added. (c) The guide's Feature test section opens with a new sentence linking golden rule 6; the moved line keeps its plain text so it stays identical to the old one.
- M6 (d): rejected. I did not rewrite the existing commits. Instead, 1bdd505's message states the correct figure: 178,635 is the old file's size in bytes, 178,605 in characters. The short-rule count is fixed in the guide ('five of them in short').
- F1: applied. The Editor digest now has the ResolveOutputPath rule (relative to the project, network and device paths refused, overwrite only with `overwrite: true`), and Output paths is in the editor-command row of the table. There is no link on the bullet itself, to save space.
- F2: applied. New Asset pipeline bullet: AssetDependencies::Scan or ImportResult::References, each valid handle once and never the asset itself (NormalizeReferences); pruned exports, delete checks and prefetches depend on them. The guide's 'Adding an asset type' gets a new paragraph pointing to References and bounds and to the sub-asset handle rule, and the table's asset-type row lists both.
- F3: applied. The importer bullet now requires sub-asset handles from DeriveSubAssetHandle, stable across re-imports and machines.
- F4: applied. The commands bullet now covers EditorCommandResults::Session, rejecting out-of-range values, Track before changing and TrackCreated after creating, EditorActionGroup, and the warning plus RunningCopy while playing. The table row now lists Command conventions, Undo, Edits while playing, Deferred commands, No silent data loss and Output paths.
- F5: applied. UI code passes `force` only once the person has answered EditorShell::RequestDiscardChanges, and only for the changes they were asked about.
- F6: applied. Steps that change files check them first and fail with the reason when they no longer fit, never overwriting; asset steps never mark the scene unsaved.
- F7: applied. Added: deferred commands via RunCommandToCompletion, clicking in a tab only once it shows, custom components in a child process, never changing the environment for a child, TextureStreaming = false and AddNeutralPostProcess for exact-pixel tests, SetPartialLoads(false), rendering until no glyph is pending, and checking that a new regression test fails without its fix.
- F8: applied. New Testing bullet: expectations that differ in Dist use #if defined(ST_DIST); CI tests Release and only the release steps run the tests in Dist.
- F9: applied, with one trade-off. Added 'a script class never runs' and that SDK calls count only on receivers whose type the reader knows. To stay under 35,000 I removed the digest's sentence on how to extend the feature test. Each failure condition already says what to add, and the link now reads 'Feature test: how to extend it'.
- F10: applied. Added ReadImportSource, FindExternalFiles, MakeValidUTF8, and bumping GltfImporter along with TextureImporter when cooked textures change; editor code re-imports in the background (blocking forms are for tools and tests).
- F11: applied. New table rows for streaming/residency (Residency, Mip streaming, Architecture.md 'Streaming and residency') and for shaders/the renderer (Render scene, Batching, Skinned meshes, Streaming and uploads). The asset-type, editor-command and panel rows are wider (the panel row adds Shortcuts and editor status). The mip-streaming rule and its link are in the asset digest. I removed the rows for host functions, importer settings, perf tests and releases, because their digest bullets link exactly those sections.
- F12: applied. New Rendering bullet: ST_SKINNED variants in every pass (prepass and forward pass agree to the bit), mirrored instances get clockwise pipelines and flipped tangents, normals come from TransformNormal. Also added: staging copies run before SubmitBatch, and PrepareBackBuffer is called first.
- F13: applied. 'Projects never choose what runs': the external editor is the user's setting or STRATA_EXTERNAL_EDITOR and starts without a shell; script.open opens only code, CMake and text files of the project or engine. The batch-file detail is left to the guide.
- F14: applied. Clients check the server's proof first, only the handshake is served before authentication, and the project's .strata/EditorSession.json is untrusted and never holds the port or token. Not added: that a client never switches editors silently. That rule is about client behaviour and stays in the guide.
- F15: applied, all items: CheckSceneEditable while a scene opens, PrefabSync::SetBaseline, PrefabInstances::MarkSynced, EditorActionGroup, GetSceneChange (otherwise recovery snapshots the whole scene), RouteFocused selection shortcuts, Scene::ReleaseInBackground, and bumping ThumbnailCache::c_Version.
- F16: applied. New Scripting bullet: save files go through IGameSaves, names pass SaveFiles::ValidateName before any path is formed, writes are atomic, and in-process runs provide IGameWindow and IGameSaves.
- F17: applied in part. Added: commits that add or lower a budget state the measured values, lower is better, say why each metric uses its statistic, and measure peak memory in a helper process. Not added, for space: GPUContext::ExecuteAndWait (it stays in the guide's Perf tests).
- F18: applied. Callbacks are appended to StrataScriptClassDesc and read only when its StructSize covers them.
- F20: applied. The caches are checked after every update, the render proxies after every RenderScene::Sync.
- F21: applied (same change as M6b).
- F22: applied. 'Other engine modules reach animation only through scene services'; only the Engine and AssetPipeline layers include Animation.
- F24: applied in part. Restored the clear call names, the reason MeshRendererComponent writes need a signal (render proxies and mesh colliders follow on_update), and that unknown components survive play mode, prefab snapshots, duplication and undo. The validator names and why overrides must be public stay in the guide.
- To make room: AGENTS.md's Packages and Releases paragraphs became one digest. The StrataEditor, StrataRuntime, Samples, Tools, Docs and .claude/skills rows are shorter, with full rows in the guide. The NVRHI, module-registration and transform rules are tighter, with full wording already in the guide. The code style section is unchanged.
- README.md: the Linux development packages are now listed in Docs/DeveloperGuide.md ('Building'), so README.md points there instead of AGENTS.md.
