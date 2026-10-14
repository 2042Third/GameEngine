# Wave 4 on the Mac

Wave 4's plan and specs were written for the Windows reference machine. Since 2026-10-13 the work runs on a Mac. This
file overrides what they say about the machine, the tools and the perf gates; everything else in them stands. Every
stream's agent reads it after `specs/common.md`.

## The machine

- Apple M2 Max (8 performance and 4 efficiency cores, 12 threads), 96 GB, macOS 26, Apple Clang 21, CMake 4.1, Ninja,
  Python 3.
- Vulkan runs on Metal through MoltenVK 1.4.2 (Homebrew: `molten-vk`, `vulkan-loader`, `vulkan-validationlayers`).
  Every test of the `gpu` label passes on it.
- Your worktree is `.worktrees/<key>` on `agent/<key>`. Build there, in `build/macos`: `cmake --preset macos`, then
  `cmake --build build/macos --config Debug --parallel`. Test presets: `macos-debug`, `macos-release`.
- The validation layer: `export VK_ADD_LAYER_PATH=/Users/mikeyang/Documents/gameengine/build/_tools/vulkan-layers` (a
  copy of Homebrew's manifest that names its library by its full path, Docs/DeveloperGuide.md, "Building"). Without it,
  Debug runs warn that the layer could not be loaded and run without it, and validation errors go unseen.
- Several agents share 12 threads: builds and test runs of several streams at once are slow. Run one full suite of
  your own at a time.
- Scratch files go to `build/scratch` in your worktree, never into the source tree.

## Tools

The old machine's scratch tools are gone; these replace them.

- **CI-compiler lint** (replaces `gcc_lint.sh` and `compat_check.py`): `python3 Tools/CompatLint.py --changed-since
  <base>`, after a Debug build. It compiles the changed files with GCC 14 (CI's Linux compiler) and LLVM 16 with its
  libc++ (the closest to CI's Apple Clang 15), with warnings as errors, and must come back clean. Platform code
  (`src/Platform`) is compiled by LLVM 16 only; its Linux paths compile in CI. A changed header: pass the files that
  include it, or `--all`.
- **Brace check:** none; `.clang-format` states the style, and the review reads it.
- **Perf lock:** `python3 Tools/PerfLock.py --label <key> -- <command> [arguments...]`. It waits up to 30 minutes for
  the lock (then exits 75), ends a command after 25 minutes (exit 124), and `--status` names the holder.
- **CI:** the orchestrator pushes `ci/<key>`; read the results with `python3 -I Docs/Handoff/tools/ci_status.py <sha>`
  and `ci_annotations.py <sha>`, or `gh run view`.

## Perf on the Mac

- `StrataTests/Perf/Budgets.json` holds the reference machine's measurements. On the Mac, `-L perf` informs; it is not
  the gate. Never change an existing entry from Mac numbers, never raise one.
- A new metric gets a provisional entry, `{ "Unit": ..., "Provisional": true }`, with the spec's acceptance limit as
  its `Target` when it has one: its checks measure it once, print and record it, and never fail. Report its Mac values
  and A/B ratios; the reference machine sets `Measured` and `Budget` later (Docs/Roadmap.md lists what waits for it).
- A/Bs as `specs/common.md` section 9 says: interleaved, at least 3 pairs, under the perf lock, against the base build
  (the step-0 tip's Release build; the orchestrator gives its path), judged by the ratio of the medians.
- An acceptance criterion stated in the reference machine's absolute numbers cannot be proven here: report the Mac value
  and mark it "deferred to the reference machine". A criterion stated as a ratio to the base, a count or an exact
  property is proven here.

## The GPU (gpu-driven-rendering above all)

Measured with `vulkaninfo` on this Mac:

- **Missing:** `drawIndirectCount`, `samplerFilterMinmax`, geometry shaders, mesh shaders, pipeline statistics queries,
  64-bit buffer atomics, a dedicated transfer queue.
- **Present:** `multiDrawIndirect` (`maxDrawIndirectCount` 2^30), `drawIndirectFirstInstance`, `shaderDrawParameters`,
  timestamps on graphics and compute (`timestampPeriod` 1 ns), calibrated timestamps, buffer device address, subgroups
  of 32, 64-bit integers, 16-bit integers and floats, scalar block layout.
- **Limits:** 31 storage buffers per shader stage, 32 KiB of compute shared memory, 1,024 invocations per work group,
  `maxPerSetDescriptors` 1,212 (the bindless table is fine: Docs/DeveloperGuide.md, "macOS and MoltenVK").

So:

- The indirect path without a GPU count (culled draws with no instances, zeroed tails) is this machine's `Auto` path and
  the path tested here. The `drawIndirectCount` path must exist, be chosen by the device's features and be covered by
  the tests that force each mode where the device supports it; report it as untested here.
- The depth pyramid reduces with explicit gathers here; the min/max sampler path is untested here.
- Every new pass stays within 31 storage buffers per stage and 32 KiB of shared memory.
- GPU timestamps: before gates rely on them, check that timestamps written between passes inside and outside rendering
  passes give consistent, non-zero intervals on this driver, and report what you found.

## What cannot be done here

Each stream's report lists these as deferred, with what it measured on the Mac:

- budgets of new metrics (provisional entries) and acceptance limits in the reference machine's absolute numbers;
- GPU tests on NVIDIA and Windows: the `drawIndirectCount`, transfer-queue and min/max-sampler paths;
- Windows-only behavior (the Visual Studio generator's script builds, console handling, executable icons), which CI
  covers as far as its Release tests reach;
- the visual pass at 150% on a Windows display; on the Mac, check at `--ui-scale 1` and at the display's 2.

## Reading the specs

- `windows-*` presets are `macos-*` here; `<repo>` is `/Users/mikeyang/Documents/gameengine`; `<scratch>` is your
  worktree's `build/scratch`.
- AGENTS.md was split: the rules stay in AGENTS.md (under 40,000 characters, checked by `Docs.AgentGuide`), the details
  are in Docs/DeveloperGuide.md. Where a spec says to document something in AGENTS.md, rules go there and details into
  the guide.
