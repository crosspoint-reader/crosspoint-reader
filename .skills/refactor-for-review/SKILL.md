---
name: refactor-for-review
description: Producing small, single-concern, reviewable changes. Use when refactoring, cleaning up, restructuring, decomposing, or preparing a change for PR, especially in this multi-contributor AI-assisted codebase that is prone to sprawl diffs. Covers one-concern-per-commit, extracting helpers without widening scope, not bundling unrelated edits, decomposing oversized activities, comment hygiene, and a pre-handoff self-review checklist.
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
- Refactor and behavior change do not ride together. A pure refactor must not
  alter behavior; a behavior change should not drag a refactor along. If both
  are needed: two commits, refactor first.

## Keep the diff narrow

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

## Comments earn their place

Comments explain why: an invariant, a defense, a past incident, a non-obvious
constraint. Never what the next line already says. Delete narration, phase-marker
comments ("now we loop over..."), and restated function names. If a comment and
the code it sits on say the same thing, the comment is the thing to cut.

## Self-review before handoff

- [ ] The change does exactly one thing; nothing unrelated rode along.
- [ ] Refactor and behavior change are not mixed in one commit.
- [ ] No "while I'm here" creep; rename/signature ripples are split out.
- [ ] Extractions remove real duplication or name a real concept, not
      speculative abstraction.
- [ ] New comments say why, not what; no narration or phase markers.
- [ ] A reviewer can understand the diff without running it.
