#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def default_env() -> str:
    if sys.platform == "darwin":
        return "simulator"
    return "simulator_i386"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the CrossPoint simulator benchmark")
    parser.add_argument(
        "--epub",
        default="test/epubs/test_mixed_images.epub",
        help="Repo-relative or absolute path to the EPUB to benchmark",
    )
    parser.add_argument(
        "--pages",
        type=int,
        default=100,
        help="Number of forward page turns to benchmark",
    )
    parser.add_argument(
        "--heap-bytes",
        type=int,
        default=0,
        help="Optional simulator heap cap in bytes; 0 disables the cap",
    )
    parser.add_argument(
        "--sd-root",
        default="fs_",
        help="Simulator SD root to stage into",
    )
    parser.add_argument(
        "--keep-cache",
        action="store_true",
        help="Do not clear /.crosspoint before the run",
    )
    parser.add_argument(
        "--video-driver",
        default="dummy",
        help="SDL_VIDEODRIVER to use. Set to x11/wayland/etc to watch the window.",
    )
    parser.add_argument(
        "--env",
        default=default_env(),
        help="PlatformIO environment to run. Defaults to simulator_i386 except on macOS, where it falls back to simulator.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    epub_src = Path(args.epub)
    if not epub_src.is_absolute():
        epub_src = repo_root / epub_src
    epub_src = epub_src.resolve()

    if not epub_src.exists():
        raise SystemExit(f"EPUB not found: {epub_src}")

    sd_root = Path(args.sd_root)
    if not sd_root.is_absolute():
        sd_root = repo_root / sd_root
    books_dir = sd_root / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    staged_name = "benchmark.epub"
    staged_epub = books_dir / staged_name
    shutil.copy2(epub_src, staged_epub)

    env = os.environ.copy()
    env["CROSSPOINT_SIM_BENCHMARK"] = "1"
    env["CROSSPOINT_SIM_BENCH_EPUB"] = f"/books/{staged_name}"
    env["CROSSPOINT_SIM_BENCH_PAGES"] = str(args.pages)
    env["CROSSPOINT_SIM_SD"] = str(sd_root)
    env["SDL_VIDEODRIVER"] = args.video_driver
    if not args.keep_cache:
        env["CROSSPOINT_SIM_BENCH_CLEAR_CACHE"] = "1"
    if args.heap_bytes > 0:
        env["CROSSPOINT_SIM_HEAP_BYTES"] = str(args.heap_bytes)

    cmd = ["python3", "-m", "platformio", "run", "-e", args.env, "-t", "run_simulator"]
    return subprocess.run(cmd, cwd=repo_root, env=env, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
