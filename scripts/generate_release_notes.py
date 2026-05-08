#!/usr/bin/env python3
"""Generate release notes from semantic commit messages.

This script finds the most recent qualifying GitHub release, resolves its tag to
the underlying commit SHA, and builds release notes from semantic commits since
that commit. If no qualifying release is found, it falls back to the most recent
100 non-merge commits reachable from the target SHA.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

REQUEST_TIMEOUT = 15
USER_FACING_TYPES = {"docs", "feat", "fix", "perf"}
BACKEND_TYPES = {"build", "chore", "ci", "refactor", "test"}
DEFAULT_FALLBACK_LIMIT = 100
CONVENTIONAL_RE = re.compile(
    r"^(?P<type>[A-Za-z]+)(?:\([^)]*\))?(?P<breaking>!)?:\s+(?P<description>.+)$"
)


def run_git(args: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        check=check,
    )


def resolve_tag_to_sha(tag_name: str) -> str | None:
    result = run_git(["rev-list", "-n", "1", tag_name], check=False)
    if result.returncode != 0:
        return None
    sha = result.stdout.strip()
    return sha or None


def git_commits_since(
    base_sha: str | None, target_sha: str, limit: int
) -> list[tuple[str, str]]:
    format_arg = "%H%x1f%s"
    cmd = ["log", "--reverse", "--no-merges", f"--format={format_arg}"]
    if base_sha:
        cmd.append(f"{base_sha}..{target_sha}")
    else:
        cmd.extend([f"-{limit}", target_sha])

    result = run_git(cmd)
    commits: list[tuple[str, str]] = []
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        sha, subject = line.split("\x1f", 1)
        commits.append((sha.strip(), subject.strip()))
    return commits


def github_request(url: str, token: str) -> tuple[list[dict], str | None]:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "User-Agent": "crosspoint-release-notes-generator",
        },
    )
    with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT) as response:
        payload = json.loads(response.read().decode("utf-8"))
        return payload, response.headers.get("Link")


def parse_next_link(link_header: str | None) -> str | None:
    if not link_header:
        return None
    for part in link_header.split(","):
        section = part.strip()
        if 'rel="next"' not in section:
            continue
        start = section.find("<")
        end = section.find(">")
        if start == -1 or end == -1 or end <= start + 1:
            continue
        return section[start + 1 : end]
    return None


def iter_releases(repo: str, token: str):
    url = f"https://api.github.com/repos/{repo}/releases?per_page=100"
    while url:
        payload, link_header = github_request(url, token)
        for release in payload:
            yield release
        url = parse_next_link(link_header)


def select_previous_release(
    repo: str,
    token: str,
    include_prereleases: bool,
    exclude_tag: str | None,
) -> dict | None:
    for release in iter_releases(repo, token):
        if release.get("draft"):
            continue
        if not include_prereleases and release.get("prerelease"):
            continue
        tag_name = release.get("tag_name")
        if not tag_name:
            continue
        if exclude_tag and tag_name == exclude_tag:
            continue
        return release
    return None


def categorize_commit(subject: str) -> tuple[str, str]:
    match = CONVENTIONAL_RE.match(subject)
    if not match:
        return "other", subject

    commit_type = match.group("type").lower()
    description = f'{commit_type}: {match.group("description")}'
    if match.group("breaking"):
        description = f"BREAKING: {description}"

    if commit_type in USER_FACING_TYPES:
        return "user", description
    if commit_type in BACKEND_TYPES:
        return "backend", description
    return "other", subject


def render_section(title: str, items: list[str]) -> list[str]:
    lines = [f"## {title}", ""]
    if items:
        lines.extend(items)
    else:
        lines.append("- None.")
    lines.append("")
    return lines


def build_release_notes(
    target_sha: str,
    current_label: str,
    previous_release: dict | None,
    commits: list[tuple[str, str]],
    fallback_limit: int,
) -> str:
    lines = [f"# CrossPoint {current_label}", ""]

    if previous_release:
        prev_tag = previous_release["tag_name"]
        prev_sha = previous_release.get("resolved_sha")
        lines.append(
            f"Changes since `{prev_tag}` (`{prev_sha[:7]}`) through `{target_sha[:7]}`."
        )
    else:
        lines.append(
            f"No qualifying previous release was found; showing up to the last {fallback_limit} commits through `{target_sha[:7]}`."
        )
    lines.append("")

    user_items: list[str] = []
    backend_items: list[str] = []
    other_items: list[str] = []

    for sha, subject in commits:
        category, description = categorize_commit(subject)
        item = f"- {description} (`{sha[:7]}`)"
        if category == "user":
            user_items.append(item)
        elif category == "backend":
            backend_items.append(item)
        else:
            other_items.append(item)

    lines.extend(render_section("User-Facing Changes", user_items))
    lines.extend(render_section("Backend Changes", backend_items))
    lines.extend(render_section("Other Changes", other_items))
    return "\n".join(lines).rstrip() + "\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--current-label", required=True)
    parser.add_argument(
        "--release-scope",
        choices=("full", "any"),
        required=True,
        help="Use 'full' for stable releases only or 'any' for any published release type.",
    )
    parser.add_argument("--exclude-tag")
    parser.add_argument("--fallback-limit", type=int, default=DEFAULT_FALLBACK_LIMIT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GITHUB_TOKEN")

    if not repo:
        print("[error] GITHUB_REPOSITORY is required", file=sys.stderr)
        return 1
    if not token:
        print("[error] GITHUB_TOKEN is required", file=sys.stderr)
        return 1

    include_prereleases = args.release_scope == "any"
    previous_release = None
    base_sha = None

    try:
        previous_release = select_previous_release(
            repo=repo,
            token=token,
            include_prereleases=include_prereleases,
            exclude_tag=args.exclude_tag,
        )
    except urllib.error.HTTPError as err:
        print(
            f"[warn] Failed to query GitHub releases: HTTP {err.code}", file=sys.stderr
        )
    except urllib.error.URLError as err:
        print(f"[warn] Failed to query GitHub releases: {err.reason}", file=sys.stderr)

    if previous_release:
        previous_tag = previous_release["tag_name"]
        base_sha = resolve_tag_to_sha(previous_tag)
        if base_sha:
            previous_release["resolved_sha"] = base_sha
        else:
            print(
                f"[warn] Could not resolve previous release tag '{previous_tag}' locally; falling back to recent commits.",
                file=sys.stderr,
            )
            previous_release = None

    commits = git_commits_since(base_sha, args.target_sha, args.fallback_limit)
    notes = build_release_notes(
        target_sha=args.target_sha,
        current_label=args.current_label,
        previous_release=previous_release,
        commits=commits,
        fallback_limit=args.fallback_limit,
    )

    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
