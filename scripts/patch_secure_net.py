"""Apply the security corrections to the pinned SecureNet dependency, or fail the build."""
from pathlib import Path
import subprocess
import sys


def apply_patch(project: Path) -> None:
    patches = sorted((project / "scripts/patches").glob("secure-net-*.patch"))
    if not patches:
        raise FileNotFoundError("SecureNet security patches are missing")
    sdk = project / "freeink-sdk"
    for patch in patches:
        command = ["git", "apply", str(patch)]
        applied = subprocess.run(command + ["--reverse", "--check"], cwd=sdk, capture_output=True)
        if applied.returncode == 0:
            continue
        subprocess.run(command + ["--check"], cwd=sdk, check=True)
        subprocess.run(command, cwd=sdk, check=True)
        print("Applied SecureNet patch:", patch.name)


if "Import" in globals():
    Import("env")
    apply_patch(Path(env.subst("$PROJECT_DIR")))
elif __name__ == "__main__":
    apply_patch(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
