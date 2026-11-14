#include <cmath>

#include "OtLayoutInternal.h"

// GPOS subtables (OT/Layout/GPOS): value records, anchors and device tables,
// pair adjustment, cursive and mark attachment, and the final joining of
// attachment offsets.

namespace ot::layout {

namespace {

// HarfBuzz's roundf(): floor(x + 0.5), so halves round up (-12.5 -> -12),
// unlike the C library's away-from-zero rounding.
int32_t hbRound(const float x) { return static_cast<int32_t>(floorf(x + 0.5f)); }

constexpr uint16_t VF_X_PLACEMENT = 0x0001, VF_Y_PLACEMENT = 0x0002, VF_X_ADVANCE = 0x0004, VF_Y_ADVANCE = 0x0008,
                   VF_X_PLA_DEVICE = 0x0010, VF_Y_PLA_DEVICE = 0x0020, VF_X_ADV_DEVICE = 0x0040, VF_DEVICES = 0x00F0;

unsigned valueLength(const uint16_t format) { return static_cast<unsigned>(__builtin_popcount(format)); }

// Device table correction in scaled units (hinting deltas only: variation
// indices contribute nothing without variation coordinates).
int32_t deviceDelta(const Table& device, const unsigned ppem, const int32_t scale) {
  if (!ppem) return 0;
  const uint16_t f = device.u16(4);
  if (f < 1 || f > 3) return 0;
  const uint16_t startSize = device.u16(0);
  const uint16_t endSize = device.u16(2);
  if (ppem < startSize || ppem > endSize) return 0;
  const unsigned s = ppem - startSize;
  const unsigned word = device.u16(6 + 2 * (s >> (4 - f)));
  const unsigned bits = word >> (16 - (((s & ((1u << (4 - f)) - 1)) + 1) << f));
  const unsigned mask = 0xFFFFu >> (16 - (1u << f));
  int delta = static_cast<int>(bits & mask);
  if (static_cast<unsigned>(delta) >= ((mask + 1) >> 1)) delta -= static_cast<int>(mask + 1);
  if (!delta) return 0;
  return static_cast<int32_t>(static_cast<int64_t>(delta) * scale / static_cast<int64_t>(ppem));
}

// ValueFormat::apply_value() for horizontal text. `base` is the table value
// record device offsets are relative to.
void applyValue(const ApplyContext* c, const uint16_t format, const Table& base, const Table& values, uint32_t o,
                GlyphPosition& pos) {
  const Scale& s = c->scale;
  if (!format) return;
  if (format & VF_X_PLACEMENT) {
    pos.xOffset = satAdd(pos.xOffset, s.emScaleX(values.s16(o)));
    o += 2;
  }
  if (format & VF_Y_PLACEMENT) {
    pos.yOffset = satAdd(pos.yOffset, s.emScaleY(values.s16(o)));
    o += 2;
  }
  if (format & VF_X_ADVANCE) {
    pos.xAdvance = satAdd(pos.xAdvance, s.emScaleX(values.s16(o)));
    o += 2;
  }
  if (format & VF_Y_ADVANCE) o += 2;
  if (!(format & VF_DEVICES)) return;
  const bool useX = s.xPpem != 0;
  const bool useY = s.yPpem != 0;
  if (!useX && !useY) return;
  if (format & VF_X_PLA_DEVICE) {
    if (useX) pos.xOffset = satAdd(pos.xOffset, deviceDelta(base.at(values.u16(o)), s.xPpem, s.xScale));
    o += 2;
  }
  if (format & VF_Y_PLA_DEVICE) {
    if (useY) pos.yOffset = satAdd(pos.yOffset, deviceDelta(base.at(values.u16(o)), s.yPpem, s.yScale));
    o += 2;
  }
  if (format & VF_X_ADV_DEVICE) {
    if (useX) pos.xAdvance = satAdd(pos.xAdvance, deviceDelta(base.at(values.u16(o)), s.xPpem, s.xScale));
    o += 2;
  }
}

void anchorPoint(const ApplyContext* c, const Table& anchor, float* x, float* y) {
  const Scale& s = c->scale;
  *x = *y = 0;
  switch (anchor.u16(0)) {
    case 1:
    case 2:  // contour points need hinting, which is not done: use the coordinates
      *x = s.emFscaleX(anchor.s16(2));
      *y = s.emFscaleY(anchor.s16(4));
      break;
    case 3:
      *x = s.emFscaleX(anchor.s16(2));
      *y = s.emFscaleY(anchor.s16(4));
      if (s.xPpem) *x += static_cast<float>(deviceDelta(anchor.offset16(6), s.xPpem, s.xScale));
      if (s.yPpem) *y += static_cast<float>(deviceDelta(anchor.offset16(8), s.yPpem, s.yScale));
      break;
    default:
      break;
  }
}

bool applySinglePos(ApplyContext* c, const Table& st) {
  Buffer& buffer = c->buffer;
  const int index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (index == NOT_COVERED) return false;
  const uint16_t format = st.u16(4);
  switch (st.u16(0)) {
    case 1:
      applyValue(c, format, st, st, 6, buffer.pos[buffer.idx]);
      break;
    case 2:
      if (static_cast<unsigned>(index) >= st.u16(6)) return false;
      applyValue(c, format, st, st, 8 + 2 * valueLength(format) * static_cast<unsigned>(index), buffer.pos[buffer.idx]);
      break;
    default:
      return false;
  }
  buffer.idx++;
  return true;
}

bool applyPairPos(ApplyContext* c, const Table& st) {
  Buffer& buffer = c->buffer;
  const int index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (index == NOT_COVERED) return false;
  const uint16_t format1 = st.u16(4);
  const uint16_t format2 = st.u16(6);
  const unsigned len1 = valueLength(format1);
  const unsigned len2 = valueLength(format2);
  SkippyIter& it = c->iterInput;
  it.resetFast(buffer.idx);
  if (!it.next()) return false;
  unsigned second = it.idx;

  switch (st.u16(0)) {
    case 1: {
      if (static_cast<unsigned>(index) >= st.u16(8)) return false;
      const Table set = st.offset16(10 + 2 * index);
      const unsigned recordSize = 2 * (1 + len1 + len2);
      const uint32_t glyph = buffer.info[second].codepoint;
      int lo = 0;
      int hi = static_cast<int>(set.u16(0)) - 1;
      while (lo <= hi) {
        const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
        const uint32_t record = 2 + recordSize * static_cast<unsigned>(mid);
        const uint16_t g = set.u16(record);
        if (glyph < g) {
          hi = mid - 1;
        } else if (glyph > g) {
          lo = mid + 1;
        } else {
          if (len1) applyValue(c, format1, set, set, record + 2, buffer.pos[buffer.idx]);
          if (len2) applyValue(c, format2, set, set, record + 2 + 2 * len1, buffer.pos[second]);
          if (len2) second++;
          buffer.idx = second;
          return true;
        }
      }
      return false;
    }
    case 2: {
      const unsigned klass1 = classOf(st.offset16(8), buffer.cur().codepoint);
      const unsigned klass2 = classOf(st.offset16(10), buffer.info[second].codepoint);
      const uint16_t class1Count = st.u16(12);
      const uint16_t class2Count = st.u16(14);
      if (klass1 >= class1Count || klass2 >= class2Count) return false;
      const uint32_t record = 16 + 2 * (len1 + len2) * (klass1 * class2Count + klass2);
      if (len1) applyValue(c, format1, st, st, record, buffer.pos[buffer.idx]);
      if (len2) applyValue(c, format2, st, st, record + 2 * len1, buffer.pos[second]);
      if (len2) second++;
      buffer.idx = second;
      return true;
    }
    default:
      return false;
  }
}

// reverse_cursive_minor_offset(): the old cursive chain of `child` now
// hangs off it, so reverse every link along that chain (stopping at
// `newParent`), innermost first.
void reverseCursiveMinorOffset(GlyphPosition* pos, const unsigned len, const unsigned child, const unsigned newParent) {
  uint16_t chainNodes[MAX_ATTACH_DEPTH + 2];
  unsigned n = 0;
  unsigned node = child;
  for (unsigned level = 0; level <= MAX_ATTACH_DEPTH; level++) {
    const int chain = pos[node].attachChain;
    if (!chain || !(pos[node].attachType & ATTACH_TYPE_CURSIVE)) break;
    pos[node].attachChain = 0;
    const unsigned parent = static_cast<unsigned>(static_cast<int>(node) + chain);
    if (parent >= len || parent == newParent) break;
    chainNodes[n++] = static_cast<uint16_t>(node);
    chainNodes[n] = static_cast<uint16_t>(parent);
    node = parent;
  }
  for (unsigned k = n; k-- > 0;) {
    const unsigned i = chainNodes[k];
    const unsigned j = chainNodes[k + 1];
    pos[j].yOffset = satSub(0, pos[i].yOffset);
    pos[j].attachChain = static_cast<int16_t>(static_cast<int>(i) - static_cast<int>(j));
    pos[j].attachType = pos[i].attachType;
  }
}

bool applyCursivePos(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const Table coverage = st.offset16(2);
  const uint16_t count = st.u16(4);
  auto record = [&](const uint32_t glyph, const int which) -> Table {
    const int index = coverageIndex(coverage, glyph);
    if (index == NOT_COVERED || static_cast<unsigned>(index) >= count) return Table();
    return st.offset16(6 + 4 * index + 2 * which);
  };
  const Table entry = record(buffer.cur().codepoint, 0);
  if (entry.empty()) return false;
  SkippyIter& it = c->iterInput;
  it.resetFast(buffer.idx);
  if (!it.prev()) return false;
  const Table exit = record(buffer.info[it.idx].codepoint, 1);
  if (exit.empty()) return false;

  const unsigned i = it.idx;
  const unsigned j = buffer.idx;
  float entryX, entryY, exitX, exitY;
  anchorPoint(c, exit, &exitX, &exitY);
  anchorPoint(c, entry, &entryX, &entryY);
  GlyphPosition* pos = buffer.pos.data();

  pos[i].xAdvance = satAdd(hbRound(exitX), pos[i].xOffset);
  const int32_t d = satAdd(hbRound(entryX), pos[j].xOffset);
  pos[j].xAdvance = satSub(pos[j].xAdvance, d);
  pos[j].xOffset = satSub(pos[j].xOffset, d);

  unsigned child = i;
  unsigned parent = j;
  int32_t yOffset = hbRound(entryY - exitY);
  if (!(c->lookupProps & lookupflag::RIGHT_TO_LEFT)) {
    const unsigned k = child;
    child = parent;
    parent = k;
    yOffset = -yOffset;
  }
  reverseCursiveMinorOffset(pos, buffer.len(), child, parent);
  const int chain = static_cast<int>(parent) - static_cast<int>(child);
  if (chain < INT16_MIN || chain > INT16_MAX) {
    buffer.idx++;
    return true;
  }
  pos[child].attachChain = static_cast<int16_t>(chain);
  pos[child].attachType = ATTACH_TYPE_CURSIVE;
  buffer.hasGposAttachment = true;
  pos[child].yOffset = yOffset;
  if (pos[parent].attachChain == -pos[child].attachChain) {
    pos[parent].attachChain = 0;
    pos[parent].yOffset = 0;
  }
  buffer.idx++;
  return true;
}

// resolve_cross_offset().
int32_t crossOffset(const GlyphPosition* pos, const unsigned len, unsigned glyphPos) {
  int32_t offset = pos[glyphPos].yOffset;
  for (unsigned steps = 0; steps < len && (pos[glyphPos].attachType & ATTACH_TYPE_CURSIVE); steps++) {
    const int chain = pos[glyphPos].attachChain;
    if (!chain) break;
    const unsigned parent = static_cast<unsigned>(static_cast<int>(glyphPos) + chain);
    if (parent >= len) break;
    glyphPos = parent;
    offset = satAdd(offset, pos[glyphPos].yOffset);
  }
  return offset;
}

// MarkArray::apply(): attaches the current mark to the glyph at `glyphPos`.
// `anchors` is the AnchorMatrix (base, ligature component or mark2 array).
bool applyMarkArray(ApplyContext* c, const Table& markArray, const unsigned markIndex, const unsigned row,
                    const Table& anchors, const unsigned classCount, const unsigned glyphPos) {
  Buffer& buffer = c->buffer;
  // A mark index past the array reads as a Null record: class 0, no anchor.
  const bool inRange = markIndex < markArray.u16(0);
  const uint32_t record = 2 + 4 * markIndex;
  const unsigned markClass = inRange ? markArray.u16(record) : 0;
  const Table markAnchor = inRange ? markArray.offset16(record + 2) : Table();
  const unsigned rows = anchors.u16(0);
  if (row >= rows || markClass >= classCount) return false;
  const uint16_t anchorOffset = anchors.u16(2 + 2 * (row * classCount + markClass));
  if (!anchorOffset) return false;
  const Table glyphAnchor = anchors.at(anchorOffset);

  float markX, markY, baseX, baseY;
  anchorPoint(c, markAnchor, &markX, &markY);
  anchorPoint(c, glyphAnchor, &baseX, &baseY);
  const int32_t baseOffset = crossOffset(buffer.pos.data(), buffer.len(), glyphPos);
  GlyphPosition& mark = buffer.pos[buffer.idx];
  const int chain = static_cast<int>(glyphPos) - static_cast<int>(buffer.idx);
  if (chain >= INT16_MIN && chain <= INT16_MAX) {
    mark.attachChain = static_cast<int16_t>(chain);
    mark.attachType = ATTACH_TYPE_MARK;
    mark.xOffset = hbRound(baseX - markX);
    mark.yOffset = hbRound(baseY - markY);
    mark.yOffset = satAdd(mark.yOffset, baseOffset);
    buffer.hasGposAttachment = true;
  } else {
    mark.attachChain = 0;
  }
  buffer.idx++;
  return true;
}

// MarkBasePosFormat1::accept().
bool acceptBase(const Buffer& buffer, const unsigned idx) {
  const GlyphInfo& g = buffer.info[idx];
  if (!g.multiplied() || g.ligComp() == 0 || idx == 0) return true;
  const GlyphInfo& p = buffer.info[idx - 1];
  return p.isMark() || !p.multiplied() || g.ligId() != p.ligId() || g.ligComp() != p.ligComp() + 1;
}

bool acceptLigature(const Buffer& buffer, const unsigned idx) {
  const GlyphInfo& g = buffer.info[idx];
  return !g.multiplied() || g.ligComp() == 0;
}

// The last non-mark glyph before the current one that `accept` takes, with
// the per-lookup memo HarfBuzz keeps (last_base / last_base_until).
int findBase(ApplyContext* c, const Table& baseCoverage, bool (*accept)(const Buffer&, unsigned)) {
  Buffer& buffer = c->buffer;
  SkippyIter& it = c->iterInput;
  it.lookupProps = lookupflag::IGNORE_MARKS;
  if (c->lastBaseUntil > buffer.idx) {
    c->lastBaseUntil = 0;
    c->lastBase = -1;
  }
  for (unsigned j = buffer.idx; j > c->lastBaseUntil; j--) {
    SkippyIter::Result m = it.match(buffer.info[j - 1]);
    if (m == SkippyIter::MATCH && !accept(buffer, j - 1) &&
        coverageIndex(baseCoverage, buffer.info[j - 1].codepoint) == NOT_COVERED) {
      m = SkippyIter::SKIP;
    }
    if (m == SkippyIter::MATCH) {
      c->lastBase = static_cast<int>(j) - 1;
      break;
    }
  }
  c->lastBaseUntil = buffer.idx;
  return c->lastBase;
}

bool applyMarkBasePos(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int markIndex = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (markIndex == NOT_COVERED) return false;
  const Table baseCoverage = st.offset16(4);
  const int base = findBase(c, baseCoverage, acceptBase);
  if (base < 0) return false;
  const int baseIndex = coverageIndex(baseCoverage, buffer.info[base].codepoint);
  if (baseIndex == NOT_COVERED) return false;
  return applyMarkArray(c, st.offset16(8), static_cast<unsigned>(markIndex), static_cast<unsigned>(baseIndex),
                        st.offset16(10), st.u16(6), static_cast<unsigned>(base));
}

bool applyMarkLigPos(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int markIndex = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (markIndex == NOT_COVERED) return false;
  const Table ligCoverage = st.offset16(4);
  const int base = findBase(c, ligCoverage, acceptLigature);
  if (base < 0) return false;
  const int ligIndex = coverageIndex(ligCoverage, buffer.info[base].codepoint);
  if (ligIndex == NOT_COVERED) return false;
  const Table ligArray = st.offset16(10);
  if (static_cast<unsigned>(ligIndex) >= ligArray.u16(0)) return false;
  const Table ligAttach = ligArray.offset16(2 + 2 * ligIndex);
  const unsigned compCount = ligAttach.u16(0);
  if (!compCount) return false;
  const unsigned ligId = buffer.info[base].ligId();
  const unsigned markId = buffer.cur().ligId();
  const unsigned markComp = buffer.cur().ligComp();
  unsigned compIndex;
  if (ligId && ligId == markId && markComp > 0) {
    compIndex = (compCount < markComp ? compCount : markComp) - 1;
  } else {
    compIndex = compCount - 1;
  }
  return applyMarkArray(c, st.offset16(8), static_cast<unsigned>(markIndex), compIndex, ligAttach, st.u16(6),
                        static_cast<unsigned>(base));
}

bool applyMarkMarkPos(ApplyContext* c, const Table& st) {
  if (st.u16(0) != 1) return false;
  Buffer& buffer = c->buffer;
  const int mark1Index = coverageIndex(st.offset16(2), buffer.cur().codepoint);
  if (mark1Index == NOT_COVERED) return false;
  SkippyIter& it = c->iterInput;
  it.resetFast(buffer.idx);
  it.lookupProps = c->lookupProps & ~lookupflag::IGNORE_FLAGS;
  if (!it.prev()) return false;
  const unsigned j = it.idx;
  if (!buffer.info[j].isMark()) return false;
  const unsigned id1 = buffer.cur().ligId();
  const unsigned id2 = buffer.info[j].ligId();
  const unsigned comp1 = buffer.cur().ligComp();
  const unsigned comp2 = buffer.info[j].ligComp();
  bool good;
  if (id1 == id2) {
    good = id1 == 0 || comp1 == comp2;
  } else {
    good = (id1 > 0 && !comp1) || (id2 > 0 && !comp2);
  }
  if (!good) return false;
  const int mark2Index = coverageIndex(st.offset16(4), buffer.info[j].codepoint);
  if (mark2Index == NOT_COVERED) return false;
  return applyMarkArray(c, st.offset16(8), static_cast<unsigned>(mark1Index), static_cast<unsigned>(mark2Index),
                        st.offset16(10), st.u16(6), j);
}

}  // namespace

bool applyGposSubtable(ApplyContext* c, const uint16_t type, const Table& st) {
  switch (type) {
    case gpos::SINGLE:
      return applySinglePos(c, st);
    case gpos::PAIR:
      return applyPairPos(c, st);
    case gpos::CURSIVE:
      return applyCursivePos(c, st);
    case gpos::MARK_BASE:
      return applyMarkBasePos(c, st);
    case gpos::MARK_LIG:
      return applyMarkLigPos(c, st);
    case gpos::MARK_MARK:
      return applyMarkMarkPos(c, st);
    case gpos::CONTEXT:
      return applyContext(c, st);
    case gpos::CHAIN:
      return applyChainContext(c, st);
    default:
      return false;
  }
}

}  // namespace ot::layout

namespace ot {

// propagate_attachment_offsets(): each attached glyph takes on the offset of
// the glyph it hangs off (resolved first, up to MAX_ATTACH_DEPTH links), and
// marks cancel the advances between them and their base.
void propagateAttachmentOffsets(Buffer& buffer) {
  using layout::satAdd;
  using layout::satSub;
  if (!buffer.hasGposAttachment) return;
  GlyphPosition* pos = buffer.pos.data();
  const unsigned len = buffer.len();
  uint16_t chainNodes[layout::MAX_ATTACH_DEPTH + 2];
  for (unsigned start = 0; start < len; start++) {
    if (!pos[start].attachChain) continue;
    unsigned n = 0;
    unsigned node = start;
    for (unsigned level = layout::MAX_ATTACH_DEPTH;; level--) {
      const int chain = pos[node].attachChain;
      pos[node].attachChain = 0;
      const unsigned parent = static_cast<unsigned>(static_cast<int>(node) + chain);
      if (parent >= len || level == 0) break;
      chainNodes[n++] = static_cast<uint16_t>(node);
      chainNodes[n] = static_cast<uint16_t>(parent);
      if (!pos[parent].attachChain) break;
      node = parent;
    }
    for (unsigned k = n; k-- > 0;) {
      const unsigned i = chainNodes[k];
      const unsigned j = chainNodes[k + 1];
      if (pos[i].attachType & ATTACH_TYPE_CURSIVE) {
        pos[i].yOffset = satAdd(pos[i].yOffset, pos[j].yOffset);
        continue;
      }
      pos[i].xOffset = satAdd(pos[i].xOffset, pos[j].xOffset);
      if (j < i) {
        for (unsigned m = j; m < i; m++) {
          pos[i].xOffset = satSub(pos[i].xOffset, pos[m].xAdvance);
          pos[i].yOffset = satSub(pos[i].yOffset, pos[m].yAdvance);
        }
      } else {
        for (unsigned m = i; m < j; m++) {
          pos[i].xOffset = satAdd(pos[i].xOffset, pos[m].xAdvance);
          pos[i].yOffset = satAdd(pos[i].yOffset, pos[m].yAdvance);
        }
      }
    }
  }
}

}  // namespace ot
