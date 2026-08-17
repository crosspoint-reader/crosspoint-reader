"""Shared GlyphStream v1 encoder, trainer, and validation primitives."""

from __future__ import annotations

import heapq
import math
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np


TOP_VALUE = 1 << 24
HAS_REF_FLAG = 0x80
RAW_FLAG = 0x40
SHIFT_BIAS = 7
MIN_SHIFT = -7
MAX_SHIFT = 8
MAX_GLYPH_WIDTH = 63
MAX_GLYPH_HEIGHT = 46


@dataclass(frozen=True)
class GlyphBitmap:
    width: int
    height: int
    pixels: list[int]
    codepoint: int = 0


@dataclass(frozen=True)
class Reference:
    base_index: int = -1
    shift: int = 0
    depth: int = 0


@dataclass(frozen=True)
class TreeNode:
    feature: int
    child0: int
    child1: int


@dataclass(frozen=True)
class TreeModel:
    nodes: tuple[TreeNode, ...]
    probabilities: tuple[int, ...]


@dataclass(frozen=True)
class GlyphStreamModel:
    ink: TreeModel
    hi: TreeModel
    lo: TreeModel


class RangeEncoder:
    def __init__(self) -> None:
        self.low = 0
        self.range = 0xFFFFFFFF
        self.cache = 0
        self.pending = 1
        self.output = bytearray()

    def encode_bit(self, p1: int, bit: int) -> None:
        bound = (self.range >> 16) * p1
        if bit:
            self.range = bound
        else:
            self.low += bound
            self.range -= bound

        while self.range < TOP_VALUE:
            self._shift_low()
            self.range = (self.range << 8) & 0xFFFFFFFF

    def _shift_low(self) -> None:
        low32 = self.low & 0xFFFFFFFF
        carry = self.low >> 32
        if low32 < 0xFF000000 or carry != 0:
            byte = self.cache
            while self.pending > 0:
                self.output.append((byte + carry) & 0xFF)
                byte = 0xFF
                self.pending -= 1
            self.cache = (low32 >> 24) & 0xFF

        self.pending += 1
        self.low = (low32 & 0x00FFFFFF) << 8

    def finish(self) -> bytes:
        # Any value in [low, low + range) decodes identically; pick the one
        # with the most trailing zero bits so the zero-truncation below can
        # actually drop flush bytes (range >= 2^24 guarantees >= 3 of them).
        top = self.low + self.range
        for zero_bits in range(40, -1, -1):
            aligned = ((self.low + (1 << zero_bits) - 1) >> zero_bits) << zero_bits
            if aligned < top:
                self.low = aligned
                break

        for _ in range(5):
            self._shift_low()

        if not self.output or self.output[0] != 0:
            raise AssertionError("range coder leading byte is not zero")

        payload = self.output[1:]
        while payload and payload[-1] == 0:
            payload.pop()
        return bytes(payload)


class RangeDecoder:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.pos = 0
        self.range = 0xFFFFFFFF
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._read_byte()) & 0xFFFFFFFF

    def _read_byte(self) -> int:
        if self.pos >= len(self.payload):
            return 0
        value = self.payload[self.pos]
        self.pos += 1
        return value

    def decode_bit(self, p1: int) -> int:
        bound = (self.range >> 16) * p1
        if self.code < bound:
            bit = 1
            self.range = bound
        else:
            bit = 0
            self.code = (self.code - bound) & 0xFFFFFFFF
            self.range = (self.range - bound) & 0xFFFFFFFF

        while self.range < TOP_VALUE:
            self.code = ((self.code << 8) | self._read_byte()) & 0xFFFFFFFF
            self.range = (self.range << 8) & 0xFFFFFFFF
        return bit


def packed_size(width: int, height: int, is_2bit: bool) -> int:
    pixel_count = width * height
    pixels_per_byte = 4 if is_2bit else 8
    return (pixel_count + pixels_per_byte - 1) // pixels_per_byte


def pack_bitmap(pixels: list[int], is_2bit: bool) -> bytes:
    bits_per_pixel = 2 if is_2bit else 1
    mask = 0x3 if is_2bit else 0x1
    output = bytearray((len(pixels) * bits_per_pixel + 7) // 8)
    for index, pixel in enumerate(pixels):
        bit_offset = index * bits_per_pixel
        shift = 8 - bits_per_pixel - (bit_offset & 7)
        output[bit_offset >> 3] |= (pixel & mask) << shift
    return bytes(output)


def unpack_bitmap(packed: bytes, width: int, height: int, is_2bit: bool) -> list[int]:
    bits_per_pixel = 2 if is_2bit else 1
    mask = 0x3 if is_2bit else 0x1
    pixels = []
    for index in range(width * height):
        bit_offset = index * bits_per_pixel
        shift = 8 - bits_per_pixel - (bit_offset & 7)
        pixels.append((packed[bit_offset >> 3] >> shift) & mask)
    return pixels


def _pixel(glyph: GlyphBitmap, x: int, y: int) -> int:
    if x < 0 or x >= glyph.width or y < 0 or y >= glyph.height:
        return 0
    return glyph.pixels[y * glyph.width + x]


def ink_features(glyph: GlyphBitmap, x: int, y: int, base: GlyphBitmap | None, shift: int) -> tuple[int, ...]:
    def ink(dx: int, dy: int) -> int:
        return int(_pixel(glyph, x + dx, y + dy) > 0)

    def base_ink(dy: int) -> int:
        return int(base is not None and _shifted_pixel(base, x, y + dy, shift) > 0)

    return (
        ink(-1, 0),
        ink(0, -1),
        ink(-1, -1),
        ink(1, -1),
        ink(-2, 0),
        ink(0, -2),
        ink(-1, -2),
        ink(1, -2),
        ink(-3, 0),
        ink(2, -1),
        ink(2, -2),
        ink(-2, -1),
        int(4 * x < glyph.width),
        int(4 * x >= 3 * glyph.width),
        int(4 * y < glyph.height),
        int(4 * y >= 3 * glyph.height),
        base_ink(0),
        base_ink(1),
    )


def gray_features(glyph: GlyphBitmap, x: int, y: int, base: GlyphBitmap | None, shift: int) -> tuple[int, ...]:
    west = _pixel(glyph, x - 1, y)
    north = _pixel(glyph, x, y - 1)
    northeast = _pixel(glyph, x + 1, y - 1)
    northwest = _pixel(glyph, x - 1, y - 1)
    base_value = _shifted_pixel(base, x, y, shift) if base is not None else 0
    return (
        west & 1,
        (west >> 1) & 1,
        north & 1,
        (north >> 1) & 1,
        northeast & 1,
        (northeast >> 1) & 1,
        northwest & 1,
        (northwest >> 1) & 1,
        int(_pixel(glyph, x, y + 1) > 0),
        int(_pixel(glyph, x - 1, y + 1) > 0),
        int(_pixel(glyph, x + 1, y + 1) > 0),
        base_value & 1,
        (base_value >> 1) & 1,
        int(4 * x < glyph.width),
        int(4 * x >= 3 * glyph.width),
        int(4 * y < glyph.height),
        int(4 * y >= 3 * glyph.height),
    )


def tree_probability(tree: TreeModel, features: tuple[int, ...]) -> int:
    if not tree.nodes:
        return tree.probabilities[0]

    entry = 0
    while entry >= 0:
        node = tree.nodes[entry]
        entry = node.child1 if features[node.feature] else node.child0
    return tree.probabilities[~entry]


def _visit_decisions(
    glyph: GlyphBitmap,
    base: GlyphBitmap | None,
    shift: int,
    is_2bit: bool,
    model: GlyphStreamModel,
    visitor,
) -> None:
    for y in range(glyph.height):
        for x in range(glyph.width):
            value = glyph.pixels[y * glyph.width + x]
            probability = tree_probability(model.ink, ink_features(glyph, x, y, base, shift))
            visitor(probability, int(value > 0))

    if not is_2bit:
        return

    for y in range(glyph.height):
        for x in range(glyph.width):
            value = glyph.pixels[y * glyph.width + x]
            if value == 0:
                continue
            features = gray_features(glyph, x, y, base, shift)
            is_black = int(value == 3)
            visitor(tree_probability(model.hi, features), is_black)
            if not is_black:
                visitor(tree_probability(model.lo, features), int(value == 2))


def model_cost(
    glyph: GlyphBitmap,
    base: GlyphBitmap | None,
    shift: int,
    is_2bit: bool,
    model: GlyphStreamModel,
) -> float:
    cost = 0.0

    def add_cost(probability: int, bit: int) -> None:
        nonlocal cost
        outcome_probability = probability if bit else 65536 - probability
        cost -= math.log2(outcome_probability / 65536.0)

    _visit_decisions(glyph, base, shift, is_2bit, model, add_cost)
    return cost


def _best_heuristic_shift(
    target: np.ndarray, candidate: np.ndarray, width: int, height: int
) -> tuple[int, int]:
    total_ink = int(np.count_nonzero(target))
    best_score: int | None = None
    best_shift = 0
    for shift in sorted(range(MIN_SHIFT, MAX_SHIFT + 1), key=lambda value: (abs(value), value)):
        first_y = max(0, -shift)
        last_y = min(height, candidate.shape[0] - shift)
        if first_y < last_y:
            target_slice = target[first_y:last_y]
            base_slice = candidate[first_y + shift : last_y + shift]
            matched_ink = int(np.count_nonzero((target_slice == base_slice) & (target_slice > 0)))
            mismatches = int(np.count_nonzero(target_slice != base_slice))
            mismatches += total_ink - int(np.count_nonzero(target_slice))
        else:
            matched_ink = 0
            mismatches = total_ink
        score = matched_ink - mismatches
        if best_score is None or score > best_score:
            best_score = score
            best_shift = shift
    return best_score if best_score is not None else -(width * height), best_shift


def _reference_direction_allowed(target: GlyphBitmap, candidate: GlyphBitmap) -> bool:
    return target.codepoint >= 0x80 or candidate.codepoint < 0x80


def select_reference(
    glyph_index: int,
    glyphs: list[GlyphBitmap],
    references: list[Reference],
    model: GlyphStreamModel,
    is_2bit: bool,
    pixel_arrays: list[np.ndarray] | None = None,
) -> Reference:
    glyph = glyphs[glyph_index]
    if glyph.width == 0 or glyph.height == 0:
        return Reference()

    if pixel_arrays is None:
        pixel_arrays = [
            np.asarray(item.pixels, dtype=np.uint8).reshape(item.height, item.width) for item in glyphs
        ]

    prior_same_width = [
        index
        for index in range(glyph_index)
        if glyphs[index].width == glyph.width
        and references[index].depth <= 1
        and _reference_direction_allowed(glyph, glyphs[index])
    ][-256:]
    target = pixel_arrays[glyph_index]
    shortlisted: list[tuple[int, int, int]] = []
    for candidate_index in prior_same_width:
        score, shift = _best_heuristic_shift(
            target,
            pixel_arrays[candidate_index],
            glyph.width,
            glyph.height,
        )
        shortlisted.append((score, candidate_index, shift))
    shortlisted.sort(key=lambda item: (-item[0], item[1], abs(item[2]), item[2]))

    no_reference_cost = model_cost(glyph, None, 0, is_2bit, model)
    choices = [
        (
            model_cost(glyph, glyphs[candidate_index], shift, is_2bit, model),
            candidate_index,
            shift,
        )
        for _, candidate_index, shift in shortlisted[:6]
    ]
    if not choices:
        return Reference()

    reference_cost, base_index, shift = min(
        choices,
        key=lambda item: (item[0], item[1], abs(item[2]), item[2]),
    )
    if reference_cost + 16 >= no_reference_cost:
        return Reference()
    return Reference(base_index, shift, references[base_index].depth + 1)


def encode_glyph(
    glyph: GlyphBitmap,
    glyphs: list[GlyphBitmap],
    reference: Reference,
    is_2bit: bool,
    model: GlyphStreamModel,
) -> bytes:
    if glyph.width == 0 or glyph.height == 0:
        return b""
    if glyph.width > MAX_GLYPH_WIDTH or glyph.height > MAX_GLYPH_HEIGHT:
        raise ValueError(f"glyph dimensions exceed GlyphStream scratch planes: {glyph.width}x{glyph.height}")
    if reference.shift < MIN_SHIFT or reference.shift > MAX_SHIFT:
        raise ValueError(f"glyph reference shift is out of range: {reference.shift}")

    base = glyphs[reference.base_index] if reference.base_index >= 0 else None
    encoder = RangeEncoder()
    _visit_decisions(glyph, base, reference.shift, is_2bit, model, encoder.encode_bit)
    payload = encoder.finish()

    if reference.base_index >= 0:
        header = HAS_REF_FLAG | (reference.shift + SHIFT_BIAS)
        prefix = bytes((header, reference.base_index & 0xFF, reference.base_index >> 8))
    else:
        prefix = b"\0"

    raw_payload = pack_bitmap(glyph.pixels, is_2bit)
    if len(prefix) + len(payload) >= 1 + len(raw_payload):
        return bytes((RAW_FLAG,)) + raw_payload

    stream = prefix + payload
    if len(stream) > 0xFFFF:
        raise ValueError(f"glyph stream exceeds uint16 dataLength: {len(stream)}")
    return stream


def encode_font(
    glyphs: list[GlyphBitmap], is_2bit: bool, model: GlyphStreamModel
) -> tuple[list[bytes], list[Reference]]:
    streams: list[bytes] = []
    references: list[Reference] = []
    pixel_arrays = [
        np.asarray(glyph.pixels, dtype=np.uint8).reshape(glyph.height, glyph.width) for glyph in glyphs
    ]

    for glyph_index, glyph in enumerate(glyphs):
        reference = select_reference(
            glyph_index,
            glyphs,
            references,
            model,
            is_2bit,
            pixel_arrays,
        )
        stream = encode_glyph(glyph, glyphs, reference, is_2bit, model)
        if stream and stream[0] == RAW_FLAG:
            reference = Reference()
        streams.append(stream)
        references.append(reference)
    return streams, references


def decode_font(
    glyphs: list[GlyphBitmap],
    streams: list[bytes],
    is_2bit: bool,
    model: GlyphStreamModel,
) -> list[GlyphBitmap]:
    if len(glyphs) != len(streams):
        raise ValueError("glyph metadata and stream counts differ")

    decoded: list[GlyphBitmap] = []
    references: list[Reference] = []
    for glyph_index, (metadata, stream) in enumerate(zip(glyphs, streams)):
        if metadata.width == 0 or metadata.height == 0:
            if stream:
                raise ValueError("empty glyph has stream bytes")
            decoded.append(GlyphBitmap(metadata.width, metadata.height, [], metadata.codepoint))
            references.append(Reference())
            continue
        if not stream:
            raise ValueError("non-empty glyph has no stream header")

        header = stream[0]
        if header & 0x30:
            raise ValueError("glyph stream reserved header bits are set")
        if header & RAW_FLAG:
            if header != RAW_FLAG:
                raise ValueError("RAW glyph stream has invalid header bits")
            raw_size = packed_size(metadata.width, metadata.height, is_2bit)
            if len(stream) < 1 + raw_size:
                raise ValueError("RAW glyph stream is truncated")
            pixels = unpack_bitmap(stream[1 : 1 + raw_size], metadata.width, metadata.height, is_2bit)
            decoded.append(GlyphBitmap(metadata.width, metadata.height, pixels, metadata.codepoint))
            references.append(Reference())
            continue

        base: GlyphBitmap | None = None
        shift = 0
        reference = Reference()
        payload_offset = 1
        if header & HAS_REF_FLAG:
            if len(stream) < 3:
                raise ValueError("referenced glyph stream has no base index")
            base_index = stream[1] | (stream[2] << 8)
            if base_index >= glyph_index:
                raise ValueError("glyph stream base is not an earlier glyph")
            if glyphs[base_index].width != metadata.width:
                raise ValueError("glyph stream base width differs")
            if references[base_index].depth >= 2:
                raise ValueError("glyph stream reference chain exceeds depth two")
            shift = (header & 0x0F) - SHIFT_BIAS
            base = decoded[base_index]
            reference = Reference(base_index, shift, references[base_index].depth + 1)
            payload_offset = 3
        elif header & 0x0F:
            raise ValueError("unreferenced glyph stream has a shift")

        pixels = [0] * (metadata.width * metadata.height)
        glyph = GlyphBitmap(metadata.width, metadata.height, pixels, metadata.codepoint)
        decoder = RangeDecoder(stream[payload_offset:])
        for y in range(metadata.height):
            for x in range(metadata.width):
                probability = tree_probability(model.ink, ink_features(glyph, x, y, base, shift))
                pixels[y * metadata.width + x] = decoder.decode_bit(probability)

        if is_2bit:
            for y in range(metadata.height):
                for x in range(metadata.width):
                    pixel_index = y * metadata.width + x
                    if pixels[pixel_index] == 0:
                        continue
                    features = gray_features(glyph, x, y, base, shift)
                    is_black = decoder.decode_bit(tree_probability(model.hi, features))
                    if is_black:
                        pixels[pixel_index] = 3
                    else:
                        pixels[pixel_index] = 2 if decoder.decode_bit(tree_probability(model.lo, features)) else 1

        decoded.append(glyph)
        references.append(reference)
    return decoded


def save_bitmap_dump(path: str | Path, font_name: str, glyphs: list[GlyphBitmap], is_2bit: bool) -> None:
    offsets = [0]
    flat_pixels: list[int] = []
    for glyph in glyphs:
        flat_pixels.extend(glyph.pixels)
        offsets.append(len(flat_pixels))

    np.savez_compressed(
        path,
        font_name=np.asarray(font_name),
        pixels=np.asarray(flat_pixels, dtype=np.uint8),
        offsets=np.asarray(offsets, dtype=np.uint32),
        glyph_indices=np.arange(len(glyphs), dtype=np.uint32),
        codepoints=np.asarray([glyph.codepoint for glyph in glyphs], dtype=np.uint32),
        widths=np.asarray([glyph.width for glyph in glyphs], dtype=np.uint8),
        heights=np.asarray([glyph.height for glyph in glyphs], dtype=np.uint8),
        is_2bit=np.asarray(is_2bit, dtype=np.uint8),
    )


def load_bitmap_dump(path: str | Path) -> tuple[str, list[GlyphBitmap], bool]:
    with np.load(path, allow_pickle=False) as dump:
        pixels = dump["pixels"]
        offsets = dump["offsets"]
        widths = dump["widths"]
        heights = dump["heights"]
        codepoints = dump["codepoints"]
        glyph_indices = dump["glyph_indices"]
        if len(offsets) != len(widths) + 1 or len(glyph_indices) != len(widths):
            raise ValueError("invalid glyph dump array lengths")
        if not np.array_equal(glyph_indices, np.arange(len(widths), dtype=np.uint32)):
            raise ValueError("glyph dump indices are not contiguous")
        glyphs = [
            GlyphBitmap(
                int(widths[index]),
                int(heights[index]),
                pixels[int(offsets[index]) : int(offsets[index + 1])].astype(np.uint8).tolist(),
                int(codepoints[index]),
            )
            for index in range(len(widths))
        ]
        return str(dump["font_name"]), glyphs, bool(dump["is_2bit"])


def emit_model_header(model: GlyphStreamModel, path: str | Path) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#define GLYPH_STREAM_MODEL_VERSION 1",
        "",
        "typedef struct {",
        "  uint16_t feature;",
        "  int16_t child0;",
        "  int16_t child1;",
        "} GlyphStreamTreeNode;",
        "",
    ]

    for prefix, tree in (("INK", model.ink), ("HI", model.hi), ("LO", model.lo)):
        title = prefix.title()
        lines.append(f"#define GLYPH_STREAM_{prefix}_NODE_COUNT {len(tree.nodes)}")
        lines.append(f"#define GLYPH_STREAM_{prefix}_PROB_COUNT {len(tree.probabilities)}")
        lines.append(f"static constexpr GlyphStreamTreeNode k{title}Tree[] = {{")
        if tree.nodes:
            lines.extend(f"    {{ {node.feature}, {node.child0}, {node.child1} }}," for node in tree.nodes)
        else:
            lines.append("    { 0, -1, -1 },")
        lines.append("};")
        lines.append(f"static constexpr uint16_t k{title}Probs[] = {{")
        for start in range(0, len(tree.probabilities), 12):
            values = ", ".join(str(value) for value in tree.probabilities[start : start + 12])
            lines.append(f"    {values},")
        lines.append("};")
        lines.append("")

    Path(path).write_text("\n".join(lines), encoding="utf-8")


def load_model_header(path: str | Path) -> GlyphStreamModel:
    source = Path(path).read_text(encoding="utf-8")

    def parse_tree(prefix: str) -> TreeModel:
        title = prefix.title()
        node_count_match = re.search(rf"#define GLYPH_STREAM_{prefix}_NODE_COUNT (\d+)", source)
        prob_count_match = re.search(rf"#define GLYPH_STREAM_{prefix}_PROB_COUNT (\d+)", source)
        nodes_match = re.search(rf"k{title}Tree\[\]\s*=\s*\{{(.*?)\}};", source, re.DOTALL)
        probs_match = re.search(rf"k{title}Probs\[\]\s*=\s*\{{(.*?)\}};", source, re.DOTALL)
        if not node_count_match or not prob_count_match or not nodes_match or not probs_match:
            raise ValueError(f"model header is missing {prefix} arrays")

        nodes = tuple(
            TreeNode(int(feature), int(child0), int(child1))
            for feature, child0, child1 in re.findall(r"\{\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", nodes_match.group(1))
        )
        probabilities = tuple(int(value) for value in re.findall(r"\d+", probs_match.group(1)))
        node_count = int(node_count_match.group(1))
        prob_count = int(prob_count_match.group(1))
        if len(nodes) < node_count or len(probabilities) != prob_count:
            raise ValueError(f"model header has inconsistent {prefix} counts")
        return TreeModel(nodes[:node_count], probabilities)

    return GlyphStreamModel(parse_tree("INK"), parse_tree("HI"), parse_tree("LO"))


def _shifted_pixel(glyph: GlyphBitmap, x: int, y: int, shift: int) -> int:
    source_y = y + shift
    if x < 0 or x >= glyph.width or source_y < 0 or source_y >= glyph.height:
        return 0
    return glyph.pixels[source_y * glyph.width + x]


def bootstrap_references(glyphs: list[GlyphBitmap]) -> list[Reference]:
    references: list[Reference] = []
    prior_by_width: dict[int, list[int]] = {}
    shift_order = sorted(range(-7, 9), key=lambda shift: (abs(shift), shift))
    pixel_arrays = [
        np.asarray(glyph.pixels, dtype=np.uint8).reshape(glyph.height, glyph.width)
        for glyph in glyphs
    ]

    for glyph_index, glyph in enumerate(glyphs):
        best_score: int | None = None
        best_base = -1
        best_shift = 0
        target_pixels = pixel_arrays[glyph_index]
        total_ink = int(np.count_nonzero(target_pixels))
        candidates = prior_by_width.get(glyph.width, [])[-256:]

        for candidate_index in candidates:
            if references[candidate_index].depth >= 2:
                continue
            candidate = glyphs[candidate_index]
            if not _reference_direction_allowed(glyph, candidate):
                continue
            candidate_pixels = pixel_arrays[candidate_index]
            candidate_best: tuple[int, int] | None = None
            for shift in shift_order:
                first_y = max(0, -shift)
                last_y = min(glyph.height, candidate.height - shift)
                if first_y < last_y:
                    target_slice = target_pixels[first_y:last_y]
                    base_slice = candidate_pixels[first_y + shift : last_y + shift]
                    matched_ink = int(np.count_nonzero((target_slice == base_slice) & (target_slice > 0)))
                    mismatches = int(np.count_nonzero(target_slice != base_slice))
                    mismatches += total_ink - int(np.count_nonzero(target_slice))
                else:
                    matched_ink = 0
                    mismatches = total_ink
                if matched_ink < 1 or matched_ink * 2 < total_ink or mismatches * 4 > glyph.width * glyph.height:
                    continue
                score = matched_ink - mismatches
                if candidate_best is None or score > candidate_best[0]:
                    candidate_best = (score, shift)

            if candidate_best is not None and (best_score is None or candidate_best[0] > best_score):
                best_score = candidate_best[0]
                best_base = candidate_index
                best_shift = candidate_best[1]

        if best_base >= 0:
            references.append(Reference(best_base, best_shift, references[best_base].depth + 1))
        else:
            references.append(Reference())
        prior_by_width.setdefault(glyph.width, []).append(glyph_index)

    return references


@dataclass
class _TrainingLeaf:
    creation: int
    counts: dict[int, tuple[int, int]]
    used_features: frozenset[int]
    parent: "_TrainingBranch | None" = None
    parent_side: int = 0


@dataclass
class _TrainingBranch:
    feature: int
    child0: "_TrainingLeaf | _TrainingBranch"
    child1: "_TrainingLeaf | _TrainingBranch"


def _training_bits(counts: dict[int, tuple[int, int]]) -> float:
    n0 = sum(count[0] for count in counts.values())
    n1 = sum(count[1] for count in counts.values())
    p1 = (n1 + 0.5) / (n0 + n1 + 1.0)
    return -n1 * math.log2(p1) - n0 * math.log2(1.0 - p1)


def _quantized_probability(counts: dict[int, tuple[int, int]]) -> int:
    n0 = sum(count[0] for count in counts.values())
    n1 = sum(count[1] for count in counts.values())
    p1 = (n1 + 0.5) / (n0 + n1 + 1.0)
    return max(128, min(65408, round(p1 * 65536)))


def _best_split(
    leaf: _TrainingLeaf, feature_count: int
) -> tuple[float, int, dict[int, tuple[int, int]], dict[int, tuple[int, int]]] | None:
    current_bits = _training_bits(leaf.counts)
    best: tuple[float, int, dict[int, tuple[int, int]], dict[int, tuple[int, int]]] | None = None
    for feature in range(feature_count):
        if feature in leaf.used_features:
            continue
        zero = {vector: count for vector, count in leaf.counts.items() if not ((vector >> feature) & 1)}
        one = {vector: count for vector, count in leaf.counts.items() if (vector >> feature) & 1}
        if not zero or not one:
            continue
        gain = current_bits - _training_bits(zero) - _training_bits(one)
        if gain <= 1e-12:
            continue
        if best is None or gain > best[0] + 1e-12 or (abs(gain - best[0]) <= 1e-12 and feature < best[1]):
            best = (gain, feature, zero, one)
    return best


def grow_tree_from_counts(counts: dict[int, tuple[int, int]], feature_count: int, leaf_budget: int) -> TreeModel:
    if not counts:
        return TreeModel((), (32768,))

    next_creation = 1
    root: _TrainingLeaf | _TrainingBranch = _TrainingLeaf(0, counts, frozenset())
    leaves: dict[int, _TrainingLeaf] = {0: root}
    candidates: list[
        tuple[float, int, int, _TrainingLeaf, dict[int, tuple[int, int]], dict[int, tuple[int, int]]]
    ] = []

    def queue_split(leaf: _TrainingLeaf) -> None:
        split = _best_split(leaf, feature_count)
        if split is None:
            return
        gain, feature, zero, one = split
        heapq.heappush(candidates, (-gain, feature, leaf.creation, leaf, zero, one))

    queue_split(root)

    while len(leaves) < leaf_budget and candidates:
        _, feature, _, leaf, zero_counts, one_counts = heapq.heappop(candidates)
        used = leaf.used_features | {feature}
        child0 = _TrainingLeaf(next_creation, zero_counts, used)
        next_creation += 1
        child1 = _TrainingLeaf(next_creation, one_counts, used)
        next_creation += 1
        branch = _TrainingBranch(feature, child0, child1)
        child0.parent = branch
        child0.parent_side = 0
        child1.parent = branch
        child1.parent_side = 1

        if leaf.parent is None:
            root = branch
        elif leaf.parent_side == 0:
            leaf.parent.child0 = branch
        else:
            leaf.parent.child1 = branch
        del leaves[leaf.creation]
        leaves[child0.creation] = child0
        leaves[child1.creation] = child1
        queue_split(child0)
        queue_split(child1)

    ordered_leaves = sorted(leaves.values(), key=lambda leaf: leaf.creation)
    leaf_indices = {leaf.creation: index for index, leaf in enumerate(ordered_leaves)}
    nodes: list[TreeNode | None] = []

    def emit(entry: _TrainingLeaf | _TrainingBranch) -> int:
        if isinstance(entry, _TrainingLeaf):
            return ~leaf_indices[entry.creation]
        node_index = len(nodes)
        nodes.append(None)
        child0 = emit(entry.child0)
        child1 = emit(entry.child1)
        nodes[node_index] = TreeNode(entry.feature, child0, child1)
        return node_index

    root_index = emit(root)
    if root_index < 0:
        return TreeModel((), (_quantized_probability(root.counts),))
    return TreeModel(
        tuple(node for node in nodes if node is not None),
        tuple(_quantized_probability(leaf.counts) for leaf in ordered_leaves),
    )


def grow_tree(samples: list[tuple[tuple[int, ...], int]], leaf_budget: int) -> TreeModel:
    if not samples:
        return TreeModel((), (32768,))
    feature_count = len(samples[0][0])
    combined: dict[int, list[int]] = {}
    for features, outcome in samples:
        vector = sum((bit & 1) << feature for feature, bit in enumerate(features))
        combined.setdefault(vector, [0, 0])[outcome & 1] += 1
    counts = {vector: (values[0], values[1]) for vector, values in combined.items()}
    return grow_tree_from_counts(counts, feature_count, leaf_budget)


def _feature_vector(features: tuple[int, ...]) -> int:
    return sum((bit & 1) << feature for feature, bit in enumerate(features))


def _add_outcome(counts: dict[int, list[int]], vector: int, outcome: int) -> None:
    counts.setdefault(vector, [0, 0])[outcome & 1] += 1


def accumulate_training_counts(
    glyphs: list[GlyphBitmap], is_2bit: bool
) -> tuple[dict[int, tuple[int, int]], dict[int, tuple[int, int]], dict[int, tuple[int, int]]]:
    references = bootstrap_references(glyphs)
    ink_counts: dict[int, list[int]] = {}
    hi_counts: dict[int, list[int]] = {}
    lo_counts: dict[int, list[int]] = {}

    for glyph_index, glyph in enumerate(glyphs):
        reference = references[glyph_index]
        base = glyphs[reference.base_index] if reference.base_index >= 0 else None
        for y in range(glyph.height):
            for x in range(glyph.width):
                value = glyph.pixels[y * glyph.width + x]
                ink_vector = _feature_vector(ink_features(glyph, x, y, base, reference.shift))
                _add_outcome(ink_counts, ink_vector, int(value > 0))
                if not is_2bit or value == 0:
                    continue
                gray_vector = _feature_vector(gray_features(glyph, x, y, base, reference.shift))
                is_black = int(value == 3)
                _add_outcome(hi_counts, gray_vector, is_black)
                if not is_black:
                    _add_outcome(lo_counts, gray_vector, int(value == 2))

    def freeze(counts: dict[int, list[int]]) -> dict[int, tuple[int, int]]:
        return {vector: (values[0], values[1]) for vector, values in counts.items()}

    return freeze(ink_counts), freeze(hi_counts), freeze(lo_counts)


def train_model(
    corpora: list[tuple[list[GlyphBitmap], bool]], leaf_budgets: tuple[int, int, int] = (512, 256, 128)
) -> GlyphStreamModel:
    merged: tuple[dict[int, list[int]], dict[int, list[int]], dict[int, list[int]]] = ({}, {}, {})
    for glyphs, is_2bit in corpora:
        font_counts = accumulate_training_counts(glyphs, is_2bit)
        for destination, source in zip(merged, font_counts):
            for vector, (n0, n1) in source.items():
                aggregate = destination.setdefault(vector, [0, 0])
                aggregate[0] += n0
                aggregate[1] += n1

    frozen = tuple(
        {vector: (values[0], values[1]) for vector, values in counts.items()} for counts in merged
    )
    return GlyphStreamModel(
        grow_tree_from_counts(frozen[0], 18, leaf_budgets[0]),
        grow_tree_from_counts(frozen[1], 17, leaf_budgets[1]),
        grow_tree_from_counts(frozen[2], 17, leaf_budgets[2]),
    )
