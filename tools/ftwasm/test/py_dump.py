"""Dump the glyph data fontconvert_sdcard.py computes, for the parity test.

Runs the converter's own rasterize_font_style() (imported from the repo, not
re-implemented) and writes one JSON file per (font, size):

    python -I -B py_dump.py --repo <witchhunt-reader> --jobs jobs.json --out-dir <dir>

jobs.json: [{"id": "...", "font": "/abs/path.ttf", "size": 14, "intervals": "builtin,..."}, ...]
Each output: {"freetype": "2.13.2", "intervals": [[s,e],...], "advanceY", "ascender",
              "descender", "totalBitmapSize", "seconds",
              "glyphs": [[cp, w, h, adv, left, top, len, off, hex], ...]}
"""
import argparse
import contextlib
import io
import json
import os
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--jobs", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    sys.dont_write_bytecode = True  # never drop .pyc files into the repo
    sys.path.insert(0, os.path.join(args.repo, "lib", "EpdFont", "scripts"))
    import freetype  # noqa: E402
    import fontconvert_sdcard as fc  # noqa: E402

    ft_version = ".".join(str(x) for x in freetype.version())
    with open(args.jobs) as f:
        jobs = json.load(f)
    os.makedirs(args.out_dir, exist_ok=True)
    for job in jobs:
        intervals = fc.resolve_intervals(job["intervals"])
        t0 = time.perf_counter()
        with contextlib.redirect_stderr(io.StringIO()):
            sd = fc.rasterize_font_style(job["font"], job["size"], intervals, style_id=job.get("style", 0))
        dt = time.perf_counter() - t0
        out = {
            "freetype": ft_version,
            "lib": freetype.raw._lib._name,
            "intervals": [list(iv) for iv in sd.intervals],
            "advanceY": sd.advanceY,
            "ascender": sd.ascender,
            "descender": sd.descender,
            "totalBitmapSize": sd.total_bitmap_size,
            "seconds": dt,
            "glyphs": [[g.code_point, g.width, g.height, g.advance_x, g.left, g.top,
                        g.data_length, g.data_offset, packed.hex()] for g, packed in sd.all_glyphs],
        }
        with open(os.path.join(args.out_dir, job["id"] + ".json"), "w") as f:
            json.dump(out, f)
        print(f"{job['id']}: {len(sd.all_glyphs)} glyphs, {dt:.2f}s", flush=True)


if __name__ == "__main__":
    main()
