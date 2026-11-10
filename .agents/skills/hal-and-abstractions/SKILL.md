---
name: hal-and-abstractions
description: HAL and UI ownership contracts. Use when changing hardware/storage access, input or activity ownership, rendering contracts, or SDK boundaries.
---

# HAL and Abstractions

The [HAL rule](../../rules/architecture-hal.md#hardware-abstraction-layer-hal)
owns hardware/storage access, synchronization, file lifetime, and singleton
contracts. Consult the section for the boundary being changed.

## Route the change

1. Trace the existing owner and callers. Reuse exposed HAL methods and shared
   UI hosts before adding an interface.
2. UI controls follow [shared UI/input ownership](../../rules/ui-activities.md#shared-ui-and-input);
   geometry follows [orientation](../../rules/ui-activities.md#orientation-aware-logic).
3. Input edges and transitions follow [frame ownership](../../rules/ui-activities.md#input-frames-and-ownership)
   and its transition/verification sections before choosing a guard.
4. User-facing text follows the [translation workflow](../../rules/generated-files-cache.md#translation-workflow).

## Drawing a new boundary

Check the pinned SDK contract and existing HAL first. If firmware needs an
unexposed SDK capability, add it to the relevant HAL with its synchronization,
logging, and error contract; never reach around the HAL from an activity.
A wrapper that only renames another call adds no useful boundary. Keep an
abstraction only when it owns a contract or hides a real implementation choice.

**Done when:** changed callers use the established owner, any new boundary has a
demonstrated contract, and relevant file/input/render lifetimes remain intact.
