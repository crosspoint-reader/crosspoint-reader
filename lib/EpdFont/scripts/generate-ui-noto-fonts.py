#!/usr/bin/env python3
"""Generate optical Noto fallback faces for monochrome UI rendering."""

import hashlib
import os
import tempfile
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


SOURCE_DIR = Path(__file__).resolve().parent.parent / "builtinFonts" / "source"

# Sources:
# https://github.com/google/fonts/tree/main/ofl/notosanshebrew
# https://github.com/google/fonts/tree/main/ofl/notosansarabic
FAMILIES = (
    {
        "directory": "NotoSansHebrew",
        "source": "NotoSansHebrew-Variable.ttf",
        "sha256": "7ef36a2c3593758cdb622e1bdef4f84523e92fbc3ccc667438dd80ff54c2de88",
        "instances": (
            ("NotoSansHebrew-UIMedium.ttf", {"wght": 550, "wdth": 95}),
            ("NotoSansHebrew-UIBold.ttf", {"wght": 900, "wdth": 92.5}),
        ),
    },
    {
        "directory": "NotoSansArabic",
        "source": "NotoSansArabic-Variable.ttf",
        "sha256": "63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e",
        "instances": (
            ("NotoSansArabic-UIMedium.ttf", {"wght": 600, "wdth": 90}),
            ("NotoSansArabic-UIBold.ttf", {"wght": 900, "wdth": 90}),
        ),
    },
)


def verify_source(path: Path, expected_sha256: str) -> None:
    actual_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual_sha256 != expected_sha256:
        raise RuntimeError(f"Unexpected source font hash for {path}: {actual_sha256}")


def generate_instance(source_path: Path, output_path: Path, axes: dict[str, float]) -> None:
    source_font = TTFont(source_path, recalcTimestamp=False)
    try:
        instance = instantiateVariableFont(
            source_font,
            axes,
            optimize=False,
        )
        try:
            fd, temporary_name = tempfile.mkstemp(suffix=".ttf", dir=output_path.parent)
            os.close(fd)
            temporary_path = Path(temporary_name)
            try:
                instance.save(temporary_path)
                temporary_path.chmod(0o644)
                generated = temporary_path.read_bytes()
                if output_path.exists() and output_path.read_bytes() == generated:
                    temporary_path.unlink()
                    output_path.chmod(0o644)
                else:
                    temporary_path.replace(output_path)
            except Exception:
                temporary_path.unlink(missing_ok=True)
                raise
        finally:
            instance.close()
    finally:
        source_font.close()


def main() -> None:
    for family in FAMILIES:
        family_dir = SOURCE_DIR / family["directory"]
        source_path = family_dir / family["source"]
        verify_source(source_path, family["sha256"])
        for output_name, axes in family["instances"]:
            output_path = family_dir / output_name
            generate_instance(source_path, output_path, axes)
            axis_description = ", ".join(f"{name}={value}" for name, value in axes.items())
            print(f"Generated {output_path.name} ({axis_description})")


if __name__ == "__main__":
    main()
