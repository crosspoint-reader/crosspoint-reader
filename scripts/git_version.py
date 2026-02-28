Import("env")
import subprocess

repo_root = env.subst("$PROJECT_DIR")
try:
    sha = subprocess.check_output(
        ["git", "-C", repo_root, "rev-parse", "--short", "HEAD"],
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    sha = "unknown"

env.Append(CPPDEFINES=[("GIT_SHA", '\\"' + sha + '\\"')])
print("[git_version] GIT_SHA = " + sha)
