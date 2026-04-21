# Stage 0 CrossPoint Base Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a forked CrossPoint base in `/Users/mark/Coding/esp32/writerdeck-x4`, prove unchanged firmware builds, and add a simulator target before writer features begin.

**Architecture:** Keep upstream CrossPoint as the clean base and add only Stage 0 documentation plus simulator plumbing. The simulator target is isolated in PlatformIO as `env:simulator`, using the CrossInk-proven `crosspoint-simulator` dependency and a small run script.

**Tech Stack:** CrossPoint Reader, PlatformIO, Arduino framework for ESP32-C3, native PlatformIO simulator environment, SDL2 on macOS.

---

## File Structure

- Existing planning docs to keep:
  - `/Users/mark/Coding/esp32/writerdeck-x4/docs/superpowers/specs/2026-04-21-x4-writerdeck-mvp-design.md`
  - `/Users/mark/Coding/esp32/writerdeck-x4/docs/writerdeck/development-flow.md`
  - `/Users/mark/Coding/esp32/writerdeck-x4/docs/superpowers/plans/2026-04-21-stage-0-crosspoint-base.md`
- Modify after importing CrossPoint:
  - `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`
- Create after importing CrossPoint:
  - `/Users/mark/Coding/esp32/writerdeck-x4/scripts/run_simulator.py`

Stage 0 deliberately does not create writer activity, editor, storage, or input files.

---

### Task 1: Establish The Forked CrossPoint Working Tree

**Files:**
- Preserve: `/Users/mark/Coding/esp32/writerdeck-x4/docs/superpowers/specs/2026-04-21-x4-writerdeck-mvp-design.md`
- Preserve: `/Users/mark/Coding/esp32/writerdeck-x4/docs/writerdeck/development-flow.md`
- Preserve: `/Users/mark/Coding/esp32/writerdeck-x4/docs/superpowers/plans/2026-04-21-stage-0-crosspoint-base.md`

- [ ] **Step 1: Confirm the current directory is not already a git repo**

Run:

```bash
git -C /Users/mark/Coding/esp32/writerdeck-x4 status --short --branch
```

Expected: command fails with `fatal: not a git repository`.

- [ ] **Step 2: Create the GitHub fork without cloning**

Run:

```bash
gh repo fork crosspoint-reader/crosspoint-reader --clone=false --remote=false
```

Expected: either creates `wammerwilcox/crosspoint-reader` or reports that the fork already exists.

- [ ] **Step 3: Initialize the local repo in the existing workspace**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git init -b master
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
git remote add origin https://github.com/wammerwilcox/crosspoint-reader.git
git fetch upstream master --tags
git checkout -B master upstream/master
git branch --set-upstream-to=upstream/master master
git submodule update --init --recursive
```

Expected: `master` points at `upstream/master`; planning docs remain untracked in `docs/superpowers/` and `docs/writerdeck/`.

- [ ] **Step 4: Verify remotes**

Run:

```bash
git -C /Users/mark/Coding/esp32/writerdeck-x4 remote -v
```

Expected output includes:

```text
origin	https://github.com/wammerwilcox/crosspoint-reader.git (fetch)
origin	https://github.com/wammerwilcox/crosspoint-reader.git (push)
upstream	https://github.com/crosspoint-reader/crosspoint-reader.git (fetch)
upstream	https://github.com/crosspoint-reader/crosspoint-reader.git (push)
```

- [ ] **Step 5: Create the Stage 0 branch**

Run:

```bash
git -C /Users/mark/Coding/esp32/writerdeck-x4 switch -c stage-0-simulator
```

Expected: branch `stage-0-simulator` is checked out.

---

### Task 2: Prove The Unmodified CrossPoint Build

**Files:**
- Read: `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`

- [ ] **Step 1: Verify PlatformIO can see the default environment**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio project config --json-output
```

Expected: JSON output includes an environment named `env:default`.

- [ ] **Step 2: Build upstream CrossPoint unchanged**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e default
```

Expected: build succeeds with `SUCCESS` for environment `default`.

- [ ] **Step 3: Record baseline size evidence**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e default --target size
```

Expected: size report prints RAM and flash usage for the ESP32-C3 firmware.

- [ ] **Step 4: Commit the imported planning docs only**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git add docs/superpowers/specs/2026-04-21-x4-writerdeck-mvp-design.md docs/writerdeck/development-flow.md docs/superpowers/plans/2026-04-21-stage-0-crosspoint-base.md
git commit -m "docs: add writerdeck stage 0 plan"
```

Expected: commit succeeds and contains only the three writerdeck planning documents.

---

### Task 3: Write The Failing Simulator Check

**Files:**
- Read: `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`

- [ ] **Step 1: Run simulator build before the simulator target exists**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e simulator
```

Expected: fails because `simulator` is not defined in upstream CrossPoint's `platformio.ini`.

- [ ] **Step 2: Confirm the failure is the expected missing environment**

Expected output includes a PlatformIO unknown-environment error for `simulator`. If the command fails for missing `pio`, missing Python packages, or another host setup problem, fix the host setup before changing CrossPoint files.

---

### Task 4: Add The Simulator Target

**Files:**
- Modify: `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`
- Create: `/Users/mark/Coding/esp32/writerdeck-x4/scripts/run_simulator.py`

- [ ] **Step 1: Add the simulator runner script**

Create `/Users/mark/Coding/esp32/writerdeck-x4/scripts/run_simulator.py`:

```python
Import("env")  # noqa: F821 - SCons injects this at build time


def run_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    subprocess.run([binary], cwd=os.getcwd(), check=True)


env.AddCustomTarget(
    name="run_simulator",
    dependencies=None,
    actions=run_simulator,
    title="Run Simulator",
    description="Build and run the desktop simulator",
    always_build=True,
)
```

- [ ] **Step 2: Add `env:simulator` to PlatformIO**

Append this block to `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`:

```ini
[env:simulator]
platform = native
build_flags =
  -std=gnu++2a
  -arch arm64
  -I/opt/homebrew/include
  -L/opt/homebrew/lib
  -lSDL2
  -DSIMULATOR
  -Wno-c++11-narrowing
  -Wno-bidi-chars
  -DCROSSPOINT_VERSION=\"sim\"
  -DENABLE_SERIAL_LOG
  -DLOG_LEVEL=2
  -DEINK_DISPLAY_SINGLE_BUFFER_MODE=1
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
  -DXML_GE=0
  -DXML_CONTEXT_BYTES=1024
  -DUSE_UTF8_LONG_NAMES=1
  -DPNG_MAX_BUFFERED_PIXELS=16416
  -DDISABLE_FS_H_WARNING=1
  -DDESTRUCTOR_CLOSES_FILE=1
lib_ignore = hal, PNGdec, JPEGDEC
extra_scripts =
  pre:scripts/gen_i18n.py
  pre:scripts/git_branch.py
  pre:scripts/build_html.py
  scripts/run_simulator.py
lib_deps =
  simulator=https://github.com/uxjulia/crosspoint-simulator
  bblanchon/ArduinoJson @ 7.4.2
  ricmoo/QRCode @ ^0.0.1
  links2004/WebSockets @ 2.7.3
```

- [ ] **Step 3: Build the simulator target**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e simulator
```

Expected: build succeeds with `SUCCESS` for environment `simulator`.

- [ ] **Step 4: Run the simulator target**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e simulator -t run_simulator
```

Expected: the SDL simulator opens a CrossPoint window. Close the window manually after verifying it is not blank.

- [ ] **Step 5: Rebuild firmware target after simulator changes**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e default
```

Expected: firmware build still succeeds with `SUCCESS` for environment `default`.

- [ ] **Step 6: Commit simulator support**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git add platformio.ini scripts/run_simulator.py
git commit -m "build: add desktop simulator target"
```

Expected: commit succeeds and contains only `platformio.ini` plus `scripts/run_simulator.py`.

---

### Task 5: Final Stage 0 Verification

**Files:**
- Read: `/Users/mark/Coding/esp32/writerdeck-x4/platformio.ini`
- Read: `/Users/mark/Coding/esp32/writerdeck-x4/docs/writerdeck/development-flow.md`

- [ ] **Step 1: Show local commits relative to upstream**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git fetch upstream
git log --oneline upstream/master..HEAD
```

Expected: output lists the Stage 0 docs commit and simulator commit only.

- [ ] **Step 2: Show net local diff relative to upstream**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git diff --stat upstream/master...HEAD
```

Expected: output mentions only writerdeck docs, `platformio.ini`, and `scripts/run_simulator.py`.

- [ ] **Step 3: Run final build checks**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
pio run -e default
pio run -e simulator
```

Expected: both builds succeed.

- [ ] **Step 4: Push the Stage 0 branch**

Run:

```bash
cd /Users/mark/Coding/esp32/writerdeck-x4
git push -u origin stage-0-simulator
```

Expected: branch `stage-0-simulator` is pushed to `origin`.
