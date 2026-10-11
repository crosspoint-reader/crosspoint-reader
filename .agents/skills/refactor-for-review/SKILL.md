---
name: refactor-for-review
description: Keep structural changes reviewable. Use when restructuring code, extracting helpers, or separating unrelated cleanup.
---

# Refactor for Review

This is a multi-contributor, AI-assisted codebase, and the dominant failure mode
is the sprawl diff: a one-line intent that touches thirty files. The goal is a
change a reviewer can verify in one sitting. Cleaner structure that makes the
next change easier is the win, not lines added.

## One concern per change

- A commit/PR does one thing. A bug fix is not also a rename is not also a
  reformat. If you spot an unrelated improvement mid-change, leave it or capture
  it separately; do not fold it in.
- When the working tree has bundled two changes, separate them with the
  copy-affected-files-aside, reset, re-apply one concern, restore the rest
  pattern, not by committing the tangle.
- Keep unrelated refactoring separate from behavior changes. A small structural
  change needed for the requested fix may stay with it; explain why it is needed.
  A change described as a pure refactor must preserve behavior.

## Keep the diff narrow

- For a bug fix, trace the triggering input and state through every affected
  caller before editing. Fix the shared cause where those callers route;
  account for callers with different contracts instead of copying guards into
  individual screens.
- Reuse existing helpers and interfaces before adding a wrapper, factory, or
  configuration option. Keep a new abstraction only when it owns a real
  contract or hides a demonstrated implementation choice.

- Extract a helper to remove real duplication or to name a concept, not to chase
  abstraction. Three-plus copies, or a block that needs a name to be understood:
  extract. Two similar lines: leave them.
- No "while I'm here" scope creep. A signature or type change that ripples to
  many call sites is its own PR: map every caller first, update them in one
  topological pass, and land it separately, not as a rider on a feature.
- Match the surrounding code: comment density, naming, idiom. The diff should
  read like the file, not like a different author.

## Decompose oversized units

An activity or function that has outgrown one screen of responsibility (multiple
unrelated state machines, or a file far larger than its siblings) is a
decomposition candidate. Extract a cohesive sub-responsibility into its own
unit, as a standalone behavior-preserving refactor, verified on its own, never
mixed into a feature change.

## Preserve requirements and verification

The smallest diff must still meet the full requirement. Preserve trust-boundary
validation, data-loss prevention, security, accessibility, and required hardware
calibration. For changed non-trivial behavior, use the smallest meaningful
regression check in the existing test infrastructure and the relevant device
checks; follow the [testing rule](../../rules/testing-debugging.md).

## Comments earn their place

Apply the [comment rules](../../rules/coding-standards.md#comment-style).

## Self-review before handoff

- [ ] The change does exactly one thing; nothing unrelated rode along.
- [ ] Structural changes serve the requirement; unrelated refactoring is separate.
- [ ] No "while I'm here" creep; rename/signature ripples are split out.
- [ ] Extractions remove real duplication or name a real concept, not
      speculative abstraction.
- [ ] The fix accounts for affected callers; simplicity preserves the required
      failure handling and behavior, with relevant regression checks.
- [ ] Added or changed comments are short and useful without the diff or PR history.
- [ ] A reviewer can understand the diff without running it.
