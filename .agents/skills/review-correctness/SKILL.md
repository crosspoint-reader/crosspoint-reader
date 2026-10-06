---
name: review-correctness
description: Read-only firmware-handoff review for regressions introduced or made reachable by the current diff; require concrete failure paths.
---

# Correctness review

Review only the pinned logical change. Do not edit files or broaden the task into a general subsystem audit.

For every changed behavior:

1. Trace inputs, state transitions, ownership, cleanup, and error paths through real callers.
2. Check boundaries, empty and failure cases, lifecycle transitions, stale state, and interaction with unchanged callers.
3. Require a reachable scenario. A suspicion is not a finding without the input or prior state, execution path, and wrong result.
4. Distinguish a bug introduced or exposed by the diff from a pre-existing issue. Put pre-existing issues in a short separate note only when the change materially exposes them.
5. Check repository history when behavior or intent is ambiguous.

Report each finding with severity, `file:line`, triggering scenario, wrong result, and smallest credible fix. Collapse multiple symptoms of one bug. A clean result is normal.

**Done when:** every behavioral hunk is accounted for and every reported bug has a concrete reachable failure path.
