# CrossPoint Reader Roadmap

This roadmap describes how CrossPoint is moving through the tighter scope defined in [SCOPE.md](SCOPE.md). It is
intentionally phased: Phase 0 closed out the commitments already in flight before locking down to the stricter
"fill gaps the stock firmware leaves" delineator.

Phases are sequential. We do not start the next phase until the prior one is wrapped or explicitly carried over.

---

## Phase 0 - Close Out Legacy Scope Items — **COMPLETE**

**Goal:** Land the work that was already in motion under the prior, broader scope so contributors are not left
hanging, and so we enter the stricter phases with a clean slate.

**Landed in Phase 0:**

* **RTL support PRs.** The in-flight right-to-left work was reviewed, iterated, and merged.
* **Dictionary PR.** The offline dictionary lookup work was reviewed and merged.
* **Bookmarks** feature. First-class navigation markers in EPUBs.
* ~~**Transparent sleep screens.**~~ Shelved; not picked back up under the stricter phases.

Phase 0 is closed. The tighter scope in [SCOPE.md](SCOPE.md) is now fully enforced. "But it was on the old roadmap"
is not a valid argument for accepting a PR.

---

## Phase 1 - Consolidation, Footprint, and Multi-Device Support — **IN PROGRESS**

**Goal:** Reduce memory and flash usage, clean up the codebase, and land the SDK / HAL generalization work so
CrossPoint runs cleanly on ESP32-based e-reader hardware beyond Xteink (X3 / X4), including ESP32-S3 class devices.

**Focus areas:**

* DRAM and heap fragmentation reduction across the reader core.
* Flash footprint reduction (dead code, redundant strings, oversized tables).
* Refactors that tighten the HAL / SDK boundary.
* ~~Pluggable per-device SDK layers (display, input, storage, battery) and per-device build configuration without
  forking the reader core.~~ **Done.** CrossPoint now builds for and runs on multiple device targets beyond the
  Xteink X3 / X4, including ESP32-S3 class hardware (X4 Pro, PaperMono, Seeed Sticky).
* Adding support for a new device is done in the [FreeInk SDK](https://freeink.org) first (display, input,
  storage, battery drivers), followed by a commit to this repo adding board support (build environment and
  device configuration).
* E-ink driver refinement (ghosting, partial update behavior).

**Closed during this phase:** new themes built into firmware, new external network connectors (sync engines, cloud
storage, remote file access).

---

## Phase 2 - Languages, Fonts, and Themes

**Goal:** With the codebase smaller and portable, make reading great in every language: multi-language support,
better font support with custom fonts, UI translations, and themes loaded from the SD card instead of consuming
flash.

**Focus areas:**

* Multi-language reading support (underserved languages, complex script support where realistic on ESP32 hardware).
  Substantial progress has already landed: RTL reading (Arabic, Hebrew) and CJK via SD card fonts.
* ~~Better font support and custom fonts.~~ **Landed early.** SD card fonts with a downloader and font manager,
  script grouping, and CJK support are shipped (see [docs/sd-card-fonts.md](docs/sd-card-fonts.md)).
* ~~UI languages and localization.~~ **Landed early.** The UI ships with 30+ translations, including RTL languages,
  and continues to receive improvements.
* Moving themes off-firmware to SD-loaded assets (see SCOPE.md Section 6).
* **SD-loaded plugins.** Extend the device from the SD card without growing the firmware: plugin packages that add
  integrations and connectors, running through the web server and a whitelisted job queue instead of compiled-in
  code. This is how new "talk to a server" functionality gets added without paying the flash and RAM cost in the
  core firmware, keeping the connector freeze in SCOPE.md intact.
* **Moving hyphenation files off-firmware.** Hyphenation rules vary per language and the files are large (German
  alone is ~200KB). Today these eat flash budget that should be available for the reader core. The plan is to build
  a downloader analogous to the existing font downloader and store the hyphenation files on SD, loading on
  demand. This unlocks better hyphenation for long-word languages (German, Finnish, Norwegian, etc.) without paying
  the flash cost up front.

This phase depends on Phase 1 cleanup landing first; otherwise we generalize a moving target.

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
