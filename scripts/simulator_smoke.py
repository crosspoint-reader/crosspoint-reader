#!/usr/bin/env python3
"""Exercise the real reader through the official simulator's scripted buttons.

Requires Pillow. Run under xvfb-run on Linux. Every invocation owns a fresh
output directory; it never reads or deletes a developer's simulated SD card.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zipfile

from PIL import Image, ImageChops


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def make_book(path):
    """Generate original, deterministic EPUB 2 text without external assets."""
    paragraphs = "".join(
        f"<p>Paragraph {n:03d}. Reading should be calm and predictable. "
        "Turn the page to find another numbered paragraph. "
        "This original text exercises pagination, navigation and saved progress.</p>"
        for n in range(1, 81)
    )
    entries = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '<?xml version="1.0"?>'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles></container>',
        "OEBPS/content.opf": '<?xml version="1.0"?>'
        '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:identifier id="id">crosspoint-ci-smoke</dc:identifier>'
        '<dc:title>CrossPoint CI Reading Test</dc:title><dc:creator>CrossPoint contributors</dc:creator>'
        '<dc:language>en</dc:language></metadata><manifest>'
        '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
        '<item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
        '</manifest><spine toc="ncx"><itemref idref="chapter"/></spine></package>',
        "OEBPS/toc.ncx": '<?xml version="1.0"?>'
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
        '<head><meta name="dtb:uid" content="crosspoint-ci-smoke"/></head>'
        '<docTitle><text>CrossPoint CI Reading Test</text></docTitle><navMap>'
        '<navPoint id="chapter" playOrder="1"><navLabel><text>Reading test</text></navLabel>'
        '<content src="chapter.xhtml"/></navPoint></navMap></ncx>',
        "OEBPS/chapter.xhtml": '<?xml version="1.0" encoding="utf-8"?>'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Reading test</title></head>'
        f'<body><h1>Reading test</h1>{paragraphs}</body></html>',
    }
    with zipfile.ZipFile(path, "w") as book:
        for name, text in entries.items():
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            book.writestr(info, text.encode("utf-8"))


def run_phase(binary, output, name, buttons, screenshots):
    env = {key: value for key, value in os.environ.items() if not key.startswith("CROSSPOINT_SIM_")}
    env.update({
        "CROSSPOINT_SIM_INPUT_SCRIPT": buttons,
        "CROSSPOINT_SIM_SCREENSHOTS": ";".join(f"{ms}:{filename}.bmp" for ms, filename in screenshots),
        "TZ": "UTC",
    })
    with (output / f"{name}.log").open("w", encoding="utf-8") as log:
        result = subprocess.run([str(binary)], cwd=output, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=45, check=False)
    require(result.returncode == 0, f"{name}: simulator exited {result.returncode}; see {name}.log")
    return (output / f"{name}.log").read_text(encoding="utf-8", errors="replace")


def load_screen(output, name, size):
    path = output / f"{name}.bmp"
    require(path.is_file(), f"Missing screenshot: {name}")
    with Image.open(path) as source:
        image = source.convert("RGB")
    require(image.size == size, f"{name}: expected {size}, got {image.size}")
    require(image.convert("L").getextrema()[0] < 100 and image.convert("L").getextrema()[1] > 200,
            f"{name}: blank or invalid display")
    image.save(output / f"{name}.png")
    # Exclude the status bar, whose clock is not a rendering regression.
    return image.crop((0, 0, size[0], size[1] - 80))


def smoke(binary, output, device):
    books = output / "fs_" / "books"
    books.mkdir(parents=True)
    cache = output / "fs_" / ".crosspoint"
    cache.mkdir()
    make_book(books / "smoke.epub")
    size = (528, 792) if device == "x3" else (480, 800)

    run_phase(binary, output, "home", "5000:QUIT", [(4000, "home")])
    load_screen(output, "home", size)

    # Use the firmware's persisted resume path, without a test-only reader API.
    (cache / "state.json").write_text(json.dumps({
        "openEpubPath": "/books/smoke.epub", "lastSleepFromReader": True,
        "readerActivityLoadCount": 0, "showBootScreen": False,
    }), encoding="utf-8")
    # Leave the activity before QUIT: killing an active reader intentionally
    # leaves its crash-loop guard set, so the next boot goes Home instead.
    log = run_phase(binary, output, "reading",
                    "7000:DOWN;10000:UP;13000:DOWN;17000:BACK;20000:QUIT",
                    [(6000, "page-1"), (9000, "page-2"), (12000, "page-1-back"), (16000, "page-2-again")])
    pages = {name: load_screen(output, name, size)
             for name in ("page-1", "page-2", "page-1-back", "page-2-again")}
    require(ImageChops.difference(pages["page-1"], pages["page-2"]).getbbox() is not None,
            "Page-forward did not change the reading content")
    for first, second in (("page-1", "page-1-back"), ("page-2", "page-2-again")):
        require(ImageChops.difference(pages[first], pages[second]).getbbox() is None,
                f"Round-trip navigation changed content: {first} / {second}")
    saved = re.findall(r"Progress saved: spine=(\d+) offset=\d+ page=(\d+)", log)
    transitions = [position for i, position in enumerate(saved) if i == 0 or position != saved[i - 1]]
    require(transitions == [("0", "0"), ("0", "1"), ("0", "0"), ("0", "1")],
            f"Unexpected page transitions: {transitions}")
    progress_files = list(cache.glob("epub_*/progress.bin"))
    require(len(progress_files) == 1, "Expected one persisted EPUB progress file")
    data = progress_files[0].read_bytes()
    require(len(data) in (6, 10), f"Unexpected progress record size: {len(data)}")
    spine, page, count = struct.unpack_from("<HHH", data)
    require(spine == 0 and page == 1 and count > 1, f"Wrong saved progress: {(spine, page, count)}")

    state = json.loads((cache / "state.json").read_text(encoding="utf-8"))
    require(state.get("readerActivityLoadCount") == 0, "Reader did not exit cleanly before restart")
    log = run_phase(binary, output, "reopen", "7000:QUIT", [(6000, "page-2-reopened")])
    reopened = load_screen(output, "page-2-reopened", size)
    require("Loaded cache: 0, 1" in log, "Reopen did not load the saved page")
    require(ImageChops.difference(pages["page-2"], reopened).getbbox() is None,
            "Reopening the book did not reproduce its saved page")
    return {"device": device, "screen_size": size, "transitions": transitions,
            "saved_progress": {"spine": spine, "page": page, "page_count": count},
            "book_sha256": hashlib.sha256((books / "smoke.epub").read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--device", choices=("x3", "x4"), required=True)
    parser.add_argument("--output", type=Path, required=True, help="New directory; must not already exist")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"status": "failed", "device": args.device}
    try:
        report.update(smoke(binary, output, args.device))
        report["status"] = "passed"
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
