#include "ImagePackedWriter.h"

#if CROSSPOINT_ACCELERATED_IMAGES
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "ImagePackedBand.h"

namespace imagePacked {
Writer::Writer(const glyphBitmap::Target& target, int width, int first, int end, int top, int bottom,
               glyphBitmap::Plane plane, bool absolute, bool whiteAsTransparent, bool enabled)
    : target(target),
      width(width),
      first(first),
      end(end),
      top(top),
      bottom(bottom),
      plane(plane),
      absolute(absolute),
      whiteAsTransparent(whiteAsTransparent),
      stride(0) {
  if (!enabled || !target.buffer || width <= 0 || width > 2048 || target.frame.dxX != 0 ||
      (target.frame.dxY != 1 && target.frame.dxY != -1) || (target.frame.dyX != 1 && target.frame.dyX != -1) ||
      target.frame.dyY != 0)
    return;
  stride = (width + 3) / 4;
  this->first = std::max(0, first);
  this->end = std::min(width, end);
  glyphBitmap::clipAxis(target.frame.y, target.frame.dxY, target.originY, target.originY + target.rows, this->first,
                        this->end);
  if (this->first >= this->end || top >= bottom) return;
  const size_t bytes = static_cast<size_t>(stride) * 8;
  storage = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!storage) LOG_ERR("GFX", "Packed image rows OOM (%u bytes); using pixel output", static_cast<unsigned>(bytes));
}

Writer::~Writer() { flush(); }

bool Writer::writeRow(const uint8_t* row, int screenY) {
  if (!storage) return false;
  if (screenY < top || screenY >= bottom) return true;
  const int physicalX = target.frame.x + screenY * target.frame.dyX;
  if (physicalX < 0 || physicalX >= target.width) return true;
  const int nextByte = physicalX / 8;
  if (valid && nextByte != byte) flush();
  byte = nextByte;
  const int lane = physicalX & 7;
  memcpy(storage.get() + lane * stride, row, stride);
  valid |= 0x80u >> lane;
  if (valid == 0xff) flush();
  return true;
}

void Writer::flush() {
  if (!valid) return;
  const int localY = target.frame.y + first * target.frame.dxY - target.originY;
  drawStridedBand(storage.get(), stride, valid, {width, first, end},
                  {target.buffer + static_cast<size_t>(localY) * target.stride + byte, target.stride * target.frame.dxY,
                   plane, absolute},
                  whiteAsTransparent ? WhitePixels::KeepBackground : WhitePixels::Draw);
  valid = 0;
}
}  // namespace imagePacked
#endif
