#include "StreamDecoder.h"

#include <InflateReader.h>
#include <Logging.h>

#include <cstring>

namespace {

struct FileInflateReadCtx {
  FsFile* file = nullptr;
  uint32_t cur = 0;
  uint32_t left = 0;
  uint8_t buf[256];
  uint32_t blen = 0;
  uint32_t bpos = 0;
};

FileInflateReadCtx* g_fileInflateCtx = nullptr;

// Streaming input for uzlib: no malloc of full compressed stream (saves RAM on embedded).
static int fileInflateReadCb(uzlib_uncomp* u) {
  (void)u;
  FileInflateReadCtx* c = g_fileInflateCtx;
  if (!c) return -1;
  if (c->bpos < c->blen) {
    return c->buf[c->bpos++];
  }
  if (c->left == 0) return -1;
  const uint32_t n = c->left < sizeof(c->buf) ? c->left : static_cast<uint32_t>(sizeof(c->buf));
  if (!c->file->seek(c->cur)) return -1;
  const int r = c->file->read(c->buf, n);
  if (r <= 0) return -1;
  c->cur += static_cast<uint32_t>(r);
  c->left -= static_cast<uint32_t>(r);
  c->blen = static_cast<uint32_t>(r);
  c->bpos = 1;
  return c->buf[0];
}

}  // namespace

// RFC 1950 zlib wrapper: deflate method (CMF&0xF)==8 and (CMF<<8|FLG)%31==0; optional preset dict (FLG&0x20).
static size_t zlibWrapperSkipLen(const uint8_t* compressed, size_t compressedLen) {
  if (compressedLen < 2) return 0;
  const uint8_t cmf = compressed[0];
  const uint8_t flg = compressed[1];
  if ((cmf & 0x0F) != 8) return 0;
  const int header16 = (cmf << 8) | flg;
  if (header16 % 31 != 0) return 0;
  size_t skip = 2;
  if (flg & 0x20) {
    if (compressedLen < 6) return 0;
    skip += 4;
  }
  return skip;
}

size_t StreamDecoder::flateDecode(FsFile& file, uint32_t streamOffset, uint32_t compressedLen, uint8_t* outBuf,
                                  size_t maxOutBytes) {
  if (!outBuf || maxOutBytes == 0 || compressedLen == 0) return 0;

  FileInflateReadCtx ctx{};
  ctx.file = &file;

  if (compressedLen >= 2) {
    if (!file.seek(streamOffset)) return 0;
    uint8_t h[6]{};
    if (file.read(h, 2) != 2) return 0;
    size_t have = 2;
    if (compressedLen >= 6 && (h[1] & 0x20) != 0) {
      if (file.read(h + 2, 4) != 4) return 0;
      have = 6;
    }
    const size_t zlibSkip = zlibWrapperSkipLen(h, have);
    if (zlibSkip >= 2) {
      ctx.cur = streamOffset + static_cast<uint32_t>(zlibSkip);
      ctx.left = compressedLen - static_cast<uint32_t>(zlibSkip);
    } else if (have == 6) {
      std::memcpy(ctx.buf, h, 6);
      ctx.blen = 6;
      ctx.bpos = 0;
      ctx.cur = streamOffset + 6;
      ctx.left = compressedLen - 6;
    } else {
      ctx.buf[0] = h[0];
      ctx.buf[1] = h[1];
      ctx.blen = 2;
      ctx.bpos = 0;
      ctx.cur = streamOffset + 2;
      ctx.left = compressedLen - 2;
    }
  } else if (compressedLen == 1) {
    if (!file.seek(streamOffset)) return 0;
    if (file.read(ctx.buf, 1) != 1) return 0;
    ctx.blen = 1;
    ctx.bpos = 0;
    ctx.cur = streamOffset + 1;
    ctx.left = 0;
  }

  g_fileInflateCtx = &ctx;

  InflateReader ir;
  if (!ir.init(true)) {
    g_fileInflateCtx = nullptr;
    return 0;
  }
  uzlib_uncomp* d = ir.raw();
  d->source_read_cb = fileInflateReadCb;
  d->source = nullptr;
  d->source_limit = nullptr;

  size_t total = 0;
  while (total < maxOutBytes) {
    size_t produced = 0;
    const InflateStatus st = ir.readAtMost(outBuf + total, maxOutBytes - total, &produced);
    total += produced;
    if (st == InflateStatus::Error) {
      break;
    }
    if (st == InflateStatus::Done) {
      break;
    }
  }

  g_fileInflateCtx = nullptr;
  return total;
}

size_t StreamDecoder::flateDecodeBytes(const uint8_t* compressed, size_t compressedLen, uint8_t* outBuf,
                                       size_t maxOutBytes) {
  if (!compressed || compressedLen == 0 || !outBuf || maxOutBytes == 0) return 0;

  InflateReader ir;
  if (!ir.init(true)) {
    return 0;
  }
  const size_t zlibSkip = zlibWrapperSkipLen(compressed, compressedLen);
  if (zlibSkip > compressedLen) {
    return 0;
  }
  ir.setSource(compressed + zlibSkip, compressedLen - zlibSkip);

  size_t total = 0;
  while (total < maxOutBytes) {
    size_t produced = 0;
    const InflateStatus st = ir.readAtMost(outBuf + total, maxOutBytes - total, &produced);
    total += produced;
    if (st == InflateStatus::Error) {
      break;
    }
    if (st == InflateStatus::Done) {
      break;
    }
  }
  return total;
}

bool StreamDecoder::flateDecodeChunks(FsFile& file, uint32_t streamOffset, uint32_t compressedLen,
                                      ChunkConsumer consumer, void* ctx) {
  if (!consumer || compressedLen == 0) return false;

  FileInflateReadCtx readCtx{};
  readCtx.file = &file;

  if (compressedLen >= 2) {
    if (!file.seek(streamOffset)) return false;
    uint8_t h[6]{};
    if (file.read(h, 2) != 2) return false;
    size_t have = 2;
    if (compressedLen >= 6 && (h[1] & 0x20) != 0) {
      if (file.read(h + 2, 4) != 4) return false;
      have = 6;
    }
    const size_t zlibSkip = zlibWrapperSkipLen(h, have);
    if (zlibSkip >= 2) {
      readCtx.cur = streamOffset + static_cast<uint32_t>(zlibSkip);
      readCtx.left = compressedLen - static_cast<uint32_t>(zlibSkip);
    } else if (have == 6) {
      std::memcpy(readCtx.buf, h, 6);
      readCtx.blen = 6;
      readCtx.bpos = 0;
      readCtx.cur = streamOffset + 6;
      readCtx.left = compressedLen - 6;
    } else {
      readCtx.buf[0] = h[0];
      readCtx.buf[1] = h[1];
      readCtx.blen = 2;
      readCtx.bpos = 0;
      readCtx.cur = streamOffset + 2;
      readCtx.left = compressedLen - 2;
    }
  } else if (compressedLen == 1) {
    if (!file.seek(streamOffset)) return false;
    if (file.read(readCtx.buf, 1) != 1) return false;
    readCtx.blen = 1;
    readCtx.bpos = 0;
    readCtx.cur = streamOffset + 1;
    readCtx.left = 0;
  }

  g_fileInflateCtx = &readCtx;

  InflateReader ir;
  if (!ir.init(true)) {
    g_fileInflateCtx = nullptr;
    return false;
  }
  uzlib_uncomp* d = ir.raw();
  d->source_read_cb = fileInflateReadCb;
  d->source = nullptr;
  d->source_limit = nullptr;

  uint8_t out[256];
  while (true) {
    size_t produced = 0;
    const InflateStatus st = ir.readAtMost(out, sizeof(out), &produced);
    if (produced > 0 && !consumer(ctx, out, produced)) {
      LOG_ERR("PDF", "StreamDecoder: chunk consumer rejected %zu bytes", produced);
      g_fileInflateCtx = nullptr;
      return false;
    }
    if (st == InflateStatus::Done) {
      break;
    }
    if (st == InflateStatus::Error) {
      LOG_ERR("PDF", "StreamDecoder: inflate error while streaming");
      g_fileInflateCtx = nullptr;
      return false;
    }
  }

  g_fileInflateCtx = nullptr;
  return true;
}

bool StreamDecoder::flateDecodeBytesChunks(const uint8_t* compressed, size_t compressedLen, ChunkConsumer consumer,
                                           void* ctx) {
  if (!compressed || compressedLen == 0 || !consumer) return false;

  InflateReader ir;
  if (!ir.init(true)) {
    return false;
  }
  const size_t zlibSkip = zlibWrapperSkipLen(compressed, compressedLen);
  if (zlibSkip > compressedLen) {
    return false;
  }
  ir.setSource(compressed + zlibSkip, compressedLen - zlibSkip);

  uint8_t out[256];
  while (true) {
    size_t produced = 0;
    const InflateStatus st = ir.readAtMost(out, sizeof(out), &produced);
    if (produced > 0 && !consumer(ctx, out, produced)) {
      LOG_ERR("PDF", "StreamDecoder: chunk consumer rejected %zu bytes", produced);
      return false;
    }
    if (st == InflateStatus::Done) {
      break;
    }
    if (st == InflateStatus::Error) {
      LOG_ERR("PDF", "StreamDecoder: inflate error while streaming");
      return false;
    }
  }
  return true;
}
