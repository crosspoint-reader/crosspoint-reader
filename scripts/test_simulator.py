#!/usr/bin/env python3
"""Build and smoke-test the CrossVi X3/X4 desktop simulator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "simulator-tests"
GOLDEN_PATH = ROOT / "simulator" / "tests" / "golden_home.json"
XTC_FIXTURES = ROOT / "test" / "xtc" / "resources"


def platformio() -> str:
    executable = shutil.which("pio") or shutil.which("platformio")
    if executable:
        return executable
    bundled = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if bundled.exists():
        return str(bundled)
    raise RuntimeError("PlatformIO is missing")


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(command))
    return subprocess.run(command, cwd=ROOT, text=True, check=True, **kwargs)


def compile_and_run_test(name: str, sources: list[str], flags: list[str]) -> None:
    binary = BUILD / name
    run(
        [
            shutil.which("g++") or "g++",
            "-std=gnu++20",
            "-Isimulator/src",
            *sources,
            "-o",
            str(binary),
            *flags,
        ]
    )
    run([str(binary)])


def test_host_adapters() -> None:
    flags = shlex.split(
        subprocess.check_output([sys.executable, "scripts/simulator_build_flags.py"], cwd=ROOT, text=True)
    )
    BUILD.mkdir(parents=True, exist_ok=True)
    compile_and_run_test(
        "controls-test",
        ["simulator/tests/SimulatorControlsTest.cpp", "simulator/src/SimulatorControls.cpp"],
        flags,
    )
    compile_and_run_test(
        "input-test",
        [
            "simulator/tests/SimulatorInputTest.cpp",
            "simulator/src/HalGPIO.cpp",
            "simulator/src/SimulatorControls.cpp",
            "simulator/src/SimulatorLifecycle.cpp",
        ],
        flags,
    )
    compile_and_run_test(
        "storage-test",
        ["simulator/tests/SimulatorStorageTest.cpp", "simulator/src/HalStorage.cpp"],
        flags,
    )


def bmp_dimensions(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:26]
    if len(header) < 26 or header[:2] != b"BM":
        raise AssertionError(f"Not a BMP screenshot: {path}")
    width, height = struct.unpack_from("<ii", header, 18)
    return width, abs(height)


def bmp_grayscale_pixels(path: Path) -> tuple[int, int, list[int]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError(f"Not a BMP screenshot: {path}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    if width <= 0 or signed_height == 0 or bits_per_pixel != 32:
        raise AssertionError(f"Unexpected simulator BMP layout: {path}")
    height = abs(signed_height)
    row_bytes = width * 4
    if pixel_offset + row_bytes * height > len(data):
        raise AssertionError(f"Truncated simulator BMP: {path}")
    pixels = [0] * (width * height)
    for logical_y in range(height):
        stored_y = logical_y if signed_height < 0 else height - 1 - logical_y
        row = pixel_offset + stored_y * row_bytes
        for x in range(width):
            blue, green, red, _ = data[row + x * 4 : row + x * 4 + 4]
            if red != green or green != blue:
                raise AssertionError(f"Non-grayscale simulator pixel in {path}")
            pixels[logical_y * width + x] = red
    return width, height, pixels


def xtg_source(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    table_offset = struct.unpack_from("<Q", data, 24)[0]
    page_offset, page_size, width, height = struct.unpack_from("<QIHH", data, table_offset)
    if data[page_offset : page_offset + 4] != b"XTG\0" or page_size < 22:
        raise AssertionError(f"Not an uncompressed XTG fixture: {path}")
    return width, height, data[page_offset + 22 : page_offset + page_size]


def create_navigation_fixture(source: Path, destination: Path) -> None:
    """Derive a small multi-page/chapter fixture from the real converter output."""
    width, height, first_payload = xtg_source(source)
    page_count = 3
    header_size = 56
    metadata_size = 256
    chapter_size = 96
    chapter_count = 2
    page_header_size = 22
    table_entry_size = 16
    metadata_offset = header_size
    chapter_offset = metadata_offset + metadata_size
    table_offset = chapter_offset + chapter_count * chapter_size
    data_offset = table_offset + page_count * table_entry_size
    page_size = page_header_size + len(first_payload)
    book = bytearray(data_offset + page_count * page_size)

    struct.pack_into("<IBBHBBBBIQQQQQ", book, 0, 0x00435458, 1, 0, page_count, 0, 1, 0, 1, 1,
                     metadata_offset, table_offset, data_offset, 0, chapter_offset)
    book[metadata_offset : metadata_offset + 27] = b"CrossVi navigation fixture\0"
    book[metadata_offset + 128 : metadata_offset + 143] = b"CrossVi tests\0\0"
    struct.pack_into("<H", book, metadata_offset + 196, chapter_count)
    chapters = (("Chapter 1", 1, 2), ("Chapter 2", 3, 3))
    for index, (name, start, end) in enumerate(chapters):
        offset = chapter_offset + index * chapter_size
        encoded = name.encode("ascii")
        book[offset : offset + len(encoded)] = encoded
        struct.pack_into("<HH", book, offset + 0x50, start, end)

    payloads = (
        first_payload,
        bytes(value ^ 0xFF for value in first_payload),
        bytes(0x00 if (index // 60) % 2 == 0 else 0xFF for index in range(len(first_payload))),
    )
    for index, payload in enumerate(payloads):
        offset = data_offset + index * page_size
        struct.pack_into("<QIHH", book, table_offset + index * table_entry_size, offset, page_size, width, height)
        struct.pack_into("<IHHBBIQ", book, offset, 0x00475458, width, height, 0, 0, len(payload), 0)
        book[offset + page_header_size : offset + page_size] = payload
    destination.write_bytes(book)


def invert_xtg_pages(path: Path) -> None:
    book = bytearray(path.read_bytes())
    page_count = struct.unpack_from("<H", book, 6)[0]
    table_offset = struct.unpack_from("<Q", book, 24)[0]
    for index in range(page_count):
        page_offset, page_size = struct.unpack_from("<QI", book, table_offset + index * 16)
        payload_offset = page_offset + 22
        for offset in range(payload_offset, page_offset + page_size):
            book[offset] ^= 0xFF
    path.write_bytes(book)


def assert_xtg_mapping(device: str, fixture: Path, screenshot: Path) -> None:
    source_width, source_height, payload = xtg_source(fixture)
    width, height, pixels = bmp_grayscale_pixels(screenshot)
    if device == "x4":
        viewport = (0, 0, 480, 800)
    else:
        viewport = (26, 0, 475, 792)
    view_x, view_y, view_width, view_height = viewport
    if (width, height) != ((480, 800) if device == "x4" else (528, 792)):
        raise AssertionError(f"Unexpected {device.upper()} XTC screenshot size: {(width, height)}")

    row_bytes = (source_width + 7) // 8
    for y in range(height):
        for x in range(width):
            if x < view_x or x >= view_x + view_width or y < view_y or y >= view_y + view_height:
                expected = 255
            else:
                source_x = (x - view_x) * (source_width - 1) // (view_width - 1)
                source_y = (y - view_y) * (source_height - 1) // (view_height - 1)
                bit = (payload[source_y * row_bytes + source_x // 8] >> (7 - source_x % 8)) & 1
                expected = 255 if bit else 0
            if pixels[y * width + x] != expected:
                raise AssertionError(
                    f"{device.upper()} XTG mapping differs at ({x}, {y}): "
                    f"{pixels[y * width + x]} != {expected}"
                )


def find_single(path: Path, pattern: str) -> Path:
    matches = list(path.glob(pattern))
    if len(matches) != 1:
        raise AssertionError(f"Expected one {pattern} under {path}, found {len(matches)}")
    return matches[0]


def smoke_xtc_fixture(device: str, fixture_name: str) -> None:
    suffix = Path(fixture_name).suffix[1:]
    output = BUILD / f"{device}-{suffix}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    (sd / ".crosspoint").mkdir()
    (sd / ".crosspoint" / "settings.json").write_text('{"sleepScreen":3}\n', encoding="utf-8")
    fixture = XTC_FIXTURES / fixture_name
    shutil.copy2(fixture, sd / f"converter.{suffix}")

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT,5000:POWER:800",
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "7500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    expected_pages = "Opened file: /converter." + suffix + " (1 pages, 480x800)"
    if expected_pages not in log or "Entering activity: XtcReader" not in log:
        raise AssertionError(f"{device.upper()} did not open {suffix.upper()} through File Browser:\n{log}")
    if "Rendering sleep cover:" not in log:
        raise AssertionError(f"{device.upper()} did not render the {suffix.upper()} sleep cover:\n{log}")

    screenshot = find_single(shots, "*.bmp")
    find_single(shots, "*.framebuffer.bin")
    if suffix == "xtc":
        assert_xtg_mapping(device, fixture, screenshot)
    else:
        _, _, grayscale = bmp_grayscale_pixels(screenshot)
        levels = set(grayscale)
        if len(levels) < 4:
            raise AssertionError(f"{device.upper()} XTCH screenshot exposed only {sorted(levels)}")

    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != b"\0\0\0\0":
        raise AssertionError(f"{device.upper()} saved the wrong first-page progress for {suffix.upper()}")
    find_single(sd / ".crosspoint", "xtc_*/source_identity.bin")
    find_single(sd / ".crosspoint", "xtc_*/cover.bmp")
    recent = json.loads((sd / ".crosspoint" / "recent.json").read_text(encoding="utf-8"))
    recent_paths = [entry.get("path") for entry in recent.get("books", [])]
    if f"/converter.{suffix}" not in recent_paths:
        raise AssertionError(f"{device.upper()} did not retain {suffix.upper()} in Recent Books")

    resume_shots = output / "resume-screenshots"
    resume_shots.mkdir()
    resume_environment = os.environ.copy()
    resume_environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(resume_shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "2500:SCREENSHOT,3200:BACK,5200:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "6200",
        }
    )
    resumed = run([str(binary)], env=resume_environment, capture_output=True, timeout=15)
    resume_log = resumed.stdout + resumed.stderr
    if "Loaded progress: page 0" not in resume_log or "Entering activity: Home" not in resume_log:
        raise AssertionError(f"{device.upper()} did not reload {suffix.upper()} progress and return Home:\n{resume_log}")
    find_single(sd / ".crosspoint", "xtc_*/thumb_*.bmp")
    if len(list(resume_shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} did not capture reader and Home after {suffix.upper()} resume")
    print(f"{device.upper()}: {suffix.upper()} File Browser/open/progress/cover/thumbnail/sleep smoke passed")


def smoke_xtc_status_modes(device: str) -> None:
    hidden_shot = find_single(BUILD / f"{device}-xtc" / "screenshots", "*.bmp")
    width, height, hidden_pixels = bmp_grayscale_pixels(hidden_shot)
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    for mode, expected_region in ((1, "bottom"), (2, "top")):
        output = BUILD / f"{device}-xtc-status-{expected_region}"
        if output.exists():
            shutil.rmtree(output)
        sd = output / "sd"
        shots = output / "screenshots"
        sd.mkdir(parents=True)
        shots.mkdir(parents=True)
        (sd / ".crosspoint").mkdir()
        (sd / ".crosspoint" / "settings.json").write_text(
            json.dumps({"xtcStatusBarMode": mode}) + "\n", encoding="utf-8"
        )
        shutil.copy2(XTC_FIXTURES / "crossvi-converter-480x800.xtc", sd / "status.xtc")
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "5000",
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
        log = completed.stdout + completed.stderr
        if "Rendered page 1/1 (1-bit)" not in log:
            raise AssertionError(f"{device.upper()} did not render XTC with {expected_region} status bar:\n{log}")
        status_width, status_height, status_pixels = bmp_grayscale_pixels(find_single(shots, "*.bmp"))
        if (status_width, status_height) != (width, height):
            raise AssertionError(f"{device.upper()} status mode changed the logical viewport")
        changed_rows = {
            index // width for index, (hidden, status) in enumerate(zip(hidden_pixels, status_pixels)) if hidden != status
        }
        if not changed_rows:
            raise AssertionError(f"{device.upper()} {expected_region} status bar did not alter the frame")
        if expected_region == "top" and max(changed_rows) >= 100:
            raise AssertionError(f"{device.upper()} top status bar changed page mapping below its overlay")
        if expected_region == "bottom" and min(changed_rows) < height - 100:
            raise AssertionError(f"{device.upper()} bottom status bar changed page mapping above its overlay")
    print(f"{device.upper()}: XTC hidden/top/bottom status-bar mapping smoke passed")


def smoke_xtc_navigation(device: str) -> None:
    output = BUILD / f"{device}-xtc-navigation"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    fixture = sd / "navigation.xtc"
    create_navigation_fixture(XTC_FIXTURES / "crossvi-converter-480x800.xtc", fixture)

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4000:SCREENSHOT,4500:RIGHT,5500:SCREENSHOT,"
                "6000:LEFT,7000:SCREENSHOT,7500:CONFIRM,8500:CONFIRM,9500:RIGHT,"
                "10500:CONFIRM,12000:SCREENSHOT,13000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "15500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=20)
    log = completed.stdout + completed.stderr
    required = (
        "Opened file: /navigation.xtc (3 pages, 480x800)",
        "Rendered page 1/3 (1-bit)",
        "Rendered page 2/3 (1-bit)",
        "Entering activity: EpubReaderMenu",
        "Entering activity: XtcReaderChapterSelection",
        "Rendered page 3/3 (1-bit)",
    )
    missing = [item for item in required if item not in log]
    if missing:
        raise AssertionError(f"{device.upper()} XTC navigation missed {missing}:\n{log}")
    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != struct.pack("<I", 2):
        raise AssertionError(f"{device.upper()} did not save the displayed chapter-jump page")
    screenshots = sorted(shots.glob("*.bmp"))
    if len(screenshots) != 4:
        raise AssertionError(f"{device.upper()} XTC navigation produced {len(screenshots)} screenshots")
    digests = [hashlib.sha256(path.read_bytes()).hexdigest() for path in screenshots]
    if len(set(digests)) != 3 or digests[0] != digests[2]:
        raise AssertionError(f"{device.upper()} forward/back/chapter screenshots do not match their pages")
    print(f"{device.upper()}: XTC forward/back/chapter/progress smoke passed")


def stress_xtc_page_turns() -> None:
    """Exercise the real reader/render tasks with 1,000 rapid page requests."""
    device = "x3"
    output = BUILD / f"{device}-xtc-stress"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    fixture = sd / "stress.xtc"
    source = XTC_FIXTURES / "crossvi-converter-480x800.xtc"
    create_navigation_fixture(source, fixture)

    first_turn_at = 3500
    # Keep every press visible across at least one ~16 ms simulator polling
    # cycle.  A 3 ms pulse can begin and end between two polls, which stresses
    # the input script rather than the reader state/render contract.
    turn_interval = 32
    press_duration = 16
    events = ["1200:CONFIRM", "2600:CONFIRM"]
    for index in range(1000):
        key = "RIGHT" if index % 2 == 0 else "LEFT"
        events.append(f"{first_turn_at + index * turn_interval}:{key}:{press_duration}")
    screenshot_at = first_turn_at + 1000 * turn_interval + 1200
    events.append(f"{screenshot_at}:SCREENSHOT")

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
            "CROSSVI_SIM_EXIT_AFTER_MS": str(screenshot_at + 700),
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=45)
    log = completed.stdout + completed.stderr
    if "Opened file: /stress.xtc (3 pages, 480x800)" not in log or "Rendered page 2/3 (1-bit)" not in log:
        raise AssertionError(f"X3 did not exercise the XTC reader during page-turn stress:\n{log}")
    screenshot = find_single(shots, "*.bmp")
    assert_xtg_mapping(device, source, screenshot)
    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != struct.pack("<I", 0):
        raise AssertionError("X3 stress ended with bitmap and saved progress on different pages")
    print("X3: 1,000 rapid XTC page-turn requests kept bitmap and progress on page 1")


def smoke_xtc_replacement() -> None:
    device = "x3"
    output = BUILD / f"{device}-xtc-replacement"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots_a = output / "screenshots-a"
    sd.mkdir(parents=True)
    shots_a.mkdir(parents=True)
    (sd / ".crosspoint").mkdir()
    (sd / ".crosspoint" / "settings.json").write_text('{"sleepScreen":3}\n', encoding="utf-8")
    book = sd / "same.xtc"
    create_navigation_fixture(XTC_FIXTURES / "crossvi-converter-480x800.xtc", book)
    original_size = book.stat().st_size
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    environment_a = os.environ.copy()
    environment_a.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots_a),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4000:RIGHT,5000:BACK,6200:SCREENSHOT,7000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "9000",
        }
    )
    run_a = run([str(binary)], env=environment_a, capture_output=True, timeout=15)
    log_a = run_a.stdout + run_a.stderr
    if "Rendered page 2/3 (1-bit)" not in log_a or "Entering activity: Home" not in log_a:
        raise AssertionError(f"X3 did not prepare replacement state from book A:\n{log_a}")
    cache_a = find_single(sd / ".crosspoint", "xtc_*")
    progress_a = (cache_a / "progress.bin").read_bytes()
    identity_a = (cache_a / "source_identity.bin").read_bytes()
    cover_a = (cache_a / "cover.bmp").read_bytes()
    thumb_a = find_single(cache_a, "thumb_*.bmp").read_bytes()
    if progress_a != struct.pack("<I", 1):
        raise AssertionError("X3 did not save book A on page 2 before replacement")

    invert_xtg_pages(book)
    if book.stat().st_size != original_size:
        raise AssertionError("Replacement fixture unexpectedly changed size")
    shots_b = output / "screenshots-b"
    shots_b.mkdir()
    environment_b = os.environ.copy()
    environment_b.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots_b),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT,5000:BACK,6200:SCREENSHOT,7000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "9000",
        }
    )
    run_b = run([str(binary)], env=environment_b, capture_output=True, timeout=15)
    log_b = run_b.stdout + run_b.stderr
    if "Rendered page 1/3 (1-bit)" not in log_b or "Loaded progress: page 1" in log_b:
        raise AssertionError(f"X3 replacement inherited book A progress:\n{log_b}")
    cache_b = find_single(sd / ".crosspoint", "xtc_*")
    if (cache_b / "progress.bin").read_bytes() != struct.pack("<I", 0):
        raise AssertionError("Book B did not start with fresh progress")
    if (cache_b / "source_identity.bin").read_bytes() == identity_a:
        raise AssertionError("Book B retained book A source identity")
    if (cache_b / "cover.bmp").read_bytes() == cover_a:
        raise AssertionError("Book B retained book A cover")
    thumbnails_b = sorted(cache_b.glob("thumb_*.bmp"))
    if any(thumbnail.read_bytes() == thumb_a for thumbnail in thumbnails_b):
        raise AssertionError("Book B retained book A thumbnail")
    print("X3: same-path/same-size XTC replacement discarded old progress, cover and thumbnail")


def smoke_book_search_actions(device: str, source: str) -> None:
    output = BUILD / f"{device}-book-search-{source}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    settings = {"uiTheme": 5}
    if device == "x3":
        settings["language"] = "VI"
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    books = []
    for index in range(20):
        name = f"12-book-{index}.txt"
        (sd / name).write_text(f"Book {index}\n\fSecond page\n", encoding="utf-8")
        books.append({"path": f"/{name}", "title": f"12 book {index}", "author": "", "coverBmpPath": ""})
    initial_pinned = []
    if source == "recent":
        initial_pinned = [book["path"] for book in books[:12]]
        (control / "recent.json").write_text(
            json.dumps({"books": books, "pinned": initial_pinned}) + "\n", encoding="utf-8"
        )

    # X3 starts without recents: six Down presses must wrap across the six
    # actual menu items and not stop on the read-only Today/Goal card. X4 uses
    # Recent Books so both action-popup callers get exercised across the pair.
    enter = (
        "1000:DOWN,1400:DOWN,1800:DOWN,2200:DOWN,2600:DOWN,3000:DOWN,3600:CONFIRM"
        if source == "browser"
        else "1200:DOWN,1700:DOWN,2300:CONFIRM"
    )
    events = [
        "800:SCREENSHOT",
        enter,
        "5000:CONFIRM:1000",  # popup must appear at 5500, before release at 6000
        "5600:SCREENSHOT",
        "6200:SCREENSHOT",
        "7600:BACK",
        "8100:UP",
        "8600:CONFIRM",
        "9400:CONFIRM",  # type the initially selected '1'
        "9800:SCREENSHOT",
        "10200:DOWN,10600:DOWN,11000:DOWN,11400:DOWN",
        "11800:RIGHT,12200:RIGHT,12600:RIGHT,13000:RIGHT",
        "13400:CONFIRM",
        "14400:SCREENSHOT",
        "15000:BACK,15500:UP,16000:CONFIRM",
        "17000:SCREENSHOT",
        "17500:DOWN,17900:DOWN,18300:DOWN,18700:DOWN",
        "19100:RIGHT,19500:RIGHT,19900:RIGHT,20300:RIGHT",
        "20700:CONFIRM",
        "21500:CONFIRM",
    ]
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
            "CROSSVI_SIM_EXIT_AFTER_MS": "26500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=35)
    log = completed.stdout + completed.stderr
    activity = "FileBrowser" if source == "browser" else "RecentBooks"
    if f"Entering activity: {activity}" not in log or log.count("Entering activity: KeyboardEntry") < 2:
        raise AssertionError(f"{device.upper()} did not complete the {source} search flow:\n{log}")
    if "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not open the wrapped first search result:\n{log}")

    screenshots = sorted(shots.glob("*.bmp"))
    if len(screenshots) != 6:
        raise AssertionError(f"{device.upper()} {source} search produced {len(screenshots)} screenshots")
    if screenshots[1].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm release selected the first popup action")
    if screenshots[3].read_bytes() != screenshots[5].read_bytes():
        raise AssertionError(f"{device.upper()} did not preserve the query when reopening search")

    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if recent.get("pinned", []) != initial_pinned:
        raise AssertionError(f"{device.upper()} popup release changed the pinned-book list")
    if not recent.get("books") or recent["books"][0].get("path") != "/12-book-0.txt":
        raise AssertionError(f"{device.upper()} search did not open its first ranked result")
    print(f"{device.upper()}: {source} long-press/search/query-retention smoke passed")


def smoke_vietnamese_telex(device: str) -> None:
    output = BUILD / f"{device}-vietnamese-telex"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI"}) + "\n", encoding="utf-8"
    )
    expected_path = "/đ dd.txt"
    (sd / expected_path.removeprefix("/")).write_text("Vietnamese Telex\n", encoding="utf-8")

    # Open File Browser and its search keyboard. Type "dd" as "đ", then
    # hold #@! long enough to disable Telex before release and type raw "dd".
    events = (
        "1000:CONFIRM,1400:UP,1800:CONFIRM,2400:DOWN,2800:DOWN,3200:RIGHT,3600:RIGHT,"
        "4000:CONFIRM,4400:CONFIRM,4800:DOWN,5200:DOWN,5600:SCREENSHOT,"
        "6200:CONFIRM:1000,6900:SCREENSHOT,7600:SCREENSHOT,8000:RIGHT,8400:CONFIRM,"
        "8800:LEFT,9200:UP,9600:UP,10000:CONFIRM,10400:CONFIRM,10800:DOWN,"
        "11200:DOWN,11600:RIGHT,12000:RIGHT,12400:RIGHT,12800:CONFIRM,"
        "14000:SCREENSHOT,14800:CONFIRM,16000:SCREENSHOT"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "17000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=25)
    log = completed.stdout + completed.stderr
    if "Entering activity: KeyboardEntry" not in log or "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not complete the Vietnamese Telex flow:\n{log}")

    screenshots = sorted(shots.glob("*.framebuffer.bin"))
    if len(screenshots) != 5:
        raise AssertionError(f"{device.upper()} Telex smoke produced {len(screenshots)} screenshots")
    if screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} Telex toggle did not render while Confirm was still held")
    if screenshots[1].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm release triggered an extra #@! action")

    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if not recent.get("books") or recent["books"][0].get("path") != expected_path:
        raise AssertionError(f"{device.upper()} did not compose and open the exact query 'đ dd'")
    print(f"{device.upper()}: Vietnamese Telex composition and hold-toggle smoke passed")


def smoke_home_stats_menu(device: str) -> None:
    output = BUILD / f"{device}-home-stats-menu"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    settings = {"uiTheme": 5}
    if device == "x3":
        settings["language"] = "VI"
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:DOWN,1600:DOWN,2200:CONFIRM,3200:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "4000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
    log = completed.stdout + completed.stderr
    if "Entering activity: ReadingStats" not in log:
        raise AssertionError(f"{device.upper()} could not open device stats from the Home menu:\n{log}")
    find_single(shots, "*.bmp")
    print(f"{device.upper()}: read-only Home summary kept the Reading Stats menu available")


def smoke_device(device: str, expected: dict[str, object], update: bool) -> tuple[str, str]:
    output = BUILD / device
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_SCREENSHOT_AFTER_MS": "800",
            "CROSSVI_SIM_EXIT_AFTER_MS": "1200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if f"Hardware detect: {device.upper()}" not in log or "Entering activity: Home" not in log:
        raise AssertionError(f"{device.upper()} did not boot to Home:\n{log}")

    bmps = list(shots.glob("*.bmp"))
    raw_files = list(shots.glob("*.framebuffer.bin"))
    if len(bmps) != 1 or len(raw_files) != 1:
        raise AssertionError(f"{device.upper()} produced an unexpected screenshot set")
    dimensions = bmp_dimensions(bmps[0])
    expected_dimensions = (int(expected["logical_width"]), int(expected["logical_height"]))
    if dimensions != expected_dimensions:
        raise AssertionError(f"{device.upper()} screenshot is {dimensions}, expected {expected_dimensions}")
    raw = raw_files[0].read_bytes()
    if len(raw) != int(expected["framebuffer_bytes"]):
        raise AssertionError(f"{device.upper()} framebuffer has {len(raw)} bytes")
    digest = hashlib.sha256(raw).hexdigest()
    if not update and digest != expected["sha256"]:
        raise AssertionError(
            f"{device.upper()} Home framebuffer changed: {digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    bmp_digest = hashlib.sha256(bmps[0].read_bytes()).hexdigest()
    if not update and bmp_digest != expected["bmp_sha256"]:
        raise AssertionError(
            f"{device.upper()} logical Home screenshot changed: {bmp_digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    print(
        f"{device.upper()}: Home {dimensions[0]}x{dimensions[1]}, {len(raw)} bytes, "
        f"raw sha256={digest}, BMP sha256={bmp_digest}"
    )
    return digest, bmp_digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true", help="reuse existing simulator binaries")
    parser.add_argument("--update-golden", action="store_true", help="accept the current empty-Home frame")
    args = parser.parse_args()

    run([sys.executable, "scripts/setup_simulator_deps.py"])
    test_host_adapters()
    if not args.skip_build:
        pio = platformio()
        run([pio, "run", "-e", "simulator_x3"])
        run([pio, "run", "-e", "simulator_x4"])

    golden = json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))
    for device in ("x3", "x4"):
        smoke_home_stats_menu(device)
        smoke_vietnamese_telex(device)
        smoke_book_search_actions(device, "browser" if device == "x3" else "recent")
        raw_digest, bmp_digest = smoke_device(device, golden[device], args.update_golden)
        golden[device]["sha256"] = raw_digest
        golden[device]["bmp_sha256"] = bmp_digest
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtc")
        smoke_xtc_status_modes(device)
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtch")
        smoke_xtc_navigation(device)
        if device == "x3":
            stress_xtc_page_turns()
            smoke_xtc_replacement()
    if args.update_golden:
        GOLDEN_PATH.write_text(json.dumps(golden, indent=2) + "\n", encoding="utf-8")
        print(f"Updated {GOLDEN_PATH.relative_to(ROOT)}")
    print("Simulator checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
