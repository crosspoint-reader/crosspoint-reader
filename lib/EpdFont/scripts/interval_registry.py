"""Loader for intervals.yaml — the single source of truth for Unicode interval
presets and their script-group tags.

Imported by:
  * fontconvert_sdcard.py     — INTERVAL_PRESETS (name -> [(start, end), ...]).
  * generate-font-manifest.py — scripts_for_presets() / SCRIPT_GROUPS to derive
                                the manifest's per-family `scripts` tags and the
                                top-level `scriptGroups` block.

Keep this module dependency-light (yaml only) so both scripts can import it
without pulling in freetype/fonttools.
"""

from __future__ import annotations

from pathlib import Path

import yaml

_REGISTRY_PATH = Path(__file__).parent / "intervals.yaml"

# The firmware stores a family's group membership in a 32-bit scriptMask
# (FontDownloadActivity::MAX_SCRIPT_GROUPS) and silently ignores manifest
# groups beyond that, so fail the build here instead.
MAX_SCRIPT_GROUPS = 32


def _load() -> dict:
    with open(_REGISTRY_PATH) as f:
        data = yaml.safe_load(f)

    groups = data.get("groups", [])
    presets = data.get("presets", {})

    if len(groups) > MAX_SCRIPT_GROUPS:
        raise ValueError(
            f"intervals.yaml: {len(groups)} groups declared, but the firmware's "
            f"scriptMask supports at most {MAX_SCRIPT_GROUPS}"
        )

    # Validate that every preset's group tag is declared in `groups` and that
    # its ranges are well-formed. This is the payoff of a single source: a
    # typo'd group, a reversed range, or a malformed range fails the build
    # instead of silently dropping fonts out of their section (or dropping
    # glyph coverage) on device.
    known_tags = {g["tag"] for g in groups}
    for name, spec in presets.items():
        tag = spec.get("group")
        if tag is not None and tag not in known_tags:
            raise ValueError(
                f"intervals.yaml: preset '{name}' references unknown group "
                f"'{tag}' (declared groups: {', '.join(sorted(known_tags))})"
            )

        ranges = spec.get("ranges")
        if not isinstance(ranges, list) or not ranges:
            raise ValueError(f"intervals.yaml: preset '{name}' has no ranges")
        for r in ranges:
            if not (isinstance(r, list) and len(r) == 2 and all(isinstance(v, int) for v in r)):
                raise ValueError(
                    f"intervals.yaml: preset '{name}' range {r!r} must be a "
                    f"2-element [start, end] list of integers"
                )
            if r[0] > r[1]:
                raise ValueError(
                    f"intervals.yaml: preset '{name}' range "
                    f"0x{r[0]:04X}-0x{r[1]:04X} has start > end"
                )

    return data


_DATA = _load()

# Preset names are case-insensitive: fontconvert_sdcard.py lowercases names
# before lookup, so both dicts below use lowercased keys and
# scripts_for_presets() lowercases too — a mixed-case name in sd-fonts.yaml
# must resolve identically for glyph coverage and script grouping.

# name -> list of (start, end) tuples (inclusive). Consumed by fontconvert.
INTERVAL_PRESETS: dict[str, list[tuple[int, int]]] = {
    name.lower(): [tuple(r) for r in spec["ranges"]]
    for name, spec in _DATA["presets"].items()
}

# Ordered list of (tag, label). List order = on-device display order.
SCRIPT_GROUPS: list[tuple[str, str]] = [
    (g["tag"], g["label"]) for g in _DATA.get("groups", [])
]

_GROUP_ORDER = {tag: i for i, (tag, _label) in enumerate(SCRIPT_GROUPS)}

# preset name (lowercased) -> group tag (None for coverage-only presets).
_PRESET_GROUP = {name.lower(): spec.get("group") for name, spec in _DATA["presets"].items()}


def scripts_for_presets(preset_names) -> list[str]:
    """Map an iterable of preset names (case-insensitive) to their ordered,
    deduplicated script-group tags. Coverage-only presets (no group) contribute
    nothing."""
    tags = set()
    for name in preset_names:
        tag = _PRESET_GROUP.get(name.strip().lower())
        if tag:
            tags.add(tag)
    return sorted(tags, key=lambda t: _GROUP_ORDER.get(t, len(SCRIPT_GROUPS)))
