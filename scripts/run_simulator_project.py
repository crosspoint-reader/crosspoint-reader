"""
Project-side PlatformIO hook for exposing the IDE-visible "Run Simulator" task.

Owned by the firmware repo so we can propagate the simulator's exit code — the
upstream library version calls subprocess.run() without checking returncode,
which causes SCons to report success even when the binary exits non-zero (e.g.
an OOM bad_alloc during benchmarking).

The library's run_simulator.py skips registration when
custom_run_simulator_target_owner = project, so this file is the sole
registrant in simulator_base environments.
"""

Import("env")  # noqa: F821 - SCons injects this at build time
import builtins

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"


def run_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    result = subprocess.run([binary], cwd=os.getcwd())
    return result.returncode


if not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=run_simulator,
        title="Run Simulator",
        description="Build and run the desktop simulator",
        always_build=True,
    )
