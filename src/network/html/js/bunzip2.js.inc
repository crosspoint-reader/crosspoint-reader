/*!
 * bunzip2 for the dictionary installer: decodes bzip2 data one block at a time.
 *
 * Vendored from seek-bzip 2.0.0 (https://github.com/cscott/seek-bzip), MIT licence:
 *   Copyright (C) 2013-2015 C. Scott Ananian
 *   Copyright (C) 2012-2015 Eli Skeggs
 *   Copyright (C) 2011 Kevin Kwok
 *   CRC32 after jbzip2, Copyright (c) 2011 Matthew Francis (same terms)
 * Based on micro-bunzip by Rob Landley, after the bzip2 decompression code by
 * Julian R Seward, which also acknowledges contributions by Mike Burrows,
 * David Wheeler, Peter Fenwick, Alistair Moffat, Radford Neal, Ian H. Witten,
 * Robert Sedgewick, and Jon L. Bentley.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Changed here: Node's Buffer and stream classes are replaced by a reader over
 * one Uint8Array; nextBlock() hands back each block's bytes as it is decoded,
 * so a large .tar.bz2 is never held decompressed as a whole and the page can
 * yield to the browser between blocks; concatenated streams (pbzip2) are
 * followed; the CRC table is generated rather than spelled out; the seek and
 * block-table helpers are dropped.
 */
const Bunzip2 = (function () {
  'use strict';

  const MAX_HUFCODE_BITS = 20;
  const MAX_SYMBOLS = 258;
  const SYMBOL_RUNA = 0;
  const SYMBOL_RUNB = 1;
  const MIN_GROUPS = 2;
  const MAX_GROUPS = 6;
  const GROUP_SIZE = 50;

  // The two 48-bit block signatures: digits of pi (a data block) and of
  // sqrt(pi) (end of stream). Exact as doubles.
  const WHOLEPI = 0x314159265359;
  const SQRTPI = 0x177245385090;

  const BITMASK = [0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff];

  // bzip2's CRC-32: polynomial 0x04c11db7, most significant bit first.
  const CRC_TABLE = (function () {
    const table = new Uint32Array(256);
    for (let i = 0; i < 256; i++) {
      let c = i << 24;
      for (let k = 0; k < 8; k++) c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : c << 1;
      table[i] = c >>> 0;
    }
    return table;
  })();

  function fail(detail) {
    throw new Error('Not a valid bzip2 file: ' + detail);
  }

  function mtf(array, index) {
    const src = array[index];
    for (let i = index; i > 0; i--) array[i] = array[i - 1];
    array[0] = src;
    return src;
  }

  function BitReader(bytes, pos) {
    this.bytes = bytes;
    this.pos = pos;
    this.bitOffset = 0;
    this.curByte = 0;
    this.hasByte = false;
  }

  BitReader.prototype.read = function (bits) {
    let result = 0;
    while (bits > 0) {
      if (!this.hasByte) {
        if (this.pos >= this.bytes.length) fail('unexpected end of data');
        this.curByte = this.bytes[this.pos++];
        this.hasByte = true;
      }
      const remaining = 8 - this.bitOffset;
      if (bits >= remaining) {
        result <<= remaining;
        result |= BITMASK[remaining] & this.curByte;
        this.hasByte = false;
        this.bitOffset = 0;
        bits -= remaining;
      } else {
        result <<= bits;
        const shift = remaining - bits;
        result |= (this.curByte & (BITMASK[bits] << shift)) >> shift;
        this.bitOffset += bits;
        bits = 0;
      }
    }
    return result;
  };

  // A 48-bit block signature.
  BitReader.prototype.pi = function () {
    const hi = this.read(24);
    const lo = this.read(24);
    return hi * 0x1000000 + lo;
  };

  function Decoder(bytes) {
    this.bytes = bytes;
    this.done = false;
    this.dbuf = null;
    this.startStream(0);
  }

  Decoder.prototype.startStream = function (pos) {
    const b = this.bytes;
    // "BZh" and a block size of '1'..'9' hundred thousand bytes.
    if (pos + 4 > b.length || b[pos] !== 0x42 || b[pos + 1] !== 0x5a || b[pos + 2] !== 0x68) fail('bad magic');
    const level = b[pos + 3] - 0x30;
    if (level < 1 || level > 9) fail('block size out of range');
    this.reader = new BitReader(b, pos + 4);
    this.dbufSize = 100000 * level;
    this.streamCRC = 0;
  };

  // How far into the input the decoder has got, for progress.
  Decoder.prototype.inputPos = function () {
    return this.reader.pos;
  };

  // The next block's bytes, or null once the data is exhausted.
  Decoder.prototype.nextBlock = function () {
    for (;;) {
      if (this.done) return null;
      if (this.readBlock()) return this.writeBlock();
      const target = this.reader.read(32) >>> 0;
      if (target !== this.streamCRC) fail('bad stream CRC');
      // The stream is padded to a byte boundary, so the reader's position is
      // where a concatenated stream would begin.
      const next = this.reader.pos;
      const b = this.bytes;
      if (next + 4 <= b.length && b[next] === 0x42 && b[next + 1] === 0x5a && b[next + 2] === 0x68) {
        this.startStream(next);
      } else {
        this.done = true;
      }
    }
  };

  // get_next_block() from micro-bunzip: reads one block's Huffman-coded
  // symbols into dbuf and prepares the inverse BWT. False at end of stream.
  Decoder.prototype.readBlock = function () {
    let i, j, k, t;
    const reader = this.reader;
    const h = reader.pi();
    if (h === SQRTPI) return false;
    if (h !== WHOLEPI) fail('bad block header');
    this.targetBlockCRC = reader.read(32) >>> 0;
    this.streamCRC = (this.targetBlockCRC ^ ((this.streamCRC << 1) | (this.streamCRC >>> 31))) >>> 0;
    if (reader.read(1)) fail('obsolete (randomised) block');
    const origPointer = reader.read(24);
    if (origPointer > this.dbufSize) fail('initial position out of bounds');

    // Mapping table: the compressor drops byte values that never occur and
    // writes a sparse bitfield of the ones present.
    t = reader.read(16);
    const symToByte = new Uint8Array(256);
    let symTotal = 0;
    for (i = 0; i < 16; i++) {
      if (t & (1 << (0xf - i))) {
        const o = i * 16;
        k = reader.read(16);
        for (j = 0; j < 16; j++) if (k & (1 << (0xf - j))) symToByte[symTotal++] = o + j;
      }
    }

    const groupCount = reader.read(3);
    if (groupCount < MIN_GROUPS || groupCount > MAX_GROUPS) fail('bad Huffman group count');
    // Every GROUP_SIZE symbols select a new Huffman group; the selector list is
    // MTF-encoded unary runs.
    const nSelectors = reader.read(15);
    if (nSelectors === 0) fail('no selectors');
    const mtfSymbol = new Uint8Array(256);
    for (i = 0; i < groupCount; i++) mtfSymbol[i] = i;
    const selectors = new Uint8Array(nSelectors);
    for (i = 0; i < nSelectors; i++) {
      for (j = 0; reader.read(1); j++) if (j >= groupCount) fail('bad selector');
      selectors[i] = mtf(mtfSymbol, j);
    }

    // Code lengths per group, delta-coded, for symTotal literals plus RUNA/RUNB.
    let symCount = symTotal + 2;
    const groups = [];
    let hufGroup;
    for (j = 0; j < groupCount; j++) {
      const length = new Uint8Array(symCount);
      const temp = new Uint16Array(MAX_HUFCODE_BITS + 1);
      t = reader.read(5);
      for (i = 0; i < symCount; i++) {
        for (;;) {
          if (t < 1 || t > MAX_HUFCODE_BITS) fail('bad code length');
          if (!reader.read(1)) break;
          if (!reader.read(1)) t++;
          else t--;
        }
        length[i] = t;
      }
      let minLen = length[0];
      let maxLen = length[0];
      for (i = 1; i < symCount; i++) {
        if (length[i] > maxLen) maxLen = length[i];
        else if (length[i] < minLen) minLen = length[i];
      }
      // permute[] maps a code to its symbol; limit[] is the largest code value
      // at each bit length, base[] what to subtract at that length.
      hufGroup = {
        permute: new Uint16Array(MAX_SYMBOLS),
        limit: new Uint32Array(MAX_HUFCODE_BITS + 2),
        base: new Uint32Array(MAX_HUFCODE_BITS + 1),
        minLen: minLen,
        maxLen: maxLen,
      };
      groups.push(hufGroup);
      let pp = 0;
      for (i = minLen; i <= maxLen; i++) {
        temp[i] = hufGroup.limit[i] = 0;
        for (t = 0; t < symCount; t++) if (length[t] === i) hufGroup.permute[pp++] = t;
      }
      for (i = 0; i < symCount; i++) temp[length[i]]++;
      pp = t = 0;
      for (i = minLen; i < maxLen; i++) {
        pp += temp[i];
        hufGroup.limit[i] = pp - 1;
        pp <<= 1;
        t += temp[i];
        hufGroup.base[i + 1] = pp - t;
      }
      hufGroup.limit[maxLen] = pp + temp[maxLen] - 1;
      hufGroup.base[minLen] = 0;
    }

    // Undo the Huffman coding and the RUNA/RUNB run-length coding into dbuf.
    const byteCount = new Uint32Array(256);
    for (i = 0; i < 256; i++) mtfSymbol[i] = i;
    let runPos = 0;
    let dbufCount = 0;
    let selector = 0;
    let uc;
    if (!this.dbuf || this.dbuf.length !== this.dbufSize) this.dbuf = new Uint32Array(this.dbufSize);
    const dbuf = this.dbuf;
    symCount = 0;
    for (;;) {
      if (!symCount--) {
        symCount = GROUP_SIZE - 1;
        if (selector >= nSelectors) fail('ran out of selectors');
        hufGroup = groups[selectors[selector++]];
      }
      i = hufGroup.minLen;
      j = reader.read(i);
      for (;; i++) {
        if (i > hufGroup.maxLen) fail('bad Huffman code');
        if (j <= hufGroup.limit[i]) break;
        j = (j << 1) | reader.read(1);
      }
      j -= hufGroup.base[i];
      if (j < 0 || j >= MAX_SYMBOLS) fail('bad Huffman symbol');
      const nextSym = hufGroup.permute[j];
      if (nextSym === SYMBOL_RUNA || nextSym === SYMBOL_RUNB) {
        // Runs are written in bijective base 2: RUNA adds 1, RUNB 2 at each place.
        if (!runPos) {
          runPos = 1;
          t = 0;
        }
        if (nextSym === SYMBOL_RUNA) t += runPos;
        else t += 2 * runPos;
        runPos <<= 1;
        continue;
      }
      if (runPos) {
        runPos = 0;
        if (dbufCount + t > this.dbufSize) fail('run overflows the block');
        uc = symToByte[mtfSymbol[0]];
        byteCount[uc] += t;
        while (t--) dbuf[dbufCount++] = uc;
      }
      if (nextSym > symTotal) break;
      if (dbufCount >= this.dbufSize) fail('block overflows');
      uc = symToByte[mtf(mtfSymbol, nextSym - 1)];
      byteCount[uc]++;
      dbuf[dbufCount++] = uc;
    }

    // Inverse Burrows-Wheeler transform: link each byte to its successor.
    if (origPointer < 0 || origPointer >= dbufCount) fail('initial position out of bounds');
    j = 0;
    for (i = 0; i < 256; i++) {
      k = j + byteCount[i];
      byteCount[i] = j;
      j = k;
    }
    for (i = 0; i < dbufCount; i++) {
      uc = dbuf[i] & 0xff;
      dbuf[byteCount[uc]] |= i << 8;
      byteCount[uc]++;
    }
    const pos = dbuf[origPointer];
    this.writeCurrent = pos & 0xff;
    this.writePos = pos >> 8;
    this.writeCount = dbufCount;
    this.writeRun = -1;
    return true;
  };

  // Walks the BWT links and undoes the initial run-length coding (four equal
  // bytes are followed by a repeat count), checking the block CRC.
  Decoder.prototype.writeBlock = function () {
    const dbuf = this.dbuf;
    let pos = this.writePos;
    let current = this.writeCurrent;
    let run = this.writeRun;
    let count = this.writeCount;
    let out = new Uint8Array(count + (count >> 2) + 1024);
    let n = 0;
    let crc = -1;
    let copies, previous, outbyte;
    while (count--) {
      previous = current;
      pos = dbuf[pos];
      current = pos & 0xff;
      pos >>= 8;
      if (run++ === 3) {
        copies = current;
        outbyte = previous;
        current = -1;
      } else {
        copies = 1;
        outbyte = current;
      }
      if (n + copies > out.length) {
        const grown = new Uint8Array(out.length * 2 + copies);
        grown.set(out.subarray(0, n));
        out = grown;
      }
      while (copies--) {
        crc = (crc << 8) ^ CRC_TABLE[((crc >>> 24) ^ outbyte) & 0xff];
        out[n++] = outbyte;
      }
      if (current !== previous) run = 0;
    }
    if ((~crc >>> 0) !== this.targetBlockCRC) fail('bad block CRC');
    return out.subarray(0, n);
  };

  return { Decoder: Decoder };
})();
