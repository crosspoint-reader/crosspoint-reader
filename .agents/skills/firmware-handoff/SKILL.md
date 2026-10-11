---
name: firmware-handoff
description: Final checks and human handoff for completed firmware/build changes. Use after implementation and self-review; excludes read-only audits and reviewer tasks.
---

# Firmware handoff

Run after implementation and main-agent self-review, before calling a firmware
change ready or making an approved local commit. The main agent owns the result;
reviewer output is evidence, not authority. This workflow has at most two review
rounds per logical change, even when slot limits require dispatching in batches.

## 1. Prepare the review candidate

Record the human requirement, comparison base, changed files, and current diff,
including uncommitted work and any SDK changes. Account for every hunk and
identify unrelated user work. Finish self-review, relevant tests, and
`./bin/clang-format-fix -g` before dispatch. Build early only when needed to
resolve a concrete compilation or build-configuration question; the required
final build belongs after review fixes.

**Done when:** the candidate meets the requirement, every hunk is accounted for,
and checks run or unavailable checks with their reasons are recorded.

## 2. Dispatch the initial reviews

Launch a separate independent read-only subagent for each axis, reserving a slot
for the main agent. Never substitute the main agent for a reviewer:

- [review-correctness](../review-correctness/SKILL.md)
- [review-architecture](../review-architecture/SKILL.md)
- [review-embedded](../review-embedded/SKILL.md)
- [review-i18n-docs](../review-i18n-docs/SKILL.md)

Use fresh reviewer contexts where supported. Give each a compact packet: human
requirement and constraints, repository path, comparison base, pinned diff and
revision, and its review axis. Supply necessary human discussion without the
main agent's analysis or verdict. Reviewers read relevant rule sections; they do
not edit, run implementation handoff, commit, publish, or flash hardware.

Keep the candidate unchanged while reviews run. Collect all four results before
editing. A reviewer with no applicable changes may return a short explanation.

**Done when:** all four initial results are recorded against the same candidate.

## 3. Resolve findings as one batch

Collapse duplicates by root cause and verify findings against reachable code.
Explain rejected findings and keep unresolved disagreements visible. Decide
which advisory cleanups belong to the requirement; defer unrelated improvements.
Apply all accepted fixes together, then verify the whole diff yourself.

Allow one targeted follow-up round only when fixes invalidate a review's
conclusions about behavior, ownership, resource costs, interfaces, persistent
formats, build compatibility, or user-facing information. Reuse the original
reviewers, giving them the diff since their reviewed candidate and the findings
to verify. Mechanical corrections, formatting, and comment-only changes receive
main-agent verification unless they change a substantive contract. Record which
final changes each review covers.

After this second round, fix verified problems and run affected checks. If a
substantive concern or independently unreviewed material change remains, report
it and leave handoff incomplete rather than starting an automatic third round.

**Done when:** no verified finding remains unaddressed and independent reviews
cover the final substantive changes within the two-round limit.

## 4. Complete final checks

Run formatting and any affected tests not already green for the final change.
Build each relevant firmware target once after the final substantive firmware or
build edit, including configuration, code-generation sources, and translations.
Follow the [testing rule](../../rules/testing-debugging.md). Preserve green
results after formatting, comments, or documentation alone; repeat a check only
when a change or failure invalidates it. Record commands, outcomes, and concrete
reasons for unavailable checks; an unavailable check is not a pass.

**Done when:** relevant tests, formatting, and final firmware builds have passed,
or missing verification is explicitly reported and handoff remains incomplete.

## 5. Hand off the actual design

Explain old and new behavior, state/resource ownership, affected files, the
important tradeoff, checks, and remaining risks in plain English. Point to the
diff. Ask the human to review it and explicitly confirm understanding of the
behavior and architecture and acceptance of maintenance responsibility, without
prescribing a confirmation phrase. Tie the request to a concrete design decision
or tradeoff. Recognize explicit acceptance already given for this logical change;
record it once and reopen it only if the design materially changes. Continuation
or commit approval alone does not establish understanding and ownership.

Give applicable hardware steps with expected results and likely failure signs,
including orientations, resources, and caches when affected. Hardware testing is
the human's responsibility and must happen before a PR is opened. Never claim
hardware verification yourself. Publication follows the root guide's human
ownership policy and the [Git rule](../../rules/git-workflow.md).

**Done when:** checks and independent reviews cover the final change, no verified
finding remains, the human has explicitly accepted understanding and ownership,
and the hardware test plan is provided. If the human rejects the architecture,
ask what must change and revise before calling it ready.
