---
name: review-architecture
description: "Read-only architecture audit of the current logical diff at firmware handoff: assess its long-term shape."
---

# Architecture review

Assume the change can work. Review its shape, not correctness, style, security, or performance. Do not edit files.

## Process

1. **Pin intent.** State the human requirement in one sentence from the conversation, issue, human discussion, diff, and history. Identify hunks the requirement does not explain.
2. **Find roots.** For each hunk, identify the design decision it belongs to. Collapse guards, helpers, call-site edits, and new parameters that express one decision. Cap the report at three roots.
3. **Find frozen constraints.** Ask what the change treated as immovable and who requires it. Look for downstream compensation, escape hatches, flags, duplicated ownership, stored derived state, layer bypasses, representation leaks, compatibility shims, and policy in the wrong layer.
4. **Do the legwork.** Steelman the chosen design. Enumerate callers of symbols an alternative changes. Check `git log -S` and blame when intent or a constraint is unclear.
5. **Compare alternatives.** For decisions with avoidable complexity, describe the smallest solution and the right-shaped solution, including tradeoffs. If they are the same, say so. A sound decision needs no invented alternative. Do not propose churn that merely moves complexity or ignores a real constraint.
6. **Judge churn separately.** The panel is advisory. State whether the current shape is sound, oversized, needs rework, or solves the wrong problem, but do not decide the handoff.

Report plain English first, assuming the reader does not know the codebase. Then give concise evidence with `file:line`, affected symbols, future-change cost, and alternatives. A clean audit is normal.

**Done when:** every hunk is attributed to an architectural root or ordinary necessary work, and each proposed alternative accounts for all affected callers.
