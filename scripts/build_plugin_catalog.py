#!/usr/bin/env python3
"""Write plugins/catalog.json, the list the web interface's Plugins page offers.

The page reads it, and each plugin's files, straight from GitHub (raw.githubusercontent.com
allows cross-origin reads), so the catalog names every file a plugin needs. A plugin whose
manifest says "catalog": false (the developer example) is left out.

The entries have the shape of crosspoint-reader's plugin catalog (name, title, description,
author, version, base, files), plus this firmware's mount, requires and allowedHosts. `base` is
the folder the files are read from, resolved against the catalog's own URL: a plugin in this
repository gets "<name>/", so a catalog read from another branch (?ref= on the page) installs
that branch's files. An entry with an absolute base names a plugin hosted elsewhere; it is
written into catalog.json by hand and kept as it is.

Run it after adding or changing a plugin. --check exits 1 when the committed catalog is out of
date, for CI.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGINS = os.path.join(ROOT, "plugins")
CATALOG = os.path.join(PLUGINS, "catalog.json")
# Copied from the manifest into the catalog entry, so the page can describe a plugin before
# downloading it.
MANIFEST_FIELDS = ("title", "description", "author", "version", "mount", "requires", "allowedHosts")
CATALOG_NAME = "Witch Reader plugins"


def is_external(entry: dict) -> bool:
    return bool(re.match(r"^https?://", entry.get("base", "")))


def external_entries() -> list:
    """The hand-written entries of plugins hosted elsewhere, from the committed catalog."""
    if not os.path.exists(CATALOG):
        return []
    with open(CATALOG, encoding="utf-8") as f:
        return [e for e in json.load(f).get("plugins", []) if is_external(e)]


def build() -> str:
    entries = []
    for name in sorted(os.listdir(PLUGINS)):
        folder = os.path.join(PLUGINS, name)
        if not os.path.isdir(folder) or not os.path.isfile(os.path.join(folder, "plugin.js")):
            continue
        manifest = {}
        manifest_path = os.path.join(folder, "manifest.json")
        if os.path.isfile(manifest_path):
            with open(manifest_path, encoding="utf-8") as f:
                manifest = json.load(f)
        if manifest.get("catalog", True) is False:
            continue
        entry = {"name": name}
        entry.update({k: manifest[k] for k in MANIFEST_FIELDS if k in manifest})
        entry.setdefault("mount", "settings")
        entry["base"] = name + "/"
        files = []
        for file_name in sorted(os.listdir(folder)):
            if os.path.isdir(os.path.join(folder, file_name)):
                sys.exit(f"ERROR: {name}/{file_name}: plugin files must sit side by side, without subfolders")
            files.append(file_name)
        entry["files"] = files
        entries.append(entry)
    local = {e["name"] for e in entries}
    for entry in external_entries():
        if entry.get("name") in local:
            sys.exit(f"ERROR: catalog entry {entry.get('name')} is both in plugins/ and hosted elsewhere")
        entries.append(entry)
    entries.sort(key=lambda e: e.get("name", ""))
    return json.dumps({"name": CATALOG_NAME, "plugins": entries}, indent=2, ensure_ascii=False) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if plugins/catalog.json is out of date")
    args = parser.parse_args()
    text = build()
    if args.check:
        current = open(CATALOG, encoding="utf-8").read() if os.path.exists(CATALOG) else ""
        if current != text:
            sys.exit("plugins/catalog.json is out of date: run scripts/build_plugin_catalog.py")
        return
    with open(CATALOG, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"Wrote {CATALOG}")


if __name__ == "__main__":
    main()
