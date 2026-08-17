#!/usr/bin/env python3
"""Train the shared GlyphStream v1 context trees from bitmap dumps."""

import argparse
import sys

from glyphstream import emit_model_header, load_bitmap_dump, train_model


def main() -> int:
    parser = argparse.ArgumentParser(description="Train the shared GlyphStream v1 model.")
    parser.add_argument("dumps", nargs="+", help="Bitmap dump .npz files produced by fontconvert.py.")
    parser.add_argument("--output", required=True, help="Generated glyphStreamModel.h path.")
    args = parser.parse_args()

    corpora = []
    for path in sorted(args.dumps):
        font_name, glyphs, is_2bit = load_bitmap_dump(path)
        print(f"training: {font_name}: {len(glyphs)} glyphs", file=sys.stderr)
        corpora.append((glyphs, is_2bit))

    model = train_model(corpora)
    emit_model_header(model, args.output)
    print(
        f"model: ink={len(model.ink.probabilities)} leaves, "
        f"hi={len(model.hi.probabilities)} leaves, lo={len(model.lo.probabilities)} leaves",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
