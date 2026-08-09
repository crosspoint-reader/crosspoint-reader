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

import json
import os
import re
import subprocess
import sys

Import("env")  # noqa: F821 - injected by PlatformIO's SConscript runner

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


def check(label, actual, limit, failures):
    """Report one figure against its budget, recording any breach."""
    if limit is None:
        print("  {:<22} {:>10,} B".format(label, actual))
        return

    headroom = limit - actual
    status = "OK" if headroom >= 0 else "OVER BUDGET"
    print(
        "  {:<22} {:>10,} B   limit {:>10,} B   {:>+10,} B  {}".format(
            label, actual, limit, headroom, status
        )
    )
    if headroom < 0:
        failures.append("{} is {:,} B over budget".format(label, -headroom))


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

    rust_flash = rust_ram = None
    if os.path.exists(map_path):
        rust = archive_contribution(map_path, RUST_ARCHIVE)
        rust_flash = total_for(rust, FLASH_SECTIONS)
        rust_ram = total_for(rust, RAM_SECTIONS)

    budget = load_budget(project_dir, env_name) or {}
    failures = []

    print("\n[memory] {}".format(env_name))
    check("flash", flash_total, budget.get("flash_max"), failures)
    check("static RAM", ram_total, budget.get("static_ram_max"), failures)
    if rust_flash is not None:
        check("  of which Rust", rust_flash, budget.get("rust_flash_max"), failures)
        check("  Rust static RAM", rust_ram, budget.get("rust_static_ram_max"), failures)

    print(
        "  static RAM is .data/.bss/.noinit only - not the heap. "
        "Rust allocates through the firmware heap and is invisible here; "
        "see the ActivityRs heap log for that."
    )

    # Written every link, so the final (authoritative) one wins. Consumers read
    # this rather than parsing the text above, which appears twice per build.
    with open(os.path.join(build_dir, "memory.json"), "w") as handle:
        json.dump(
            {
                "environment": env_name,
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


# VerboseAction supplies a short description; without one SCons prints the whole
# action, which for a link target means dumping every object file.
env.AddPostAction(  # noqa: F821
    "$BUILD_DIR/${PROGNAME}$PROGSUFFIX",
    env.VerboseAction(_post_action, "Checking memory budget"),  # noqa: F821
)
