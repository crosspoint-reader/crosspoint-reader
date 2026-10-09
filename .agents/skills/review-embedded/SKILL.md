---
name: review-embedded
description: "Read-only firmware-handoff review of ESP32 resources and hardware: RAM, fragmentation, stack, flash, timing, HAL, lifecycle, input, display, storage, and orientation."
---

# Embedded constraints review

Read the rule files and project skills that match the diff, especially hardware resources, architecture/HAL, coding standards, UI/activity lifecycle, heap discipline, and control-flow clarity. Do not edit files.

Review the diff for:

- worst-case heap use, allocation failure, fragmentation, lifetime, and largest contiguous block;
- stack growth, task stack sizing, ISR safety, RISC-V alignment, and task cleanup;
- flash/DRAM placement, template or `std::function` bloat, and string use in hot paths;
- repeated work, blocking I/O, watchdog exposure, timing assumptions, and E-Ink refresh cost;
- raw SDK, SdFat, GPIO, rendering, settings, or i18n access that bypasses established boundaries;
- activity enter/exit ownership, member handles, task shutdown, and use-after-free paths;
- logical buttons, all four orientations, viewable bounds, single-framebuffer behavior, and grayscale-buffer restoration;
- cache/version implications and hardware states the host build cannot verify.

Do not claim a resource regression without the mechanism and worst-case size or timing path. Report severity, `file:line`, mechanism, likely hardware symptom, and fix. Include concrete measurements or device checks that would confirm uncertain risks. A clean result is normal.

**Done when:** every resource-owning or hardware-facing hunk is accounted for and every finding explains its physical or memory mechanism.
