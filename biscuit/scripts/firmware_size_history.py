#!/usr/bin/env python3
"""
Build firmware at selected commits and report flash usage.

Two modes (mutually exclusive, one required):

  --range START END    Walk every commit from START (exclusive baseline) to END
                       (inclusive), oldest-first.  START is also built so the
                       first delta can be computed.  Both refs must lie on the
                       same ancestry path (i.e. one branch).

  --commits REF [...]  Build each REF in the order given.  Refs may be SHAs,
                       branch names, tags, or relative refs like HEAD~3.  They
                       can come from different branches.

Common options:
  --env ENV            PlatformIO build environment (default: "default")
  --csv [FILE]         Output as CSV.  Without FILE, writes to stdout.

Output is a human-readable table by default.  Use --csv for machine-readable
output.

Examples:
    python3 scripts/firmware_size_history.py --range HEAD~5 HEAD
    python3 scripts/firmware_size_history.py --range abc1234 def5678 --env gh_release --csv sizes.csv
    python3 scripts/firmware_size_history.py --commits main feature/new-parser
    python3 scripts/firmware_size_history.py --commits abc1234 def5678 ghi9012 --csv
"""

import argparse
import csv
import re
import subprocess
import sys

FLASH_RE = re.compile(
    r"Flash:.*?(\d+)\s+bytes\s+from\s+(\d+)\s+bytes"
)
BOX_CHAR = "\u2500"


def run(cmd, capture=True, check=True):
    result = subprocess.run(
        cmd, capture_output=capture, text=True, check=check
    )
    return result


def resolve_ref(ref):
    """Resolve a git ref to (full_sha, title), or sys.exit with a message."""
    r = run(["git", "rev-parse", "--verify", ref], check=False)
    if r.returncode != 0:
        print(f"[error] Could not resolve ref '{ref}'", file=sys.stderr)
        sys.exit(1)
    sha = r.stdout.strip()
    title = run(["git", "log", "-1", "--format=%s", sha]).stdout.strip()
    return sha, title


def git_current_ref():
    """Return the current branch name, or the detached commit hash."""
    r = run(["git", "symbolic-ref", "--short", "HEAD"], check=False)
    if r.returncode == 0:
        return r.stdout.strip()
    return run(["git", "rev-parse", "HEAD"]).stdout.strip()


def git_commit_list(start, end):
    """Return list of (hash, title) from start (exclusive) to end (inclusive), oldest first."""
    r = run([
        "git", "log", "--reverse", "--format=%H %s",
        f"{start}..{end}",
    ])
    commits = []
    for line in r.stdout.strip().splitlines():
        if not line:
            continue
        sha, title = line.split(" ", 1)
        commits.append((sha, title))
    return commits


def git_checkout(ref):
    run(["git", "checkout", "--detach", ref], check=True)


def build_firmware(env):
    """Run pio build and return the raw combined stdout+stderr."""
    result = subprocess.run(
        ["pio", "run", "-e", env],
        capture_output=True, text=True, check=False
    )
    return result.returncode, result.stdout + "\n" + result.stderr


def parse_flash_used(output):
    """Extract used-bytes integer from PlatformIO output, or None."""
    m = FLASH_RE.search(output)
    if m:
        return int(m.group(1))
    return None


def write_csv(out, rows, fieldnames):
    """Write rows as CSV to a file-like object."""
    writer = csv.DictWriter(out, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(rows)


def format_table(rows):
    """Print rows as an aligned human-readable table to stdout."""
    COL_COMMIT = 10
    COL_FLASH = 11
    COL_DELTA = 7

    def fmt_flash(val):
        if val == "FAILED":
            return "FAILED"
        return f"{val:,}"

    def fmt_delta(val):
        if val == "" or val is None:
            return ""
        return f"{val:+,}"

    header = (
        f"{'Commit':<{COL_COMMIT}}  "
        f"{'Flash':>{COL_FLASH}}  "
        f"{'Delta':>{COL_DELTA}}  "
        f"Title"
    )
    sep = (
        f"{BOX_CHAR * COL_COMMIT}  "
        f"{BOX_CHAR * COL_FLASH}  "
        f"{BOX_CHAR * COL_DELTA}  "
        f"{BOX_CHAR * 40}"
    )
    print(header)
    print(sep)
    for row in rows:
        flash_str = fmt_flash(row["flash_bytes"])
        delta_str = fmt_delta(row["delta"])
        print(
            f"{row['commit']:<{COL_COMMIT}}  "
            f"{flash_str:>{COL_FLASH}}  "
            f"{delta_str:>{COL_DELTA}}  "
            f"{row['title']}"
        )


def build_commits_from_range(start, end):
    """Validate a range and return (all_commits, description) for the build loop."""
    start_sha, start_title = resolve_ref(start)
    resolve_ref(end)

    commits = git_commit_list(start, end)
    if not commits:
        print(f"[error] No commits found in range {start}..{end}", file=sys.stderr)
        sys.exit(1)

    all_commits = [(start_sha, start_title)] + commits
    desc = f"{len(all_commits)} commits (1 baseline + {len(commits)} in range)"
    return all_commits, desc


def build_commits_from_list(refs):
    """Resolve each ref and return (all_commits, description) for the build loop."""
    all_commits = [resolve_ref(ref) for ref in refs]
    desc = f"{len(all_commits)} commit{'s' if len(all_commits) != 1 else ''}"
    return all_commits, desc


def main():
    parser = argparse.ArgumentParser(
        description="Measure firmware flash size across git commits.",
        epilog=(
            "Range mode walks every commit between START and END (one branch).  "
            "List mode builds specific refs that may come from different branches."
        ),
    )

    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--range", nargs=2, metavar=("START", "END"),
        help="Older commit (exclusive baseline) and newer commit (inclusive)",
    )
    mode.add_argument(
        "--commits", nargs="+", metavar="REF",
        help="One or more git refs to build (SHAs, branches, tags, HEAD~N, ...)",
    )

    parser.add_argument("--env", default="default", help="PlatformIO environment (default: 'default')")
    parser.add_argument(
        "--csv", nargs="?", const="-", default=None, metavar="FILE",
        help="Output as CSV (default: stdout, or specify FILE)",
    )
    args = parser.parse_args()

    # Validate refs before touching the working tree so a bad ref never
    # leaves uncommitted changes stranded in the stash.
    if args.range:
        all_commits, desc = build_commits_from_range(args.range[0], args.range[1])
        is_range = True
    else:
        all_commits, desc = build_commits_from_list(args.commits)
        is_range = False

    original_ref = git_current_ref()
    print(f"[info] Will restore to '{original_ref}' when finished.", file=sys.stderr)

    stash_needed = False
    status = run(["git", "status", "--porcelain"]).stdout.strip()
    if status:
        print("[info] Stashing uncommitted changes...", file=sys.stderr)
        run(["git", "stash", "push", "-m", "firmware_size_history auto-stash"])
        stash_needed = True

    print(f"[info] Building {desc}...", file=sys.stderr)

    results = []
    try:
        for i, (sha, title) in enumerate(all_commits):
            short = sha[:10]
            if is_range:
                label = "baseline" if i == 0 else f"{i}/{len(all_commits) - 1}"
            else:
                label = f"{i + 1}/{len(all_commits)}"
            print(f"\n[{label}] {short} {title}", file=sys.stderr)

            git_checkout(sha)

            print(f"  Building (env: {args.env})...", file=sys.stderr)
            rc, output = build_firmware(args.env)

            if rc != 0:
                print(f"  BUILD FAILED (exit {rc}) -- skipping", file=sys.stderr)
                results.append((sha, title, None))
                continue

            used = parse_flash_used(output)
            if used is None:
                print("  Could not parse flash size from output -- skipping", file=sys.stderr)
                results.append((sha, title, None))
                continue

            print(f"  Flash used: {used:,} bytes", file=sys.stderr)
            results.append((sha, title, used))

    except KeyboardInterrupt:
        print("\n[info] Interrupted -- writing partial results.", file=sys.stderr)
    finally:
        print(f"\n[info] Restoring '{original_ref}'...", file=sys.stderr)
        run(["git", "checkout", original_ref], check=False)
        if stash_needed:
            print("[info] Restoring stashed changes...", file=sys.stderr)
            run(["git", "stash", "pop"], check=False)

    # Build result rows with deltas
    rows = []
    prev_size = None
    for sha, title, used in results:
        if used is not None and prev_size is not None:
            delta = used - prev_size
        else:
            delta = ""
        rows.append({
            "commit": sha[:10],
            "title": title,
            "flash_bytes": used if used is not None else "FAILED",
            "delta": delta,
        })
        if used is not None:
            prev_size = used

    fieldnames = ["commit", "title", "flash_bytes", "delta"]

    if args.csv is not None:
        if args.csv == "-":
            write_csv(sys.stdout, rows, fieldnames)
        else:
            with open(args.csv, "w", newline="") as f:
                write_csv(f, rows, fieldnames)
            print(f"\n[done] Wrote {args.csv}", file=sys.stderr)
    else:
        print()
        format_table(rows)


if __name__ == "__main__":
    main()
