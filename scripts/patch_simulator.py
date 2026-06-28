"""
PlatformIO pre-build script: apply CrossPoint's simulator patches via `git apply`.

Used for small local fixes against the external crosspoint-simulator libdep so
the dependency can stay upstream-tracked.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys

PATCHES = {
    "0001-guard-sdl-present-on-host-heap.patch": """\
diff --git a/src/simulator_main.cpp b/src/simulator_main.cpp
index 21edd8a..87e85e8 100644
--- a/src/simulator_main.cpp
+++ b/src/simulator_main.cpp
@@ -5,6 +5,7 @@
 #include "Arduino.h"
 #include "HalDisplay.h"
 #include "HalGPIO.h"
+#include "simulator/SimulatorHeap.h"
 #include "SimulatorLifecycle.h"
 
 extern void setup();
@@ -23,7 +24,10 @@ int main(int argc, char **argv) {
     // SDL must be driven from the main thread on macOS.
     // The render task writes pixels and sets pendingPresent; we flush them
     // here.
-    display.presentIfNeeded();
+    {
+      SimulatorHeap::HostHeapScope hostHeapScope;
+      display.presentIfNeeded();
+    }
     // Yield to the OS so macOS delivers pending keyboard/window events to SDL.
     // Without this, the tight spin-loop starves the Cocoa event system and key
     // presses are only picked up sporadically. 1 ms also caps the loop at ~1
"""
}


def patch_simulator(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        simulator_dir = os.path.join(libdeps_dir, env_dir, "simulator")
        if not os.path.isdir(os.path.join(simulator_dir, ".git")):
            continue
        for name, patch_text in PATCHES.items():
            _apply_one(simulator_dir, name, patch_text)


def _apply_one(simulator_dir, name, patch_text):
    if _git_apply_succeeds(simulator_dir, patch_text, reverse=True):
        return
    if not _git_apply_succeeds(simulator_dir, patch_text, reverse=False):
        result = subprocess.run(
            ["git", "apply", "--check", "-"],
            cwd=simulator_dir,
            capture_output=True,
            input=patch_text,
            text=True,
        )
        sys.stderr.write(
            "ERROR: simulator patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(
        ["git", "apply", "-"],
        cwd=simulator_dir,
        check=True,
        input=patch_text,
        text=True,
    )
    print("Applied simulator patch: %s" % name)


def _git_apply_succeeds(simulator_dir, patch_text, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append("-")
    return subprocess.run(
        cmd, cwd=simulator_dir, capture_output=True, input=patch_text, text=True
    ).returncode == 0


patch_simulator(env)  # noqa: F821
