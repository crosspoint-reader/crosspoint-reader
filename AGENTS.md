# CrossPoint Reader agent guide

CrossPoint Reader is open-source e-reader firmware for Xteink and other FreeInk-supported devices. Its mission is a lightweight, high-performance reading experience focused on EPUB rendering. The X3/X4's ESP32-C3 remains the resource baseline: roughly 380 KB usable RAM, no PSRAM, and a single framebuffer sized for the selected panel. Other board profiles have different CPUs, display sizes, input, and memory capabilities; check the selected profile before assuming them.

## FreeInk SDK

The [FreeInk SDK](freeink-sdk/) supplies the board profiles, hardware drivers,
and shared UI components. Before adding an API or device-specific code, check
the firmware HAL and the SDK source at the revision pinned by this repository.
Use the [SDK documentation](freeink-sdk/docs/README.md) and
[FreeInk SDK index](https://freeink.org/llms.txt) to find the relevant contract.
Check `git submodule status`: a different local SDK revision must be accounted
for, not silently treated as the pinned firmware dependency.

## Start here

At session start, run `uname -s`, `git branch --show-current`, `git remote -v`, and `git status --short`. Integration work targets `develop`.

Act as a senior embedded C++ engineer. Base claims on repository evidence: cite the paths and line numbers that justify a proposed change. Explain the mechanism behind performance or memory claims and justify every new heap allocation. For every fix, tell the human how to verify it.

Read only the rules that match the task:

| When the task touches... | Read |
| --- | --- |
| Host setup, PlatformIO usage, or local configuration | [environment.md](.agents/rules/environment.md) |
| RAM, allocation, flash, strings, or hardware limits | [hardware-resources.md](.agents/rules/hardware-resources.md) and the `heap-discipline` skill |
| Build flags, storage, input, display, settings, i18n, rendering, or SDK boundaries | [architecture-hal.md](.agents/rules/architecture-hal.md) and the `hal-and-abstractions` skill |
| C or C++ implementation | [coding-standards.md](.agents/rules/coding-standards.md); also load `control-flow-clarity` for branching or state changes |
| Activities, orientation, buttons, UI, tasks, fonts, or lifecycle | [ui-activities.md](.agents/rules/ui-activities.md) |
| Plugins, service integrations, web endpoints, or protected books | [architecture-hal.md](.agents/rules/architecture-hal.md) and its contract links |
| Builds, formatting, CI, serial logs, crashes, or verification | [testing-debugging.md](.agents/rules/testing-debugging.md) |
| Git, branches, commits, remotes, or publication | [git-workflow.md](.agents/rules/git-workflow.md) |
| Generated HTML/i18n, caches, EPUB formats, or invalidation | [generated-files-cache.md](.agents/rules/generated-files-cache.md) |
| New features, activities, settings, libraries, or dependencies | `SCOPE.md` and the `scope-discipline` skill |
| Refactoring or preparing a change for review | the `refactor-for-review` skill |

Repository-local skills live under `.agents/skills/`. Load a skill when its frontmatter description matches the task; do not load every skill speculatively.

## Human ownership

A PR is a long-term maintenance commitment. Working code is not enough: prefer the simplest design that meets the real requirement, fits `SCOPE.md`, and can be understood and maintained by its human owner.

Fully autonomous end-to-end agents are forbidden. Review subagents under the main
agent's supervision may inspect code, diffs, history, and build metadata only; they may
not edit, commit, push, open/close PRs, post reviews, release, deploy, or flash.

The human must write PR descriptions; agents may give concise factual notes and test
results, never ready-to-paste PR prose. Creating/amending local commits requires
explicit human approval. Push only on an explicit human instruction to push;
edit/commit approval does not authorize it. Never open or close a PR.

Repository-facing prose: use plain English for non-native readers and standard
technical terms when clearest. Code comments must be short and useful after merge; follow the
[comment rules](.agents/rules/coding-standards.md#comment-style).

## Mandatory firmware handoff

For every logical change that can affect shipped firmware or its build—including C/C++, build configuration, partitions, code-generation sources, translations, and release scripts—the main agent must complete [.agents/skills/firmware-handoff/SKILL.md](.agents/skills/firmware-handoff/SKILL.md) before declaring the work ready or making an approved local commit.

Pure tests, diagnostics, documentation, and host-only Python scripts use a lighter review unless they alter firmware output or its build.

This concise handoff checklist is a hard requirement:

- [ ] Relevant tests and `./bin/clang-format-fix -g` completed; firmware built once after the final code edit.
- [ ] Read-only reviews completed for correctness, architecture, embedded constraints, and i18n/user documentation.
- [ ] The main agent verified, deduplicated, and fixed findings, then reran affected reviews after material fixes.
- [ ] The main agent explained the behavior and architecture in plain English to someone unfamiliar with the codebase.
- [ ] The human was told to review the diff and explicitly confirmed understanding of the behavior and architecture and ownership of maintenance.
- [ ] The agent gave a concrete hardware test plan and reminded the human that hardware testing is required before a PR can be opened.
- [ ] The agent did not claim hardware verification and did not write a PR description.

If the human rejects the architecture, stop the handoff, ask what must change, and revise before calling the work ready. Hardware testing is entirely the human's responsibility; explain what to test and what failures to watch for.
