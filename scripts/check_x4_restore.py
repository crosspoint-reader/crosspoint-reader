#!/usr/bin/env python3
"""Static regression checks for Kay's X4 CrossPoint extensions."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    platformio = read("platformio.ini")
    require(not re.search(r"^\s*BLE\s*$", platformio, re.MULTILINE), "BLE must not be listed under lib_ignore")

    require((ROOT / "src/activities/network/EbookSyncActivity.h").is_file(), "missing EbookSyncActivity.h")
    require((ROOT / "src/activities/network/EbookSyncActivity.cpp").is_file(), "missing EbookSyncActivity.cpp")

    cpp = read("src/activities/network/EbookSyncActivity.cpp")
    header = read("src/activities/network/EbookSyncActivity.h")
    require("X4_EBOOKS_LIST_URL" in header, "missing configurable list URL macro")
    require("X4_EBOOKS_DOWNLOAD_URL" in header, "missing configurable download URL macro")
    require("/webhook/x4-ebooks-list" in header, "missing x4-ebooks-list endpoint")
    require("/webhook/x4-ebooks-download" in header, "missing x4-ebooks-download endpoint")
    require("HttpDownloader::downloadToFile" in cpp, "sync must use HttpDownloader::downloadToFile")
    require("WifiSelectionActivity" in cpp, "sync must reuse WifiSelectionActivity")
    require("ensureExtensionPreserved" in cpp, "missing extension-preserving filename fix")
    require("StringUtils::sanitizeFilename" in cpp, "missing filename sanitization")
    require("STR_EBOOK_SYNC" in cpp, "missing i18n strings in EbookSyncActivity")

    am_h = read("src/activities/ActivityManager.h")
    am_cpp = read("src/activities/ActivityManager.cpp")
    home_cpp = read("src/activities/home/HomeActivity.cpp")
    home_h = read("src/activities/home/HomeActivity.h")
    require("EBOOK_SYNC" in am_h, "HomeMenuItem missing EBOOK_SYNC")
    require("goToEbookSync" in am_h and "goToEbookSync" in am_cpp, "ActivityManager missing goToEbookSync")
    require("onEbookSyncOpen" in home_h and "onEbookSyncOpen" in home_cpp, "HomeActivity missing ebook sync handler")
    require("STR_EBOOK_SYNC" in home_cpp, "Home menu missing eBook Sync label")

    english = read("lib/I18n/translations/english.yaml")
    require("STR_EBOOK_SYNC:" in english, "english.yaml missing STR_EBOOK_SYNC")
    require("STR_EBOOK_SYNC_COMPLETE:" in english, "english.yaml missing completion string")

    print("x4 restore checks OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
