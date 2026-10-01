# Pocket Library — working rules (read first, every session)

This fork of CrossPoint adds an offline "world library" (Kiwix ZIM collections,
touch search, a linked article reader, later an atlas) for the **Xteink X4 Pro**.
The owner directs and tests on the device; Claude writes all the software.
Upstream's engineering guide still governs code style and the embedded rules:
@AGENTS.md (upstream's `CLAUDE.md` was a symlink to it).

## How we work together

- **The owner does not write code.** Explain what you are doing in plain
  language, a few sentences at a time. When the owner must do something physical
  (flash, plug in, tap through a screen, read a log), give numbered,
  click-by-click steps and say exactly what "success" looks like.
- **Ask before anything irreversible.** Flashing, erasing a card, deleting
  files, force-pushing, or changing licenses all need the owner's explicit OK in
  that session.
- **Never ask the owner to flash a build unless they have confirmed both of
  these exist:** (a) their factory-firmware backup from the CrossPoint web
  installer's "Read flash," and (b) a known-good official CrossPoint X4 Pro
  `.bin`. If they can't confirm, stop and help them make them first.
- **X4 Pro builds only.** The X4 Pro is ESP32-S3. Never produce or suggest an
  ESP32-C3 binary (X3/X4 original builds will not boot on it). Our envs are
  `x4pro-pocketlib` and `x4pro-pocketlib-release` in `platformio.pocketlib.ini`.
- **Work in milestones.** Each milestone ends with: code committed, tests
  passing, a short device test checklist for the owner, and an update to
  `PROGRESS.md`. Do not start the next milestone until the owner says the last
  one passed on the device.
- **Measure, don't trust.** Every number marked *(est.)* is a guess. Replace
  guesses with measurements from the real device and record them in
  `DECISIONS.md`.
- **Keep a paper trail** at the repo root:
  - `PLAN.md` — milestones, current status, open questions.
  - `DECISIONS.md` — every design decision with the reason and the date, plus
    measured performance numbers.
  - `PROGRESS.md` — a dated log, one short entry per session: what changed,
    what the owner needs to test, what's next.
- **Batch your questions.** Collect them into one short numbered list rather
  than stopping repeatedly.

## Project guardrails (from the brief)

- New code is **GPL-3.0-or-later** with the header used in `src/pocketlib/`.
  Upstream files stay MIT with their notices intact.
- Write the ZIM reader from the openZIM specification. Never copy code from
  libzim, other ZIM readers, or the KOReader Wikipedia plugin (CC BY-NC).
- Keep upstream mergeable: new code in `lib/zim/` and `src/pocketlib/`, upstream
  files touched only behind `#ifdef POCKET_LIBRARY`. List every touched
  upstream file in `DECISIONS.md` ("Upstream touch points").
- Record every dependency and its license in `DECISIONS.md`.
