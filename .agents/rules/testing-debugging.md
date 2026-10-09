## Testing and Debugging

### Build Commands

Follow the [existing-environment protocol](environment.md#build-in-an-existing-environment):
run the selected build directly and use setup instructions only when needed.

**Via CLI**:

```bash
# Build firmware (default environment)
pio run

# Build and upload to device
pio run -t upload

# Build specific environment
pio run -e gh_release

# Run the host CMake/CTest unit suites
pio run -t unit-tests

# Clean build artifacts
pio run -t clean
```

**Via VS Code**:

* Use PlatformIO toolbar: Build (✓), Upload (→), Clean (🗑️)
* Or Command Palette: `PlatformIO: Build`, `PlatformIO: Upload`, etc.

### Monitoring and Debugging

Use the [serial monitor options](#serial-monitor-options) below.

### Code Quality

```bash
# Static analysis (cppcheck)
pio check

# Format only Git-modified C/C++ files, on every host
./bin/clang-format-fix -g
```

Do not run raw `clang-format` or probe it with `command -v`; use the wrapper even for diagnostics.

### Debugging Crashes

**Common Crash Causes**:

1. **Out of Memory** (Most common):

   ```cpp
   LOG_DBG("MEM", "Free heap: %d bytes", ESP.getFreeHeap());
   ```

   - Monitor heap usage throughout activity lifecycle

   - Check if large allocations (>10KB) occur before crash

   - Verify buffers are freed in `onExit()`

2. **Stack Overflow**:

   ```cpp
   LOG_DBG("TASK", "Stack high water: %d", uxTaskGetStackHighWaterMark(taskHandle));
   ```

   - Occurs during deep recursion or large local variables

   - Increase task stack size in `xTaskCreate()` (2048 → 4096)

   - Move large buffers to checked `makeUniqueNoThrow` storage when reuse or
     a static buffer cannot meet the ownership and lifetime requirements

3. **Use-After-Free**:

   - Activity deleted but task still running

   - Always `vTaskDelete()` in `onExit()` BEFORE activity destruction

   - Set pointers to `nullptr` after `free()`

4. **Corrupt Cache Files**:

   - Back up book state and remove only the affected book cache or its
     `sections/`; deleting all of `.crosspoint/` also removes settings/state

   - Forces clean re-parse of all EPUBs

   - Check file format versions in [docs/file-formats.md](../../docs/file-formats.md)

5. **Watchdog Timeout**:

   - Loop/task blocked for >5 seconds

   - Add `vTaskDelay(1)` in tight loops

   - Check for blocking I/O operations

**Verification Steps**:

1. Check serial output for stack traces
2. Monitor heap with `ESP.getFreeHeap()` before/after operations
3. Verify task deletion with task list (`vTaskList()`)
4. Test with `LOG_LEVEL=2` (debug logging enabled)

---


## Testing and Verification Workflow

### Testing Checklist

**AI agent scope** (what you CAN verify):

1. ✅ **Build**: Build once after the last code edit with the relevant `pio run` target. Do not clean by default, repeat a target that already passed, or rebuild after formatting/comment-only/documentation-only changes.
2. ✅ **Quality**: `pio check` when relevant + `./bin/clang-format-fix -g`
3. ✅ **Format**: Commit messages (`feat:`/`fix:`), no `.gitignore`-excluded files staged (e.g., `*.generated.h`, `.pio/`, `platformio.local.ini`)
4. ✅ **CI**: Fix GitHub Actions failures before review
5. ✅ **Code review**: Ensure orientation-aware logic is correct in all 4 modes by inspecting switch/case coverage

**Human tester scope** (flag these for the user):
6. 🔲 **Device**: Test on hardware
7. 🔲 **Orientations**: Verify all 4 modes (Portrait/Inverted/Landscape CW/CCW)
8. 🔲 **Heap**: C3 baseline target: > 50KB free, no leaks. Also measure
   `HalMemory` internal/PSRAM heaps and largest blocks across repeated book,
   dictionary, font, and WiFi transitions; free bytes alone do not prove an
   allocation will fit.
9. 🔲 **Cache**: If EPUB/layout behavior changes, back up progress, remove the
   affected book's `sections/` or cache as needed, and verify regeneration,
   including TXT/Markdown and partial-build resume when relevant.

### CI/CD Pipeline Awareness

**GitHub Actions** run automatically on pull requests:

| Workflow      | File                                        | Purpose                |
| ------------- | ------------------------------------------- | ---------------------- |
| Build Check   | `.github/workflows/ci.yml`                  | Verifies code compiles |
| Format Check  | `.github/workflows/pr-formatting-check.yml` | Validates clang-format |
| Release Build | `.github/workflows/release.yml`             | Production releases    |
| RC Build      | `.github/workflows/release_candidate.yml`   | Release candidates     |
| PR Firmware Links | `.github/workflows/pr-firmware-links.yml` | Links downloadable PR artifacts |

Inspect each workflow's current board matrix before claiming CI coverage.
Release and RC assets use board-specific names; OTA discovery must agree with
that naming. Preserve the pinned pioarduino core/package setup across workflows.

**Rules**:

- **Fix CI failures BEFORE** requesting review
- CI runs on: Push to PR, PR updates
- Format check fails → Run `./bin/clang-format-fix -g`
- Build check fails → Fix compile errors

---

## Serial Monitoring and Live Debugging

### Serial Monitor Options

1. **Enhanced**: `python3 scripts/debugging_monitor.py` (color-coded logging, recommended)
2. **Standard**: `pio device monitor` (basic, no colors)
3. **VS Code**: Monitor (🔌) button (IDE-integrated)

### Live Debugging Patterns

**Heap**: `LOG_DBG("MEM", "Free: %d", ESP.getFreeHeap());` (every 5s in loop)
For board-aware measurements, use `HalMemory` as above; the enhanced monitor
plots PSRAM separately when those log values are present.
**Stack**: `uxTaskGetStackHighWaterMark(nullptr)` (< 512 bytes → increase stack)
**Flush**: `logSerial.flush();` (force output before crash)

**Port Detection**: Windows: `mode` | Linux: `ls /dev/ttyUSB* /dev/ttyACM*` or `dmesg | grep tty`

---
