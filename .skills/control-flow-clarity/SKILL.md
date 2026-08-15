---
name: control-flow-clarity
description: Branching and state-modeling clarity in C/C++. Use when writing or refactoring logic that branches on discrete values: if/else-if ladders, status flags, mode or state ints, or anything that should be an enum plus an exhaustive switch. Covers enum class over magic ints, exhaustive switch over nested if, early-return guard clauses, table dispatch, and when each is the right call.
---

# Control-Flow Clarity

The goal: a reviewer verifies correctness by reading, not by tracing. Branching
that mirrors the problem's shape is self-evident; branching that encodes it in
ad-hoc ints and nesting forces the reader to reconstruct intent.

## Core moves

- **Model a closed set of states/modes as an `enum class`, not ints or bools.**
  A variable kept honest by a comment ("0 = hidden, 1 = showing, 2 = confirm")
  is a latent bug. Make it an enum and the comment becomes the type.
- **Dispatch on an enum with an exhaustive `switch`, no `default`.** This
  codebase relies on it: omitting `default` lets the compiler flag the
  unhandled case when someone adds an enum value. A `default:` that swallows the
  unknown case throws that safety away. Add `default` only when "every other
  value does nothing" is a deliberate, documented decision.
- **Replace nested `if/else-if` ladders that branch on one discriminant with a
  `switch`.** If each branch only maps input to a value, prefer a lookup table
  (`static constexpr` array) over both.
- **Prefer early-return guard clauses over nested success bodies.** Handle the
  error/empty/skip cases first and return; keep the main path at the left
  margin.

## When NOT to switch

- Branches test unrelated conditions, not one discriminant: a guarded `if`
  sequence is honest; a switch would be forced.
- Two outcomes on a genuine boolean: keep the `if`.
- The discriminant is an open or unbounded set (arbitrary ints, strings): table
  or map, not a switch.

## Enum hygiene

- `enum class` by default for type safety. Plain `enum` only when values must
  implicitly convert (e.g. a value that doubles as a UI dropdown index), and
  then give it a trailing `_COUNT` sentinel for safe bounds/iteration, matching
  the existing settings enums.
- Name the discriminant after what it selects, not its storage:
  `Orientation orientation`, not `uint8_t mode`.
- No magic numeric codes for states. If you write a comment mapping numbers to
  meanings, you owe an enum.

## Self-review

- [ ] No int/bool standing in for a closed set of modes; it is an `enum class`.
- [ ] Enum dispatch is an exhaustive `switch` with no catch-all `default` (or
      the `default` is a documented deliberate choice).
- [ ] No nested if/else-if ladder on a single discriminant that should be a
      switch or a table.
- [ ] Error/skip cases are early-return guards; the happy path is not buried.
- [ ] No magic numbers where a named enum or `constexpr` would state the intent.
