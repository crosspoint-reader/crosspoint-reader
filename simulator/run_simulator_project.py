"""
Project-side PlatformIO hook for exposing the IDE-visible CrossVi simulator task.

Use this from a consuming firmware repo when you want the PlatformIO IDE to show
the simulator under Project Tasks > simulator > Custom > Run Simulator.

The simulator library already auto-loads `run_simulator.py` through
`library.json`. That library hook handles compatibility patching and keeps the
CLI target available for older consumers. This separate project hook exists so
the consuming repo can own the IDE-visible task registration without causing the
same launcher script to be loaded twice.
"""

Import("env")  # noqa: F821 - SCons injects this at build time
import builtins

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"


def run_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    subprocess.run([binary], cwd=os.getcwd())


if not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=run_simulator,
        title="Run CrossVi Simulator",
        description="Build and run the CrossVi desktop simulator",
        always_build=True,
    )
