"""Report firmware memory use and fail the build on a regression.

PlatformIO runs this as a `post:` script, after the ELF is linked. It reports
three things per environment:

  * total flash and static RAM, from the toolchain's own `size -A -d`;
  * how much of that the Rust archive contributes, from `firmware.map`;
  * whether either exceeds the budget in `memory-budget.json`.

# What these numbers mean, and what they do not

The figure PlatformIO calls "RAM" is **static** `.data + .bss + .noinit` only.
It is not the heap, and its denominator (327,680) is a hardcoded board-manifest
constant that is identical for the ESP32-C3 and the ESP32-S3.

The real DRAM available to the heap at boot is smaller. On the C3, `dram0_0_seg`
is 321,808 bytes, of which ~67 KB is the DRAM shadow of the IRAM image
(`.dram0.dummy`) and then the static figure above comes off as well — leaving
roughly 200 KB for the heap. The "~380 KB" quoted in CLAUDE.md is the raw SRAM
size, a fourth number again.

So: a clean report here does **not** mean there is RAM to spare, and it cannot
see heap growth at all. Rust in particular allocates through the firmware's own
`malloc`, so its heap use is invisible to any static measurement. That is what
the runtime heap logging in ActivityRs is for.

# Why the report appears twice

The ESP32 build links `firmware.elf` twice: once before the Arduino
`idf_component.yml` is restored, and once after. Both links fire this action, and
the first ELF is incomplete (roughly a sixth of the final flash figure). The
**second** is authoritative.

Rather than have consumers guess, each run also writes `memory.json` into the
build directory. The final link overwrites it last, so that file always holds
the real numbers — CI reads it instead of scraping this output.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys


BUDGET_FILE = "memory-budget.json"
RUST_ARCHIVE = "libcrosspoint_rs.a"

# Output sections that occupy flash, and those that occupy static RAM. A section
# can be both: .dram0.data is stored in flash and copied to RAM at startup.
FLASH_SECTIONS = {
    ".flash.text",
    ".flash.rodata",
    ".iram0.text",
    ".iram0.vectors",
    ".dram0.data",
}
RAM_SECTIONS = {".dram0.data", ".dram0.bss", ".noinit"}

_MEMORY_REGION = re.compile(r"^(\w+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)")
_OUTPUT_SECTION = re.compile(r"^(\.[\w.$]+)\s")
_INPUT_SECTION_ALONE = re.compile(r"^\s(\.[\w.$]+)\s*$")
_ALLOCATION = re.compile(r"^\s+(?:\.[\w.$]+\s+)?0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S.*)$")


def section_sizes(size_tool, elf_path):
    """Section sizes from the toolchain's own `size`, as {section: bytes}."""
    result = subprocess.run(
        [size_tool, "-A", "-d", elf_path], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        return {}

    sizes = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            try:
                sizes[parts[0]] = int(parts[1])
            except ValueError:
                continue
    return sizes


def memory_regions(map_path):
    """The device's real memory regions, from the map's Memory Configuration.

    This is the honest per-device figure: `dram0_0_seg` is what the linker
    actually had, unlike the board manifest's `maximum_ram_size`, which is the
    same 327,680 for the C3 and the S3 alike.
    """
    regions = {}
    reading = False
    with open(map_path, errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.startswith("Memory Configuration"):
                reading = True
                continue
            if reading:
                if line.startswith("Linker script"):
                    break
                match = _MEMORY_REGION.match(line)
                if match and match.group(1) != "default":
                    regions[match.group(1)] = int(match.group(2), 16)
    return regions


def app_partition(project_dir):
    """Bytes in one app slot. The firmware must fit this, not the whole chip -
    OTA keeps a second copy."""
    path = os.path.join(project_dir, "partitions.csv")
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        for line in handle:
            if line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 5 and parts[1] == "app":
                try:
                    return int(parts[4], 0)
                except ValueError:
                    continue
    return None


def archive_contribution(map_path, archive):
    """Bytes `archive` contributes to each output section, from the link map.

    The map lists every input section under the output section it landed in, so
    this attributes precisely rather than guessing from symbol names. Debug
    sections are reported too but are not loaded onto the device.
    """
    totals = {}
    current_output = None
    in_memory_map = False

    with open(map_path, errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")

            if not in_memory_map:
                in_memory_map = line.startswith("Linker script and memory map")
                continue

            output = _OUTPUT_SECTION.match(line)
            if output:
                current_output = output.group(1)
                continue

            # An input section whose name is too long sits on its own line; the
            # address and size follow on the next. Skip the name-only line.
            if _INPUT_SECTION_ALONE.match(line):
                continue

            allocation = _ALLOCATION.match(line)
            if allocation and current_output:
                size = int(allocation.group(1), 16)
                origin = allocation.group(2)
                if size and archive in origin:
                    totals[current_output] = totals.get(current_output, 0) + size

    return totals


def resolve_budget(budgets, name, seen=None):
    """One environment's budget, following `_like` to a shared base.

    Environments on the same MCU want the same limits, and seven copies of the
    same four numbers drift. An entry naming `"_like": "esp32c3"` inherits that
    base and may override any single field.
    """
    entry = budgets.get(name)
    if entry is None:
        return None

    seen = seen or set()
    parent = entry.get("_like")
    if not parent or parent in seen:
        return entry

    seen.add(name)
    merged = dict(resolve_budget(budgets, parent, seen) or {})
    merged.update(entry)
    return merged


def load_budget(project_dir, env_name):
    path = os.path.join(project_dir, BUDGET_FILE)
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        return resolve_budget(json.load(handle), env_name)


def total_for(sizes, sections):
    return sum(size for name, size in sizes.items() if name in sections)


def collect(label, actual, limit, failures):
    """Record a breach. Silent when within budget - the caller summarises."""
    if limit is not None and actual > limit:
        failures.append("{} is {:,} B over budget ({:,} B of {:,} B)".format(
            label, actual - limit, actual, limit
        ))


def main(env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    env_name = env.subst("$PIOENV")

    elf_path = os.path.join(build_dir, "firmware.elf")
    map_path = os.path.join(build_dir, "firmware.map")
    if not os.path.exists(elf_path):
        return  # Nothing linked (e.g. the native simulator build).

    sizes = section_sizes(env.subst("$SIZETOOL"), elf_path)
    if not sizes:
        print("[memory] could not read section sizes; skipping report")
        return

    flash_total = total_for(sizes, FLASH_SECTIONS)
    ram_total = total_for(sizes, RAM_SECTIONS)

    regions = {}
    rust_flash = rust_ram = None
    if os.path.exists(map_path):
        regions = memory_regions(map_path)
        rust = archive_contribution(map_path, RUST_ARCHIVE)
        rust_flash = total_for(rust, FLASH_SECTIONS)
        rust_ram = total_for(rust, RAM_SECTIONS)

    budget = load_budget(project_dir, env_name) or {}
    failures = []

    # One line, not a table. esp-idf already prints a section summary and
    # PlatformIO its own RAM/Flash bars; repeating that here just buries them.
    # What only this knows is the budget verdict and Rust's share - so say that,
    # and leave the detail to `./build-and-test.sh memory-report`.
    collect("flash", flash_total, budget.get("flash_max"), failures)
    collect("static RAM", ram_total, budget.get("static_ram_max"), failures)
    if rust_flash is not None:
        collect("Rust flash", rust_flash, budget.get("rust_flash_max"), failures)
        collect("Rust static RAM", rust_ram, budget.get("rust_static_ram_max"), failures)

    rust_note = ""
    if rust_flash is not None and flash_total:
        rust_note = ", Rust {:,} B ({:.2f}%)".format(rust_flash, 100.0 * rust_flash / flash_total)
    print(
        "[memory] {}: flash {:,} B{}, static RAM {:,} B - {}".format(
            env_name,
            flash_total,
            rust_note,
            ram_total,
            "OVER BUDGET" if failures else "within budget",
        )
    )

    # Written every link, so the final (authoritative) one wins. Consumers read
    # this rather than parsing the text above, which appears twice per build.
    with open(os.path.join(build_dir, "memory.json"), "w") as handle:
        json.dump(
            {
                "environment": env_name,
                "board": env.subst("$BOARD"),
                "mcu": env.subst("$BOARD_MCU"),
                # What this device actually has, derived rather than assumed.
                "dram_total": regions.get("dram0_0_seg"),
                "iram_total": regions.get("iram0_0_seg"),
                "app_partition": app_partition(project_dir),
                "flash": flash_total,
                "static_ram": ram_total,
                "rust_flash": rust_flash,
                "rust_static_ram": rust_ram,
            },
            handle,
            indent=2,
        )

    if failures:
        for failure in failures:
            sys.stderr.write("[memory] {}\n".format(failure))
        sys.stderr.write(
            "[memory] Raise the limit in {} only with a reason.\n".format(BUDGET_FILE)
        )
        env.Exit(1)


def _post_action(target, source, env):  # noqa: ARG001 - SCons action signature
    main(env)




# ---------------------------------------------------------------------------
# Standalone reporting: `python3 scripts/report_memory.py` reads the memory.json
# each link wrote, so the figures can be reviewed without a rebuild. The same
# file is a PlatformIO post-action when SCons imports it - see the bottom.
# ---------------------------------------------------------------------------

# Lazy: SCons execs this file with no `__file__` defined, and the post-action
# path takes its project directory from the build env instead. Only the
# standalone report needs to work it out for itself.
def project_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_glob():
    return os.path.join(project_root(), ".pio", "build", "*", "memory.json")

# Fallback only. Every build records the device's real figures in memory.json,
# derived from the link map and partitions.csv, so a device with different
# memory reports its own numbers rather than these.
APP_PARTITION = 0x640000


def budgets():
    path = os.path.join(project_root(), BUDGET_FILE)
    if not os.path.exists(path):
        return {}
    with open(path) as handle:
        return json.load(handle)


def readings(only=None):
    """Every environment that has been built, newest figures first.

    The flashable image is measured here rather than recorded at link time:
    `firmware.bin` is produced *after* the post-action fires, so at that point
    it is absent or stale.
    """
    found = []
    for path in sorted(glob.glob(build_glob())):
        with open(path) as handle:
            data = json.load(handle)
        if only and data.get("environment") != only:
            continue
        image = os.path.join(os.path.dirname(path), "firmware.bin")
        if os.path.exists(image):
            data["image_bytes"] = os.path.getsize(image)
        found.append(data)
    return found


def bar(used, limit, width=28):
    """A proportion the eye can read at a glance."""
    if not limit:
        return ""
    filled = min(width, max(0, round(width * used / limit)))
    return "[" + "#" * filled + "." * (width - filled) + "]"


def line(label, used, limit, previous=None):
    pct = " {:5.1f}%".format(100.0 * used / limit) if limit else " " * 7
    out = "  {:<18} {:>10,} B{}".format(label, used, pct)
    if limit:
        out += "  of {:>10,} B  {}".format(limit, bar(used, limit))
    if previous is not None:
        delta = used - previous
        out += "  {:>+9,} B".format(delta) if delta else "         same"
    return out


def report(data, budget, baseline):
    env = data["environment"]
    flash, ram = data["flash"], data["static_ram"]
    slot = data.get("app_partition") or APP_PARTITION
    dram = data.get("dram_total")
    rust_flash = data.get("rust_flash") or 0
    rust_ram = data.get("rust_static_ram") or 0
    was = baseline.get(env, {})

    where = "  ".join(
        part
        for part in (
            data.get("board"),
            data.get("mcu"),
            "{:,} B DRAM".format(dram) if dram else None,
            "{:,} B app slot".format(slot),
        )
        if part
    )
    print("\n{}\n  {}".format(env, where))
    print(line("flash", flash, budget.get("flash_max"), was.get("flash")))
    print(line("static RAM", ram, budget.get("static_ram_max"), was.get("static_ram")))

    if rust_flash or rust_ram:
        print("  ---- of which Rust " + "-" * 41)
        print(line("Rust flash", rust_flash, budget.get("rust_flash_max"), was.get("rust_flash")))
        print(
            line(
                "Rust static RAM",
                rust_ram,
                budget.get("rust_static_ram_max"),
                was.get("rust_static_ram"),
            )
        )
        # The share that answers "what is Rust costing us?" - the absolute byte
        # count means little without the total beside it.
        print(
            "  Rust is {:.2f}% of flash and {:.2f}% of static RAM".format(
                100.0 * rust_flash / flash if flash else 0,
                100.0 * rust_ram / ram if ram else 0,
            )
        )

    # What esptool will actually write. Larger than the section sum above by the
    # image header, segment padding and the sections that are stored but never
    # counted as code or data (.eh_frame, .appdesc). This is the number that has
    # to fit, so it is the one to check before flashing.
    image = data.get("image_bytes")
    if image:
        print(
            "  flashable image      {:>10,} B  {:5.1f}%  of {:>10,} B  {}".format(
                image, 100.0 * image / slot, slot, bar(image, slot)
            )
        )
        print(
            "  {:,} B free in the app slot after flashing ({:,} B of headers and "
            "uncounted sections above the section sum)".format(slot - image, image - flash)
        )
    else:
        print(
            "  {:,} B free in the app slot ({:.1f}% used) - firmware.bin not built yet".format(
                slot - flash, 100.0 * flash / slot
            )
        )
    if dram:
        # What the heap is left with once the static image is placed. The
        # firmware's own allocator gets this, and Rust allocates from it.
        print(
            "  {:,} B of DRAM left for the heap after statics ({:.1f}% of {:,} B)".format(
                dram - ram, 100.0 * (dram - ram) / dram, dram
            )
        )

    over = []
    for label, used, limit in (
        ("flash", flash, budget.get("flash_max")),
        ("static RAM", ram, budget.get("static_ram_max")),
        ("Rust flash", rust_flash, budget.get("rust_flash_max")),
        ("Rust static RAM", rust_ram, budget.get("rust_static_ram_max")),
    ):
        if limit and used > limit:
            over.append("{} is {:,} B over budget".format(label, used - limit))
    return over


def no_figures(only):
    """Say what was found, not just what was missing.

    "nothing has been linked" sends people looking for a broken script when the
    usual cause is mundane: the last build was the simulator, which is a host
    binary with no flash or static RAM to measure.
    """
    built = sorted(
        os.path.basename(path)
        for path in glob.glob(os.path.join(project_root(), ".pio", "build", "*"))
        if os.path.isdir(path) and os.listdir(path)
    )
    device = [name for name in built if not name.startswith("simulator")]
    simulator = [name for name in built if name.startswith("simulator")]

    if only:
        return (
            "No figures for '{}'.\n"
            "Build it:  pio run -e {}".format(only, only)
        )
    if simulator and not device:
        return (
            "Only the simulator has been built ({}).\n"
            "It is a host binary - no flash, no static RAM, nothing to measure.\n"
            "Build a device firmware:  pio run            (or -e sticky)".format(
                ", ".join(simulator)
            )
        )
    if device:
        return (
            "Built but not linked: {}.\n"
            "The build stopped before the link, so no figures were written.\n"
            "Run it again and watch for errors:  pio run".format(", ".join(device))
        )
    return (
        "Nothing has been built yet.\n"
        "Build a device firmware:  pio run            (or -e sticky)"
    )


def cli():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-e", "--environment", help="report one environment only")
    parser.add_argument("--save", metavar="FILE", help="write these figures as a baseline")
    parser.add_argument("--against", metavar="FILE", help="show the delta from a baseline")
    args = parser.parse_args()

    found = readings(args.environment)
    if not found:
        print(no_figures(args.environment))
        return 0

    baseline = {}
    if args.against:
        if not os.path.exists(args.against):
            sys.stderr.write("No such baseline: {}\n".format(args.against))
            return 1
        with open(args.against) as handle:
            baseline = json.load(handle)

    all_budgets = budgets()
    breaches = []
    for data in found:
        budget = resolve_budget(all_budgets, data["environment"]) or {}
        breaches += report(data, budget, baseline)

    print(
        "\nStatic RAM excludes the heap, which is where Rust actually lives."
        "\nFor that: Settings > System > Developers, or the ActivityRs heap log."
    )

    if args.save:
        with open(args.save, "w") as handle:
            json.dump({d["environment"]: d for d in found}, handle, indent=2)
        print("\nBaseline written to {}".format(args.save))

    if breaches:
        sys.stderr.write("\n")
        for breach in breaches:
            sys.stderr.write("OVER BUDGET: {}\n".format(breach))
        return 1
    return 0




# SCons injects `Import`; running the file directly does not have it. That is
# the whole difference between the build-time gate and the report.
if "Import" in globals():
    Import("env")  # noqa: F821 - injected by PlatformIO's SConscript runner

    # VerboseAction supplies a short description; without one SCons prints the
    # whole action, which for a link target means dumping every object file.
    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}$PROGSUFFIX",
        env.VerboseAction(_post_action, "Checking memory budget"),  # noqa: F821
    )
elif __name__ == "__main__":
    sys.exit(cli())

