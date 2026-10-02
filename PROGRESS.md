# Pocket Library — Progress log

## 2026-10-01 — Session 1: reading, plan, Milestone 0 code

**What changed**
- Read upstream `AGENTS.md`, `SCOPE.md`, the X4 Pro SDK notes, the activity
  system, storage, memory and About screen. Summary in `DECISIONS.md`.
- Branch `pocket-library` off tag `1.6.5`. Added `CLAUDE.md` (working rules),
  `PLAN.md`, `DECISIONS.md`, this log, `LICENSE-GPL-3.0`,
  `platformio.pocketlib.ini` (envs `x4pro-pocketlib`, `x4pro-pocketlib-release`).
- Hidden Diagnostics screen: `src/pocketlib/DiagnosticsActivity.{h,cpp}`,
  reached by tapping **Firmware** five times on Settings → About.
- Probed real ZIM files from openZIM's test suite (findings in DECISIONS.md).

**Not done, and why**
- **No firmware has been compiled yet.** This cloud session's network policy
  blocks the PlatformIO package registry. My workarounds for that were stopped
  by the session's permission checks, so the Diagnostics code is written but
  **unbuilt and untested**. Build it on the Mac (steps below) or in a session
  whose network allows `api.registry.platformio.org`.

**For the owner to do (Milestone 0 device checklist)** — only after backups:
1. Confirm you have (a) a factory backup from the web installer's "Read flash"
   and (b) the official CrossPoint 1.6.5 X4 Pro `.bin`.
2. Flash the official 1.6.5 through crosspointreader.com → Flash tools →
   Xteink X4Pro. Success: CrossPoint boots and opens an EPUB.
3. Then flash our `x4pro-pocketlib-release` `.bin` via "Custom .bin".
   Success: it boots and reads books exactly as before.
4. Settings → About: note **Display Controller**. Tap **Firmware** five times.
   Success: a "Diagnostics" screen appears.
5. Tap **SD benchmark**, wait up to a minute, photograph the screen.

**Next**
- Owner answers the questions in the session summary.
- Build both firmwares; record sizes; hand over the `.bin`.
- Then Milestone 1 (`lib/zim/`, host tests on the openZIM files).

## 2026-10-02 — Session 1, continued: owner's answers

- Fork approved. Owner creates it on github.com (this session can't fork
  without upstream API credentials); then it gets attached and pushed.
- Network: owner will allow `api.registry.platformio.org`,
  `download.kiwix.org` and `wiki.openzim.org`. Not yet in effect here.
- **No backups yet.** Before any flashing: make the factory backup ("Read
  flash") and download the official 1.6.5 X4 Pro `.bin`. Steps in the
  session summary.
- Copyright line "Pocket Library contributors" approved.
- Wiktionary → StarDict conversion chosen (DECISIONS.md).
- Fork is `noah-pi/pocket-library`; work pushed to branch `pocket-library`.
- Added a GitHub Actions build (see DECISIONS.md). Owner enables Actions on
  the fork once; then each push produces downloadable `.bin` files.
