---
name: scope-discipline
description: Feature-scope discipline for a dedicated e-reader (not a Swiss Army knife). Use when adding a feature, a new activity, a new lib, a setting, or a dependency, or when a request would grow the firmware's surface. Covers the SCOPE.md test, the RAM-cost vs reading-benefit gate, preferring no-code or existing-mechanism solutions, awareness of the existing activity surface, and how to push back on out-of-scope asks.
---

# Scope Discipline

The mission: do one thing exceptionally well, focused reading on constrained
hardware. `SCOPE.md` is the source of truth for what is in and out. Read it
before adding surface. This is the gate to run before writing a new feature.

## The gate

Before adding a feature, activity, lib, setting, or dependency, answer in order:

1. **Is it in `SCOPE.md`?** Explicitly out: interactive apps (notepad,
   calculator, games), active connectivity (RSS, news, browser), media/audio
   playback. If it is out, say so and stop.
2. **Does it materially improve focused reading?** If the benefit is
   "nice to have" or serves a different use case, it is out. This is not a PDA.
3. **What does it cost in RAM and in the largest-free-block budget?** A feature
   that adds steady-state RAM or a large transient allocation needs a reading
   benefit that clearly outweighs it. Quantify with `firmware_size_history.py`
   and `script_profile_mem.sh` rather than guessing.
4. **Can it be done with no new code?** Prefer an existing activity, an existing
   setting, or a doc over a new code path. The cheapest feature is the one
   already built.

If a request fails the gate, push back with the specific reason and the
`SCOPE.md` basis, and offer the in-scope alternative. Make the call and say why;
do not just hand over a menu.

## Surface awareness

The firmware already carries dozens of activities. Each new one is permanent
RAM, permanent maintenance, and another thing every future refactor must not
break. Default to extending an existing activity or setting before adding a new
screen. New top-level surface needs a real justification, not "it would be
convenient."

## Settings are not free

A new setting is a field to persist, migrate, validate, translate, and render,
plus combinatorial test burden. Add one only when users genuinely need the
choice; otherwise pick a sensible fixed default.

## Self-review

- [ ] Checked against `SCOPE.md`; not on the out-of-scope list.
- [ ] Stated the concrete reading benefit, not a generic "useful."
- [ ] Named the RAM/size cost (measured, not guessed) and why the benefit wins.
- [ ] Checked whether an existing activity/setting/doc already covers it.
- [ ] New setting (if any) is justified by a real user need, not added
      "just in case."
