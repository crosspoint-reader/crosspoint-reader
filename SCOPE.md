# Project Vision & Scope: CrossPoint Reader

The goal of CrossPoint Reader is to create an efficient, open-source reading experience for ESP32-based e-reader devices. Xteink hardware (X3, X4) is where the project started and remains a primary target, but CrossPoint is explicitly broadening to support the wider ecosystem of small ESP32 e-ink readers. We believe a dedicated e-reader should do one thing exceptionally well: **facilitate focused reading.**

## 1. Core Mission

To provide a lightweight, high-performance firmware that maximizes the potential of ESP32-based e-reader hardware, prioritizing legibility, performance, and usability over "swiss-army-knife" functionality.

CrossPoint is **not** a kitchen-sink firmware, and it is **not** Xteink-only. We want clean, maintainable code that the community can build on, and that runs across the range of ESP32 e-reader devices (ESP32-C3, ESP32-S3, and adjacent variants). Every accepted change should make that goal easier, not harder. Device-specific code should live behind the HAL / SDK boundary so the reader core stays portable.

## 2. Guiding Principle: Fill Gaps the Stock Firmware Leaves

CrossPoint exists to do the things the stock firmware does poorly or not at all. New work is evaluated against that delineator:

* **Does the stock firmware already do this well?** We should hit that bar or surpass it 
* **Is another popular CrossPoint fork already solving this well?** If yes, we generally defer to that fork if it's not part of the core reading experience. e.g. stats
* **Does this directly improve the reading experience or the firmware's long-term maintainability?** If no, it is out of scope.

## 3. Current Focus

We are intentionally narrowing scope to consolidate the codebase as we open it up to more ESP32 e-reader devices.

During this period, the priorities are:

* **Memory footprint:** Reducing DRAM usage and heap fragmentation. The ESP32-C3 is the tightest target and sets the ceiling, but the gains benefit every ESP32 variant we run on. 
* **Flash footprint:** Trimming binary size to leave room for additional device targets and features. 
* **Code cleanup:** Refactoring, removing dead code, tightening abstractions, and improving readability. 
* **Reading experience:** EPUB parsing and rendering, typography, hyphenation, line spacing, font handling, and legibility improvements.

### Temporarily Closed Areas

PRs in the following areas will be closed until this notice is lifted. Adding these now makes the cleanup and multi-device work materially harder:

* **New themes.** The existing theming surface is frozen. 
* **New external network connectors.** This includes sync engines, cloud storage clients, OPDS extensions beyond what exists, remote file access, and any new "talk to a server" feature. We now have our own CrossPoint KOSync server which gives us a way to sync to 3rd party systems like Hardcover at an API level instead of bloating the firmware. If you're interested in helping here, the sync server is also open source.

If you are unsure whether your idea falls into one of these categories, open a Discussion first.

## 4. Scope

### In-Scope

*Features that directly improve the core reading experience or the firmware's maintainability.*

* **EPUB Rendering & Optimization:** Improvements to the rendering engine, CSS/image handling, and parsing performance. 
* **Typography & Legibility:** Custom font support, hyphenation, line and paragraph spacing, margins. 
* **E-Ink Driver Refinement:** Reducing full-screen flashes (ghosting management) and improving general rendering. 
* **Reading UX:** Bookmarks, progress tracking, button mapping, page navigation, and other in-reader interactions. 
* **Library Management:** Simple, intuitive ways to organize and navigate a local book collection. 
* **Memory, Flash, and Code Quality:** Refactors and cleanups that reduce resource use or improve maintainability, even without a user-visible feature.

### Out-of-Scope

*Rejected because they compromise the device's stability, maintainability, or core mission.*

* **Interactive Apps:** No notepads, calculators, or games. These belong in other forks and are not part of CrossPoint's focus. 
* **Writing / Authoring Tools:** No typed notes, journals, or editors. Input hardware and RAM are wrong for this, and other forks already explore this space. 
* **Active Connectivity:** No RSS readers, news aggregators, or web browsers. Background Wi-Fi drains the battery and complicates the single-core CPU. 
* **PDF Rendering:** PDFs are fixed-layout documents, so rendering them requires displaying pages as images rather than reflowable text, resulting in constant panning and zooming that makes for a poor reading experience on e-ink. Out of scope on the current hardware class.

## 5. Calls to Action

These are the areas where contributor help is most valuable right now. If you want to take one of these on, open a Discussion or issue first so we can coordinate.

### Theme System: Move Themes Off-Firmware

We want to abstract themes out of the firmware entirely so they no longer consume flash, and instead load from the SD card. This directly supports the current focus on flash footprint and code cleanup.

* **Status:** [@itsthisjustin](https://github.com/itsthisjustin) plans to take this on eventually but is very open to someone else claiming it sooner. 
* **Why it matters:** Every built-in theme costs flash that we would rather spend on rendering, fonts, or future device support. SD-loaded themes also let users customize without rebuilding firmware. It also leads to SD font loading for better language support in the UI. 
* **How to claim:** Comment on the relevant Discussion (or open one) before starting.

### Identifying Other Stock-Firmware Gaps

We want help cataloguing things the stock firmware (and other popular CrossPoint forks) handle poorly or not at all, so future work has a clear target list. Particularly interested in:

* **RTL (right-to-left) text support:** Arabic, Hebrew, Persian, and similar scripts. 
* **Languages with poor stock and fork coverage:** Especially those that need shaping, complex layout, or non-Latin font work that nobody is handling well today. 
* **Other gaps:** Rendering edge cases, accessibility issues, input quirks, anything stock does badly and existing forks have not fixed.

If you can read or use the device in one of these languages, your feedback (even without code) is genuinely useful. Open a Discussion with concrete examples (screenshots, sample EPUBs, expected vs actual behavior) and we will prioritize from there.

## 6. Funding and Contributor Sustainability

CrossPoint uses [Royalty.dev](https://royalty.dev) (yes, a product built by [@itsthisjustin](https://github.com/itsthisjustin)) to fund contributors. There has been some tension in the community around this, so the intent is being clarified here directly.

**Why we do this:**

* To maintain long-term interest from contributors and maintainers, in direct response to substantial community requests for a way to give back. 
* To motivate contributors to invest in the *core* project rather than spinning up competing forks. 
* To help pay for new ESP32 devices so we can port CrossPoint to additional hardware. 
* To give the project a credible long-term path to sustainability.

**How it works:**

* Funds are distributed automatically to contributors based on impact to the codebase and tenure on the project. 
* Over **$600** was raised in the first few hours after opening up funding, which is a signal the demand is real. 
* The exact scoring methodology is published at <https://app.royalty.dev/transparency>.

**This is not fixed in stone.** The weighting, eligibility, and distribution rules can be tweaked as we learn what works for this project. If you have concerns or suggestions about how funds are allocated, open a Discussion. The goal is a system that fairly recognizes the people doing the work, not a perfect one on day one.
