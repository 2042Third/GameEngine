# Wave 4 plan

Written on 2026-10-13 from five audits of main at 685aef7 (the end of wave 3), before wave 3's finishing round and the
release work landed. Wave 4 has not started.

| File | What |
| --- | --- |
| `plan.md` | The plan: the six streams, step 0 (what lands before they branch), every audit finding and its owner, merge order. Read first. |
| `roadmap.md` | The section for Docs/Roadmap.md ("### Wave 4"), put there when wave 4 started. |
| `specs/common.md` | What every stream's agent needs to know: Strata after wave 3, shared-file ownership, the report format. |
| `mac.md` | What changes on the Mac the work moved to: the machine, the tools, perf without the reference machine, MoltenVK's GPU features, what cannot be done there. Read after `specs/common.md`; it overrides what they say about these. |
| `specs/<stream>.md` | One spec per stream: `engine-modules`, `world-io`, `world-partition`, `gpu-driven-rendering`, `game-ui`, `authoring-and-playtest`. Each lists what to deliver first if time runs short. |
| `specs/notes/` | The spec writers' working notes (decisions and what they checked). |
| `audits/<lens>.md` | The audits' notes: person, agent, rendering, scale, architecture. Their evidence (logs, captures) stayed on the old machine; `plan.md` carries every finding the streams need. |

Since they were written:

- **Step 0's branches in flight** landed: `agent/release` as 0.3.0, `agent/wave3-finish` through `agent/integration`.
- **The work moved to a Mac** on 2026-10-13: `mac.md` says what changes (presets, tools, perf without the reference
  machine, MoltenVK). `<scratch>` and `<repo>` name folders of the old machine.
- **AGENTS.md was split** into the rules (AGENTS.md) and the details (Docs/DeveloperGuide.md). Where a spec says to
  document something in AGENTS.md, rules go there and details into the guide.
