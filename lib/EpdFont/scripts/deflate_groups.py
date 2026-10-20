"""Raw-DEFLATE encoding of built-in font glyph groups, shared by fontconvert.py (the reader faces) and
compress_ui_face.py (the 1-bit UI faces).

The device decodes every group with uzlib through FontDecompressor::GroupStream, streaming it through a
ring of EpdFontGroup::ringBytes. So an encoder here has two jobs: produce the smallest stream it can,
and report the largest back-reference that stream actually contains, which is the ring the device
must hold. Both encoders of built-in faces use this module so that the two cannot disagree on either.
"""

import zlib

# EpdFontGroup::ringBytes is the RAM the on-device decoder needs to stream a group, and it is the
# largest back-reference the finished stream actually contains -- not the group size and not 2^wbits,
# both of which overstate it. So measure the real streams.
#
# It has to be read out of the bitstream. Probing by decompressing at a small -wbits does NOT work:
# zlib-ng (which ships with Python 3.14) does not enforce the window on raw inflate and happily
# decodes a stream with a verified 24 KB reference at wbits=9.

_LEN_EXTRA = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0]
_DIST_BASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
              1025,1537,2049,3073,4097,6145,8193,12289,16385,24577]
_DIST_EXTRA = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13]
_CLEN_ORDER = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15]


def deflate_raw(data, wbits=15, force_zlib=False, use_zopfli=False):
    """Raw-DEFLATE compress `data` (no zlib/gzip wrapper), decodable on-device by
    uzlib via inflate(wbits=-15). Uses Zopfli when `use_zopfli` is set (a few percent
    smaller than zlib -9, much slower — fine at font-generation time), else zlib -9.
    Zopfli's Python binding only emits zlib-wrapped output, so strip the 2-byte
    header and 4-byte adler32 trailer to recover the raw DEFLATE block. A round-trip
    assert guards against any wrapper-format surprise.

    `wbits` bounds how far back the encoder may reference, which is what bounds the
    decoder's ring (see measure_ring). Zopfli has no such knob — its window is a
    compile-time constant — so force_zlib selects the bounded encoder instead."""
    if use_zopfli and not force_zlib:
        import zopfli.zlib
        wrapped = zopfli.zlib.compress(bytes(data))
        raw = wrapped[2:-4]  # drop zlib CMF/FLG header + adler32 trailer
    else:
        compressor = zlib.compressobj(level=9, wbits=-wbits)
        raw = compressor.compress(bytes(data)) + compressor.flush()
    assert zlib.decompress(raw, -15) == bytes(data), "raw-DEFLATE round-trip failed"
    return raw


class _BitReader:
    def __init__(self, data):
        self.d, self.pos = data, 0

    def bit(self):
        b = (self.d[self.pos >> 3] >> (self.pos & 7)) & 1
        self.pos += 1
        return b

    def bits(self, n):
        v = 0
        for i in range(n):
            v |= self.bit() << i
        return v


def _huffman(lengths):
    """Canonical Huffman code lengths -> {(bit_length, code): symbol}."""
    maxlen = max(lengths) if lengths else 0
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code, next_code = 0, [0] * (maxlen + 2)
    for b in range(1, maxlen + 1):
        code = (code + bl_count[b - 1]) << 1
        next_code[b] = code
    table = {}
    for sym, l in enumerate(lengths):
        if l:
            table[(l, next_code[l])] = sym
            next_code[l] += 1
    return table


def _decode_sym(br, table):
    code, length = 0, 0
    while True:
        code = (code << 1) | br.bit()
        length += 1
        if (length, code) in table:
            return table[(length, code)]
        if length > 15:
            raise ValueError("corrupt Huffman code while measuring ring size")


_FIXED_LIT = _huffman([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8)
_FIXED_DIST = _huffman([5] * 30)


def measure_ring(raw):
    """Largest back-reference distance in a raw-DEFLATE stream, i.e. the smallest ring that
    decodes it. 0 when the stream is pure literals."""
    br = _BitReader(raw)
    best = 0
    while True:
        final = br.bit()
        btype = br.bits(2)
        if btype == 0:                      # stored
            br.pos = (br.pos + 7) & ~7
            n = br.bits(16)
            br.bits(16)
            br.pos += n * 8
        else:
            if btype == 1:
                lit, dist = _FIXED_LIT, _FIXED_DIST
            elif btype == 2:
                hlit, hdist, hclen = br.bits(5) + 257, br.bits(5) + 1, br.bits(4) + 4
                cl = [0] * 19
                for i in range(hclen):
                    cl[_CLEN_ORDER[i]] = br.bits(3)
                cl_tab = _huffman(cl)
                lengths = []
                while len(lengths) < hlit + hdist:
                    s = _decode_sym(br, cl_tab)
                    if s < 16:
                        lengths.append(s)
                    elif s == 16:
                        lengths += [lengths[-1]] * (br.bits(2) + 3)
                    elif s == 17:
                        lengths += [0] * (br.bits(3) + 3)
                    else:
                        lengths += [0] * (br.bits(7) + 11)
                lit = _huffman(lengths[:hlit])
                dist = _huffman(lengths[hlit:hlit + hdist])
            else:
                raise ValueError("reserved DEFLATE block type")
            while True:
                sym = _decode_sym(br, lit)
                if sym == 256:
                    break
                if sym < 256:
                    continue
                br.bits(_LEN_EXTRA[sym - 257])
                ds = _decode_sym(br, dist)
                best = max(best, _DIST_BASE[ds] + br.bits(_DIST_EXTRA[ds]))
        if final:
            return best


def encode_block(blob, ring_max, use_zopfli):
    """Smallest raw-DEFLATE encoding of `blob` whose back-references fit `ring_max`, as (raw, ring).
    Zopfli first (when enabled) — it wins outright whenever its distances happen to fit — then
    progressively tighter bounded-window zlib. wbits=10 caps distances at 1024, so a fitting
    candidate always exists for any ring_max of at least 1024, and for a blob no longer than
    ring_max, which no stream can reference past."""
    best = None
    candidates = ([(15, False)] if use_zopfli else []) + [(w, True) for w in (15, 14, 13, 12, 11, 10)]
    for wbits, force_zlib in candidates:
        raw = deflate_raw(blob, wbits=wbits, force_zlib=force_zlib, use_zopfli=use_zopfli)
        ring = min(measure_ring(raw), len(blob))
        if ring > ring_max:
            continue
        if best is None or len(raw) < len(best[0]):
            best = (raw, ring)
    assert best is not None, "no encoding fits the ring ceiling (should be unreachable)"
    return best
