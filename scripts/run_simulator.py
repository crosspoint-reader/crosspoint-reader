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
