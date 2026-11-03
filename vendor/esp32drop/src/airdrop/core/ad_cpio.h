/* ad_cpio.h -- walk the cpio archive an AirDrop sender delivers, one entry at a time.
 *
 * WHY THIS IS AN ITERATOR AND NOT A FINDER. The function this replaces was called
 * cpio_find_image(): it walked the archive, returned the FIRST entry whose first bytes
 * looked like JPEG or PNG, and discarded everything else. For a badge that draws a photo
 * that was the right shape. For a library it is the wrong one -- a PDF, a text file, a
 * contact card, or the second of two photos all vanished silently, and the transfer was
 * reported as "no displayable image" rather than as delivered. The library's job is to
 * hand over what arrived; deciding what is interesting belongs to whoever called it.
 *
 * WHAT A REAL TRANSFER LOOKS LIKE, measured across all thirteen archives in the capture
 * corpus -- every one of them had EXACTLY TWO entries:
 *     nsz=2  fsz=0       name=.                  a directory
 *     nsz=16 fsz=265615  name=./kuro_icon.png    the file
 * So an iterator that yields every entry hands the caller a zero-byte "." on every single
 * transfer. That is why mode is parsed and exposed: the caller filters on
 * ad_cpio_is_regular(), and it is one line rather than a guess about names or sizes.
 *
 * Dependency-free C. The same code compiles into the firmware and into tools/test-cpio.sh,
 * which is the point: this is a parser fed by a remote device, so every bound it checks is
 * a bound someone else chooses.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* What the payload looks like, sniffed from its first bytes. A CONVENIENCE, never a
 * filter: an entry whose type is AD_FILE_OTHER is still delivered, with its name. */
enum {
  AD_FILE_OTHER = 0,
  AD_FILE_JPEG,
  AD_FILE_PNG,
  AD_FILE_GIF,
  AD_FILE_HEIC, /* what an iPhone sends by default unless told otherwise */
  AD_FILE_PDF,
  AD_FILE_ZIP,         /* also .docx/.xlsx/.key and every other zip container    */
  AD_FILE_APPLEDOUBLE, /* macOS metadata sidecar, not a file the user sent    */
};

struct AdCpioEntry {
  const char* name;  /* NOT NUL-terminated-safe to trust: use name_len            */
  uint32_t name_len; /* excludes the trailing NUL the archive stores              */
  const uint8_t* data;
  uint32_t len;
  uint32_t mode; /* POSIX st_mode as the archive recorded it                  */
};

struct AdCpioIter {
  const uint8_t* base;
  uint32_t len, pos;
  bool truncated; /* set when the walk stopped early on a malformed header     */
};

#define AD_CPIO_S_IFMT 0170000u
#define AD_CPIO_S_IFREG 0100000u
#define AD_CPIO_S_IFDIR 0040000u

static bool ad_cpio_is_regular(uint32_t mode) {
  /* A mode of 0 means the archive did not say. Treat that as a regular file rather than
     dropping the payload: the cost of a wrong guess in this direction is one extra
     callback, and in the other direction it is a file that silently disappears. */
  return mode == 0 || (mode & AD_CPIO_S_IFMT) == AD_CPIO_S_IFREG;
}

/* n ASCII octal digits. Returns false on any non-digit, because a header that is not
   octal is not a header and continuing past it walks into the payload. */
static bool ad_cpio_oct(const uint8_t* p, int n, uint32_t* out) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '7') return false;
    v = (v << 3) | (uint32_t)(p[i] - '0');
  }
  *out = v;
  return true;
}

static bool ad_cpio_hex(const uint8_t* p, int n, uint32_t* out) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    uint8_t c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else
      return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

static void ad_cpio_begin(struct AdCpioIter* it, const uint8_t* buf, uint32_t len) {
  it->base = buf;
  it->len = len;
  it->pos = 0;
  it->truncated = false;
}

/* The next entry, or false when the archive ends. Ending is either the TRAILER!!! record
 * (clean) or a header that does not parse (it->truncated is then set, so a caller can
 * tell "that was all of it" from "that was as much as survived").
 *
 * Every arithmetic step is bounds-checked against len BEFORE it is used, and each check is
 * written so that an overflowing size fails it rather than wrapping: the sizes come from a
 * remote device. */
static bool ad_cpio_next(struct AdCpioIter* it, struct AdCpioEntry* out) {
  const uint8_t* b = it->base;
  uint32_t len = it->len, pos = it->pos;

  if (pos + 6 > len) {
    it->truncated = (pos != len);
    return false;
  }

  uint32_t hdr, namesize, filesize, mode;
  bool align4;
  if (memcmp(b + pos, "070707", 6) == 0) { /* odc: what Apple senders emit */
    hdr = 76;
    align4 = false;
    if (pos + hdr > len) {
      it->truncated = true;
      return false;
    }
    if (!ad_cpio_oct(b + pos + 18, 6, &mode) || !ad_cpio_oct(b + pos + 59, 6, &namesize) ||
        !ad_cpio_oct(b + pos + 65, 11, &filesize)) {
      it->truncated = true;
      return false;
    }
  } else if (memcmp(b + pos, "070701", 6) == 0 || memcmp(b + pos, "070702", 6) == 0) {
    hdr = 110;
    align4 = true;
    if (pos + hdr > len) {
      it->truncated = true;
      return false;
    }
    if (!ad_cpio_hex(b + pos + 14, 8, &mode) || !ad_cpio_hex(b + pos + 54, 8, &filesize) ||
        !ad_cpio_hex(b + pos + 94, 8, &namesize)) {
      it->truncated = true;
      return false;
    }
  } else {
    it->truncated = true;
    return false;
  }

  if (namesize == 0 || namesize > len - pos - hdr) {
    it->truncated = true;
    return false;
  }
  const char* nm = (const char*)(b + pos + hdr);

  /* TRAILER!!! ends the archive and is not an entry. */
  if (namesize >= 11 && memcmp(nm, "TRAILER!!!", 10) == 0) {
    it->pos = len;
    return false;
  }

  uint32_t data_off = pos + hdr + namesize;
  if (align4) {
    if (data_off > len - ((4 - (data_off & 3)) & 3)) {
      it->truncated = true;
      return false;
    }
    data_off = (data_off + 3) & ~3u;
  }
  if (data_off > len || filesize > len - data_off) {
    it->truncated = true;
    return false;
  }

  uint32_t next = data_off + filesize;
  if (align4) {
    if (next > len - ((4 - (next & 3)) & 3))
      next = len; /* last entry, unpadded tail */
    else
      next = (next + 3) & ~3u;
  }
  if (next <= pos) {
    it->truncated = true;
    return false;
  } /* no forward progress */

  out->name = nm;
  out->name_len = namesize ? namesize - 1 : 0; /* the stored size includes the NUL */
  out->data = b + data_off;
  out->len = filesize;
  out->mode = mode;
  it->pos = next;
  return true;
}

/* "./dir/photo.png" -> "photo.png". Returns a pointer INTO name; nothing is copied. */
static const char* ad_cpio_basename(const char* name, uint32_t name_len, uint32_t* out_len) {
  uint32_t start = 0;
  for (uint32_t i = 0; i < name_len; i++)
    if (name[i] == '/') start = i + 1;
  if (out_len) *out_len = name_len - start;
  return name + start;
}

/* Type from the leading bytes. Never used to decide whether to deliver something. */
static int ad_cpio_sniff(const uint8_t* d, uint32_t n) {
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return AD_FILE_JPEG;
  if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return AD_FILE_PNG;
  if (n >= 6 && memcmp(d, "GIF8", 4) == 0) return AD_FILE_GIF;
  if (n >= 4 && memcmp(d, "%PDF", 4) == 0) return AD_FILE_PDF;
  if (n >= 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) return AD_FILE_ZIP;
  if (n >= 4 && d[0] == 0x00 && d[1] == 0x05 && d[2] == 0x16 && d[3] == 0x07) return AD_FILE_APPLEDOUBLE;
  /* ISO-BMFF: 4-byte box length, then "ftyp", then a brand. heic/heix/mif1/msf1 are what
     an iPhone produces for photos when HEIF is on, which is the factory default. */
  if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
    if (memcmp(d + 8, "heic", 4) == 0 || memcmp(d + 8, "heix", 4) == 0 || memcmp(d + 8, "mif1", 4) == 0 ||
        memcmp(d + 8, "msf1", 4) == 0)
      return AD_FILE_HEIC;
  }
  return AD_FILE_OTHER;
}

/* An AppleDouble sidecar: macOS metadata about the file of the same name, written next to
 * it as "._<name>". A Finder-sourced AirDrop carries ONE PER FILE -- measured, three files
 * arrived as six entries -- so a caller that does not know about them sees every transfer
 * as half noise, and a badge that draws the last thing it is handed draws the sidecar's
 * "cannot display" message over the photo.
 *
 * Detected by MAGIC (00 05 16 07, the AppleDouble header) rather than by the "._" name,
 * because the name is a convention and the magic is the format. The two agree in every
 * observation; if they ever disagree, believe the bytes.
 *
 * It is still DELIVERED. A library does not get to decide that some of what arrived was
 * not worth mentioning -- it gets to label it, so the caller can skip it in one line. */
static bool ad_file_is_sidecar(const struct AdCpioEntry* e) {
  return e->len >= 4 && e->data[0] == 0x00 && e->data[1] == 0x05 && e->data[2] == 0x16 && e->data[3] == 0x07;
}

/* The name convention, exposed separately so a caller can cross-check it against the
   magic, or catch a sidecar whose payload was truncated below four bytes. */
static bool ad_cpio_name_is_sidecar(const char* name, uint32_t name_len) {
  uint32_t bl;
  const char* b = ad_cpio_basename(name, name_len, &bl);
  return bl >= 2 && b[0] == '.' && b[1] == '_';
}

static const char* ad_cpio_type_name(int t) {
  switch (t) {
    case AD_FILE_JPEG:
      return "jpeg";
    case AD_FILE_PNG:
      return "png";
    case AD_FILE_GIF:
      return "gif";
    case AD_FILE_HEIC:
      return "heic";
    case AD_FILE_PDF:
      return "pdf";
    case AD_FILE_ZIP:
      return "zip";
    case AD_FILE_APPLEDOUBLE:
      return "appledouble";
    default:
      return "other";
  }
}
