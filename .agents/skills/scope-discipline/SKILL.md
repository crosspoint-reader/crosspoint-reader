---
name: scope-discipline
description: Dedicated e-reader feature scope. Use when adding features, activities, libraries, settings, dependencies, or expanding the firmware surface.
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
3. **What resources does it need?** Account for steady-state and peak RAM,
   the largest required heap block, Flash/OTA slot headroom, and SD storage.
   Keep shared code within the C3 baseline. Check internal RAM and PSRAM
   separately for the selected board. Label estimates and measurements, and
   leave missing measurements pending. Use `scripts/firmware_size_history.py`
   and `scripts/script_profile_mem.sh` for build footprint comparisons.
   Runtime heap and power claims require device measurements.
4. **Can an existing mechanism meet the requirement?** Follow the reuse ladder
   below before adding a code path.
5. **What stays active, and for how long?** Describe network and task
   lifetimes, retry limits, idle sleep, and cleanup on failure and exit.
   Follow the [activity and power rules](../../rules/ui-activities.md#freertos-task-guidelines).
   Keep network ownership within the defined flow, including its child screens.
   Release it when that flow ends.
6. **What must the human maintain?** Explain the cost of new dependencies,
   settings, and failure paths alongside the concrete reading benefit.

If a request fails the gate, push back with the specific reason and the
`SCOPE.md` basis, and offer the in-scope alternative. Make the call and say why;
do not just hand over a menu.

## Surface awareness

Before writing new code, trace the affected flow and stop at the first option
that fully meets the requirement:

1. An existing setting, activity, configuration, or documentation change.
2. An existing helper, type, or interface in this codebase.
3. An appropriate standard-library facility within the resource budget.
4. An existing SDK capability through the HAL, or an installed dependency.
5. The minimum new code with a clear owner and a demonstrated need.

Leave speculative hooks and future scaffolding for a concrete requirement.

Each new activity adds code and lifecycle responsibilities. Its resident RAM
depends on its implementation and lifetime. Include retained parents in the
peak budget when activities are pushed onto the stack.
Default to extending an existing activity or setting before adding a new screen.
Explain why the new screen is needed.

## Settings are not free

A new setting is a field to persist, migrate, validate, translate, and render,
plus combinatorial test burden. Add one only when users genuinely need the
choice; otherwise pick a sensible fixed default.

## Self-review

- [ ] Checked against `SCOPE.md`; not on the out-of-scope list.
- [ ] Stated the concrete reading benefit, not a generic "useful."
- [ ] Accounted for RAM, largest-block, Flash/OTA, storage, power, and maintenance costs and explained why the reading benefit wins.
- [ ] Labeled estimates, measurements, and pending device checks.
- [ ] Defined network/task lifetimes, retry limits, sleep behavior, and failure/exit cleanup.
- [ ] Checked whether an existing activity/setting/doc already covers it.
- [ ] New setting (if any) is justified by a real user need, not added
      "just in case."
