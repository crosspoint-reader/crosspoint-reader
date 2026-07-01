# CrossPoint Reader Roadmap

This roadmap describes how CrossPoint will transition from its current state into the tighter scope defined in
[SCOPE.md](SCOPE.md). It is intentionally phased: we close out commitments already in flight before locking down to
the stricter "fill gaps the stock firmware leaves" delineator.

Phases are sequential. We do not start the next phase until the prior one is wrapped or explicitly carried over.

---

## Phase 0 - Close Out Legacy Scope Items

**Goal:** Land the work that was already in motion under the prior, broader scope so contributors are not left
hanging, and so we enter the stricter phases with a clean slate.

**In Phase 0:**

* **RTL support PRs** that are currently open. Reviewing, iterating, and merging the in-flight right-to-left work.
* **Dictionary PR** that is currently open. Reviewing and merging the offline dictionary lookup work.
* **Bookmarks** feature. Finishing the in-flight bookmarks work so users have first-class navigation markers in
  EPUBs.
* **Transparent sleep screens** (potential). If a clean implementation lands during this phase, it is in. If it
  stalls or balloons in scope, it moves out and is not picked back up under the stricter phases.

**Exit criteria for Phase 0:**

* The RTL, dictionary, and bookmarks PRs are either merged or explicitly closed with a reason.
* Transparent sleep screens are either merged or shelved.
* No other "legacy scope" features are accepted during this phase. New work that does not fit the post-Phase-0 scope
  should wait.

Once Phase 0 closes, the tighter scope in [SCOPE.md](SCOPE.md) becomes fully enforced. After this point, "but it was
on the old roadmap" is not a valid argument for accepting a PR.

---

## Phase 1 - Consolidation and Footprint

**Goal:** Reduce memory and flash usage and clean up the codebase so that future device support and reading-quality
work has room to breathe.

**Focus areas:**

* DRAM and heap fragmentation reduction across the reader core.
* Flash footprint reduction (dead code, redundant strings, oversized tables).
* Refactors that tighten the HAL / SDK boundary in preparation for non-Xteink ESP32 devices.
* Moving themes off-firmware to SD-loaded assets (see SCOPE.md Section 6).
* **Moving hyphenation files off-firmware.** Hyphenation rules vary per language and the files are large (German
  alone is ~200KB). Today these eat flash budget that should be available for the reader core. The plan is to build
  a downloader analogous to the existing font downloader and store the dictionaries on SD / SPIFFS, loading on
  demand. This unlocks better hyphenation for long-word languages (German, Finnish, Norwegian, etc.) without paying
  the flash cost up front.

**Closed during this phase:** new themes built into firmware, new external network connectors (sync engines, cloud
storage, remote file access).

---

## Phase 2 - Multi-Device ESP32 Support and Recovery Bridge

**Goal:** Land the SDK / HAL generalization work so CrossPoint runs cleanly on ESP32-based e-reader hardware beyond
Xteink (X3 / X4), including ESP32-S3 class devices. In parallel, lay the groundwork for CrossPoint to act as a safe
bridge onto community firmware for users on locked devices.

**Focus areas:**

* Pluggable per-device SDK layers (display, input, storage, battery).
* Per-device build configuration without forking the reader core.
* Documentation for adding a new ESP32 e-reader target.
* **Bootloader / recovery bridge:** Design and prototype a bootloader or recovery-flash workflow that helps users on
  locked devices reach community firmware (CrossPoint *or* other forks) without bricking. Includes a recoverable
  fallback path when a flash goes wrong. This is intentionally fork-neutral; CrossPoint should be a bridge, not a
  trap. Currently being driven by [@jeremydk](https://github.com/jeremydk) alongside the SDK abstraction work.

This phase depends on Phase 1 cleanup landing first; otherwise we generalize a moving target. The bootloader work is
already in motion in parallel under jeremydk, but it ships under Phase 2.

---

## Phase 3 - Reading Experience Deepening

**Goal:** With the codebase smaller and portable, invest in the things only a focused reader firmware should do:
EPUB rendering, typography, hyphenation, layout edge cases, and gap-filling for languages and scripts that neither
stock nor other CrossPoint forks handle well.

**Focus areas:**

* EPUB parsing and rendering improvements.
* Typography (fonts, spacing, hyphenation, justification).
* Identified gaps from the Section 6 call-to-action work (additional RTL polish beyond Phase 0, underserved
  languages, complex script support where realistic on ESP32 hardware).
* E-ink driver refinement (ghosting, partial update behavior).

---

## Out of Roadmap

The following are explicitly *not* on the roadmap. They may live in other CrossPoint forks; they will not be picked
up here:

* Interactive apps (games, calculators, notepads).
* Writing / authoring tools.
* Active connectivity features (RSS, news, browsers).
* PDF rendering as a first-class format.

See [SCOPE.md](SCOPE.md) for the full rationale.

---

## How This Roadmap Changes

* Phase boundaries are decided by maintainers, not by individual PRs.
* If a phase needs to be extended or an item carried over, that is documented here with a short note.
* Proposals for new phases or reordering should go through a Discussion first.
