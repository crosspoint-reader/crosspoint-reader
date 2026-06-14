# Project Vision & Scope: CrossPoint Reader

The goal of CrossPoint Reader is to create an efficient, open-source reading experience for ESP32-based e-reader
devices. Xteink hardware (X3, X4) is where the project started and remains a primary target, but CrossPoint is
explicitly broadening to support the wider ecosystem of small ESP32 e-ink readers. We believe a dedicated e-reader
should do one thing exceptionally well: **facilitate focused reading.**

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the potential of ESP32-based e-reader hardware,
prioritizing legibility, performance, and usability over "swiss-army-knife" functionality.

CrossPoint is **not** a kitchen-sink firmware, and it is **not** Xteink-only. We want clean, maintainable code that
the community can build on, and that runs across the range of ESP32 e-reader devices (ESP32-C3, ESP32-S3, and
adjacent variants). Every accepted change should make that goal easier, not harder. Device-specific code should live
behind the HAL / SDK boundary so the reader core stays portable.

## 2. Guiding Principle: Fill Gaps the Stock Firmware Leaves

CrossPoint exists to do the things the stock firmware does poorly or not at all. New work is evaluated against that
delineator:

* **Does the stock firmware already do this well?** If yes, we will not duplicate it.
* **Is another popular CrossPoint fork already solving this well?** If yes, we generally defer to that fork rather than
  fragmenting the ecosystem.
* **Does this directly improve the reading experience or the firmware's long-term maintainability?** If no, it is out
  of scope.

### Language Priority

English is the priority language for new features and UI work. Other languages are considered, with preference given
to cases where the stock firmware fails (rendering, input, fonts, layout) or where existing CrossPoint forks have not
addressed the gap.

## 3. Current Focus (Until Further Notice)

We are intentionally narrowing scope to consolidate the codebase as we open it up to more ESP32 e-reader devices.
During this period, the priorities are:

* **Memory footprint:** Reducing DRAM usage and heap fragmentation. The ESP32-C3 is the tightest target and sets the
  ceiling, but the gains benefit every ESP32 variant we run on.
* **Flash footprint:** Trimming binary size to leave room for additional device targets and features.
* **Code cleanup:** Refactoring, removing dead code, tightening abstractions, and improving readability.
* **Reading experience:** EPUB parsing and rendering, typography, hyphenation, line spacing, font handling, and
  legibility improvements.

### Temporarily Closed Areas

PRs in the following areas will be closed until this notice is lifted. Adding these now makes the cleanup and
multi-device work materially harder:

* **New themes.** The existing theming surface is frozen.
* **New external network connectors.** This includes sync engines, cloud storage clients, OPDS extensions beyond what
  exists, remote file access, and any new "talk to a server" feature.

If you are unsure whether your idea falls into one of these categories, open a Discussion first.

## 4. Scope

### In-Scope

*Features that directly improve the core reading experience or the firmware's maintainability.*

* **EPUB Rendering & Optimization:** Improvements to the rendering engine, CSS/image handling, and parsing
  performance.
* **Typography & Legibility:** Custom font support, hyphenation, line and paragraph spacing, margins.
* **E-Ink Driver Refinement:** Reducing full-screen flashes (ghosting management) and improving general rendering.
* **Reading UX:** Bookmarks, progress tracking, button mapping, page navigation, and other in-reader interactions.
* **Library Management:** Simple, intuitive ways to organize and navigate a local book collection.
* **Local Transfer:** Simple, pull-based loading via the existing web server or widely used standards. New connectors
  are currently paused (see Section 3).
* **Reference Tools:** Local, offline dictionary lookup.
* **Memory, Flash, and Code Quality:** Refactors and cleanups that reduce resource use or improve maintainability,
  even without a user-visible feature.
* **Bootloader / Recovery Bridge:** A new bootloader (or bootloader-adjacent recovery flow) that helps users on
  locked devices reach other community firmware safely. CrossPoint should be a *bridge*, not a trap: even users who
  ultimately choose a different fork should be able to get there without bricking the device or ending up in a stuck
  state. This is explicitly in-scope because it serves the broader ESP32 e-reader community, not just our users.

### Out-of-Scope

*Rejected because they compromise the device's stability, maintainability, or core mission.*

* **Interactive Apps:** No notepads, calculators, or games. These belong in other forks and are not part of
  CrossPoint's focus.
* **Writing / Authoring Tools:** No typed notes, journals, or editors. Input hardware and RAM are wrong for this, and
  other forks already explore this space.
* **Active Connectivity:** No RSS readers, news aggregators, or web browsers. Background Wi-Fi drains the battery and
  complicates the single-core CPU.
* **Media Playback:** No audio players or audiobooks.
* **Complex Annotation:** No typed-out notes.
* **Duplication of stock firmware behavior** that already works well.
* **Duplication of features already solved well by other popular CrossPoint forks.**
* **PDF Rendering:** PDFs are fixed-layout documents, so rendering them requires displaying pages as images rather
  than reflowable text, resulting in constant panning and zooming that makes for a poor reading experience on e-ink.
  Out of scope on the current hardware class.

## 5. Idea Evaluation

Before proposing a feature, ask:

1. Does the stock firmware already handle this well? If yes, we pass.
2. Does another popular CrossPoint fork already handle this well? If yes, we usually defer.
3. Does it improve the core reading experience, or reduce memory / flash / code complexity?
4. Is it in one of the temporarily closed areas (new themes, new network connectors)? If yes, wait.
5. Would adding it make the codebase harder to clean up or harder to port to other devices? If yes, rework or defer.

If the idea passes those filters, it is a candidate. If you are not sure, open a **Discussion** before writing any
code.

> **Note to Contributors:** CrossPoint is intentionally narrow. "It would be cool if..." features are not enough; the
> bar is "this fixes something the stock firmware does poorly, or it makes the firmware leaner and easier to maintain."

## 6. Calls to Action

These are the areas where contributor help is most valuable right now. If you want to take one of these on, open a
Discussion or issue first so we can coordinate.

### Theme System: Move Themes Off-Firmware

We want to abstract themes out of the firmware entirely so they no longer consume flash, and instead load from the SD
card. This directly supports the current focus on flash footprint and code cleanup.

* **Status:** [@itsthisjustin](https://github.com/itsthisjustin) plans to take this on eventually but is very open to
  someone else claiming it sooner.
* **Why it matters:** Every built-in theme costs flash that we would rather spend on rendering, fonts, or future
  device support. SD-loaded themes also let users customize without rebuilding firmware.
* **How to claim:** Comment on the relevant Discussion (or open one) before starting.

### SDK Abstraction: Generalize the Lower Layers for All ESP32 E-Readers

To support the broader range of ESP32-based e-reader hardware, the SDK layers (display, input, storage, battery,
etc.) need to be abstracted away from Xteink-specific assumptions and made pluggable per device.

* **Status:** [@jeremydk](https://github.com/jeremydk) is actively working on this.
* **How to help:** Coordinate with jeremydk before opening PRs that touch `open-x4-sdk/` or `lib/hal/` so the work
  does not collide. Smaller adjacent cleanups (removing hardcoded assumptions, tightening HAL boundaries) are welcome
  if scoped against the in-flight direction.

### Bootloader / Recovery Bridge for Locked Devices

We want CrossPoint to act as a bridge for users on locked or hard-to-flash devices, so they can reach *any*
community firmware (not only ours) without bricking the device. The aim is a bootloader or recovery-flash workflow
that:

* Gets users off stock onto something open with minimal risk.
* Lets users switch between community firmwares (including non-CrossPoint forks) without specialized tooling.
* Provides a recoverable fallback if a flash goes wrong, instead of leaving the device in a stuck state.

* **Status:** [@jeremydk](https://github.com/jeremydk) is currently handling this alongside the SDK abstraction
  work. Coordinate with him before opening PRs that touch bootloader, OTA, or recovery-partition code. Contributors
  with ESP32 bootloader / OTA experience are welcome to help; open a Discussion to plug in.
* **Why it matters:** Community firmware adoption is gated by how scary the first flash is. Making that step safe is
  worth more to the ecosystem than any single feature we could add to the reader.

### Identifying Other Stock-Firmware Gaps

We want help cataloguing things the stock firmware (and other popular CrossPoint forks) handle poorly or not at all,
so future work has a clear target list. Particularly interested in:

* **RTL (right-to-left) text support:** Arabic, Hebrew, Persian, and similar scripts.
* **Languages with poor stock and fork coverage:** Especially those that need shaping, complex layout, or
  non-Latin font work that nobody is handling well today.
* **Other gaps:** Rendering edge cases, accessibility issues, input quirks, anything stock does badly and existing
  forks have not fixed.

If you can read or use the device in one of these languages, your feedback (even without code) is genuinely useful.
Open a Discussion with concrete examples (screenshots, sample EPUBs, expected vs actual behavior) and we will
prioritize from there.

## 7. Funding and Contributor Sustainability

CrossPoint uses [Royalty.dev](https://royalty.dev) (yes, a product built by [@itsthisjustin](https://github.com/itsthisjustin))
to fund contributors. There has been some tension in the community around this, so the intent is being clarified
here directly.

**Why we do this:**

* To maintain long-term interest from contributors and maintainers, in direct response to substantial community
  requests for a way to give back.
* To motivate contributors to invest in the *core* project rather than spinning up competing forks.
* To help pay for new ESP32 devices so we can port CrossPoint to additional hardware.
* To give the project a credible long-term path to sustainability.

**How it works:**

* Funds are distributed automatically to contributors based on impact to the codebase and tenure on the project.
* Over **$600** was raised in the first few days after opening up funding, which is a signal the demand is real.
* The exact scoring methodology is published at <https://app.royalty.dev/transparency>.

**This is not fixed in stone.** The weighting, eligibility, and distribution rules can be tweaked as we learn what
works for this project. If you have concerns or suggestions about how funds are allocated, open a Discussion. The
goal is a system that fairly recognizes the people doing the work, not a perfect one on day one.
