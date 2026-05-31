#!/usr/bin/env python3
"""Generate a themes.json manifest from SD theme package folders."""

import argparse
import json
import zlib
from pathlib import Path


def safe_theme_dirs(root: Path):
    for child in sorted(root.iterdir()):
        if not child.is_dir() or child.name.startswith(".") or child.name.startswith("_"):
            continue
        theme_json = child / "theme.json"
        if theme_json.exists():
            yield child


def build_manifest(root: Path, base_url: str):
    themes = []
    for theme_dir in safe_theme_dirs(root):
        theme_doc = json.loads((theme_dir / "theme.json").read_text())
        files = []
        total = 0
        for file_path in sorted(p for p in theme_dir.rglob("*") if p.is_file()):
            rel = file_path.relative_to(theme_dir).as_posix()
            data = file_path.read_bytes()
            total += len(data)
            files.append(
                {
                    "path": rel,
                    "url": f"{theme_dir.name}/{rel}",
                    "size": len(data),
                    "crc32": zlib.crc32(data) & 0xFFFFFFFF,
                }
            )

        themes.append(
            {
                "id": theme_doc["id"],
                "name": theme_doc.get("name", theme_doc["id"]),
                "description": theme_doc.get("description", ""),
                "files": files,
                "totalSize": total,
            }
        )

    return {"version": 1, "baseUrl": base_url, "themes": themes}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="sd-themes")
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--output", default="sd-themes/themes.json")
    args = parser.parse_args()

    manifest = build_manifest(Path(args.root), args.base_url)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
