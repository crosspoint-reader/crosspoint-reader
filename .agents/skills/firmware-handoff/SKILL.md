---
name: firmware-handoff
description: Mandatory final checks, four independent read-only reviews, and human handoff for every logical change that can affect shipped CrossPoint firmware or its build.
---

# Firmware handoff

Run this once after the logical firmware change is complete and before calling it ready or making an approved local commit. The main agent owns the result; reviewer output is evidence, not authority.

## 1. Pin the change

Record the intended requirement, comparison base, changed files, and diff. Account for every hunk. Include C/C++, build configuration, partition data, code-generation sources, translations, and release scripts that can change shipped firmware.

**Done when:** one sentence states the requirement and every hunk belongs to it or is identified as unrelated user work.

## 2. Run main-agent checks

Run relevant tests and `./bin/clang-format-fix -g`. Build the relevant firmware target once after the final code edit. Follow [.agents/rules/testing-debugging.md](../../rules/testing-debugging.md); do not repeat a green target or rebuild after formatting, comments, or documentation alone.

**Done when:** commands and outcomes are recorded, or each unavailable check has a concrete reason.

## 3. Dispatch independent reviews

Launch a separate independent read-only subagent for each axis below. Schedule
reviews in batches that fit the available slots, reserving a slot for the main
agent. Never substitute the main agent for a reviewer:

- [review-correctness](../review-correctness/SKILL.md)
- [review-architecture](../review-architecture/SKILL.md)
- [review-embedded](../review-embedded/SKILL.md)
- [review-i18n-docs](../review-i18n-docs/SKILL.md)

Give each reviewer the requirement, comparison base, diff, and repository path. Reviewers inspect only: they do not edit, commit, push, publish, or flash hardware.

**Done when:** all four independent subagents have returned concrete findings
or a clean result.

## 4. Resolve findings

Collapse duplicates by root cause. Verify every finding against reachable code and repository evidence. Fix verified problems yourself. Explain why rejected findings do not apply; keep unresolved disagreements visible. After a material fix, rerun every review axis the fix could affect, then rerun relevant checks and build once after the last code edit.

**Done when:** no verified finding remains unaddressed and the main agent is satisfied with the whole diff.

## 5. Hand off to the human

Explain the old behavior, new behavior, architecture, affected files, and remaining risks in plain English for someone unfamiliar with the codebase. State checks run and any unresolved disagreement. Tell the human to review the diff and ask them to confirm that they understand the behavior and architecture and accept maintenance ownership.

Give a concrete hardware test plan: actions, expected results, relevant orientations, resource or cache observations, and likely failure signs. Remind the human that hardware testing is their responsibility and must happen before a PR is opened. Never claim hardware verification yourself.

Do not write a PR description. You may provide concise factual notes. Create or
amend a local commit only after explicit human approval. Push only when the
human explicitly instructs you to push; editing or commit approval does not
authorize it. Follow the [Git rule](../../rules/git-workflow.md) for remote selection.

**Done when:** every hard checklist item in the root `AGENTS.md` is satisfied. If the human rejects the architecture, stop and revise it before calling the work ready.
