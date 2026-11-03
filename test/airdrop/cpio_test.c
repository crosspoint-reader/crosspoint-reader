/* Host tests for ad_cpio.h -- the SAME code the firmware runs.
 *
 * This parser is fed by a remote device, so every bound it checks is a bound somebody
 * else chooses. Most of these cases are therefore malformed on purpose: a length that
 * overruns the buffer, a size field that is not octal, a header that claims to be shorter
 * than it is, an entry that does not advance. A parser that walks off the end of a PSRAM
 * block here does it inside a TLS task holding a 265 KB transfer.
 *
 * The first fixture is not synthetic. It is the exact shape every one of the thirteen
 * archives in the capture corpus had:
 *     nsz=2  fsz=0       name=.                 (a directory)
 *     nsz=16 fsz=265615  name=./kuro_icon.png   (the file)
 * That shape is the reason mode is parsed at all -- an iterator that does not look at it
 * hands the caller a zero-byte "." on every single transfer.
 *
 *   cc -O2 -o /tmp/cpio_test tools/cpio_test.c && /tmp/cpio_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../vendor/esp32drop/src/airdrop/core/ad_cpio.h"

static int fails = 0, ran = 0;
static void check(const char* name, int ok, const char* detail) {
  ran++;
  printf("  [%s] %-64s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
  if (!ok) fails++;
}

/* ---- builders: emit real archive bytes, so the tests read what a sender writes ---- */

static uint8_t BUF[8192];
static uint32_t N;

static void put(const void* p, uint32_t n) {
  memcpy(BUF + N, p, n);
  N += n;
}
static void oct(uint32_t v, int n) {
  char t[16];
  snprintf(t, sizeof t, "%0*o", n, v);
  put(t, (uint32_t)n);
}
static void hex8(uint32_t v) {
  char t[16];
  snprintf(t, sizeof t, "%08x", v);
  put(t, 8);
}

/* odc (070707): fixed 76-byte header, no padding anywhere. What Apple senders emit. */
static void odc(const char* name, uint32_t mode, const void* data, uint32_t dlen) {
  put("070707", 6);
  oct(0, 6);
  oct(0, 6); /* dev, ino   */
  oct(mode, 6);
  oct(0, 6);
  oct(0, 6);
  oct(1, 6);
  oct(0, 6);  /* uid gid nlink rdev */
  oct(0, 11); /* mtime      */
  oct((uint32_t)strlen(name) + 1, 6);
  oct(dlen, 11);
  put(name, (uint32_t)strlen(name) + 1);
  if (dlen) put(data, dlen);
}

/* newc (070701): 110-byte header, name and data each padded to a 4-byte boundary. */
static void newc(const char* name, uint32_t mode, const void* data, uint32_t dlen) {
  uint32_t start = N;
  put("070701", 6);
  hex8(0);
  hex8(mode);
  hex8(0);
  hex8(0);
  hex8(1);
  hex8(0); /* ino mode uid gid nlink mtime */
  hex8(dlen);
  hex8(0);
  hex8(0);
  hex8(0);
  hex8(0); /* dev/rdev major/minor */
  hex8((uint32_t)strlen(name) + 1);
  hex8(0); /* check */
  put(name, (uint32_t)strlen(name) + 1);
  while ((N - start) & 3) BUF[N++] = 0;
  if (dlen) put(data, dlen);
  while ((N - start) & 3) BUF[N++] = 0;
}

static void trailer_odc(void) { odc("TRAILER!!!", 0, NULL, 0); }
static void trailer_newc(void) { newc("TRAILER!!!", 0, NULL, 0); }

static const uint8_t PNG[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
static const uint8_t JPG[4] = {0xFF, 0xD8, 0xFF, 0xE0};

/* ---- helpers ---- */

struct Seen {
  char name[64];
  uint32_t len;
  uint32_t mode;
  int type;
};

static int walk(struct Seen* out, int cap, bool* truncated) {
  struct AdCpioIter it;
  struct AdCpioEntry e;
  int n = 0;
  ad_cpio_begin(&it, BUF, N);
  while (n < cap && ad_cpio_next(&it, &e)) {
    uint32_t bl;
    const char* b = ad_cpio_basename(e.name, e.name_len, &bl);
    if (bl > sizeof(out[n].name) - 1) bl = sizeof(out[n].name) - 1;
    memcpy(out[n].name, b, bl);
    out[n].name[bl] = 0;
    out[n].len = e.len;
    out[n].mode = e.mode;
    out[n].type = ad_cpio_sniff(e.data, e.len);
    n++;
  }
  if (truncated) *truncated = it.truncated;
  return n;
}

int main(void) {
  struct Seen s[8];
  bool tr;
  int n;
  char d[160];

  puts("ad_cpio.h -- archive iteration");

  /* 1. THE MEASURED SHAPE. Every real transfer in the corpus is exactly this. */
  N = 0;
  odc(".", AD_CPIO_S_IFDIR | 0755, NULL, 0);
  {
    uint8_t img[64];
    memcpy(img, PNG, 8);
    memset(img + 8, 0xAB, 56);
    odc("./kuro_icon.png", AD_CPIO_S_IFREG | 0644, img, 64);
  }
  trailer_odc();
  n = walk(s, 8, &tr);
  snprintf(d, sizeof d, "n=%d tr=%d [0]=%s/%u [1]=%s/%u", n, tr, s[0].name, s[0].len, n > 1 ? s[1].name : "-",
           n > 1 ? s[1].len : 0);
  check("real shape: both entries yielded, TRAILER ends cleanly", n == 2 && !tr, d);
  check("real shape: the directory is not a regular file", n == 2 && !ad_cpio_is_regular(s[0].mode), NULL);
  check("real shape: the payload IS a regular file", n == 2 && ad_cpio_is_regular(s[1].mode), NULL);
  check("real shape: basename strips './'", n == 2 && strcmp(s[1].name, "kuro_icon.png") == 0, s[1].name);
  check("real shape: sniffed as PNG", n == 2 && s[1].type == AD_FILE_PNG, ad_cpio_type_name(s[1].type));

  /* 2. THE DEFECT THIS REPLACES: more than one file must all come out. The function this
   *    supersedes returned the FIRST image and dropped the rest, silently. */
  N = 0;
  odc(".", AD_CPIO_S_IFDIR | 0755, NULL, 0);
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  odc("./b.jpg", AD_CPIO_S_IFREG | 0644, JPG, 4);
  odc("./c.txt", AD_CPIO_S_IFREG | 0644, "hello", 5);
  trailer_odc();
  n = walk(s, 8, &tr);
  snprintf(d, sizeof d, "n=%d %s,%s,%s,%s", n, s[0].name, s[1].name, s[2].name, s[3].name);
  check("four entries in, four entries out (the first-image-only defect)", n == 4 && !tr, d);
  check("a non-image is delivered, not dropped",
        n == 4 && strcmp(s[3].name, "c.txt") == 0 && s[3].type == AD_FILE_OTHER && s[3].len == 5, NULL);

  /* 3. newc, where the padding lives. */
  N = 0;
  newc(".", AD_CPIO_S_IFDIR | 0755, NULL, 0);
  newc("./odd-name-length.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  newc("./x", AD_CPIO_S_IFREG | 0644, JPG, 4);
  trailer_newc();
  n = walk(s, 8, &tr);
  snprintf(d, sizeof d, "n=%d tr=%d [1]=%s/%u [2]=%s/%u", n, tr, s[1].name, s[1].len, s[2].name, s[2].len);
  check("newc: name and data padding walked correctly", n == 3 && !tr && s[1].len == 8 && s[2].len == 4, d);

  /* 4. A TRAILER is the end, and the end is not an error. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  trailer_odc();
  {
    uint32_t save = N;
    N = save;
  }
  n = walk(s, 8, &tr);
  check("TRAILER ends the walk without setting truncated", n == 1 && !tr, NULL);

  /* 5. NO trailer at all: the walk ends when the buffer does, and says so. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  n = walk(s, 8, &tr);
  check("no TRAILER: the one entry is still delivered", n == 1, NULL);

  /* ---- malformed input. The sender is remote; none of this may read past N. ---- */

  /* 6. filesize larger than the buffer holds. */
  N = 0;
  odc(".", AD_CPIO_S_IFDIR | 0755, NULL, 0);
  odc("./big.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  { /* rewrite that entry's filesize to 0xFFFFFF */
    uint8_t* h = BUF + 76;
    char t[16];
    snprintf(t, sizeof t, "%011o", 0xFFFFFFu);
    memcpy(h + 65, t, 11);
  }
  n = walk(s, 8, &tr);
  snprintf(d, sizeof d, "n=%d tr=%d", n, tr);
  check("filesize past the end of the buffer is refused", n == 1 && tr, d);

  /* 7. namesize larger than the buffer holds. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  {
    char t[16];
    snprintf(t, sizeof t, "%06o", 0xFFFFu);
    memcpy(BUF + 59, t, 6);
  }
  n = walk(s, 8, &tr);
  check("namesize past the end of the buffer is refused", n == 0 && tr, NULL);

  /* 8. a size field that is not octal at all. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  memcpy(BUF + 65, "ZZZZZZZZZZZ", 11);
  n = walk(s, 8, &tr);
  check("a non-octal size field is refused, not silently read as zero", n == 0 && tr, NULL);

  /* 9. a header cut off mid-way. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  N = 40;
  n = walk(s, 8, &tr);
  check("a truncated header yields nothing and sets truncated", n == 0 && tr, NULL);

  /* 10. data cut off mid-way. */
  N = 0;
  odc("./a.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
  N -= 4;
  n = walk(s, 8, &tr);
  check("a truncated payload is refused rather than half-delivered", n == 0 && tr, NULL);

  /* 11. garbage where a header should be. */
  N = 0;
  memset(BUF, 'x', 200);
  N = 200;
  n = walk(s, 8, &tr);
  check("garbage is not a header", n == 0 && tr, NULL);

  /* 12. an empty buffer. */
  N = 0;
  n = walk(s, 8, &tr);
  check("an empty archive yields nothing and is not truncated", n == 0 && !tr, NULL);

  /* 13. an empty REGULAR file is still an entry -- zero bytes is a fact, not an error. */
  N = 0;
  odc("./empty.txt", AD_CPIO_S_IFREG | 0644, NULL, 0);
  trailer_odc();
  n = walk(s, 8, &tr);
  check("a zero-byte regular file is delivered", n == 1 && s[0].len == 0 && ad_cpio_is_regular(s[0].mode), NULL);

  /* ---- mode ---- */
  puts("ad_cpio.h -- mode");
  check("mode 0 counts as regular (deliver rather than drop)", ad_cpio_is_regular(0), NULL);
  check("a directory is not regular", !ad_cpio_is_regular(AD_CPIO_S_IFDIR | 0755), NULL);
  check("a regular file is regular", ad_cpio_is_regular(AD_CPIO_S_IFREG | 0644), NULL);
  check("a symlink is not regular", !ad_cpio_is_regular(0120000u | 0777), NULL);

  /* ---- basename ---- */
  puts("ad_cpio.h -- basename");
  {
    uint32_t bl;
    const char* b;
    b = ad_cpio_basename("./d/photo.png", 13, &bl);
    check("nested path", bl == 9 && memcmp(b, "photo.png", 9) == 0, NULL);
    b = ad_cpio_basename("photo.png", 9, &bl);
    check("no slash at all", bl == 9 && memcmp(b, "photo.png", 9) == 0, NULL);
    b = ad_cpio_basename("dir/", 4, &bl);
    check("trailing slash yields an empty basename, not a read past the end", bl == 0, NULL);
    (void)b;
  }

  /* ---- sniff. A convenience, never a filter. ---- */
  puts("ad_cpio.h -- type sniffing");
  {
    const uint8_t heic[16] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0};
    const uint8_t mif1[16] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', 'i', 'f', '1', 0, 0, 0, 0};
    const uint8_t gif[6] = {'G', 'I', 'F', '8', '9', 'a'};
    const uint8_t pdf[5] = {'%', 'P', 'D', 'F', '-'};
    const uint8_t zip[4] = {'P', 'K', 3, 4};
    check("JPEG", ad_cpio_sniff(JPG, 4) == AD_FILE_JPEG, NULL);
    check("PNG", ad_cpio_sniff(PNG, 8) == AD_FILE_PNG, NULL);
    check("GIF", ad_cpio_sniff(gif, 6) == AD_FILE_GIF, NULL);
    check("PDF", ad_cpio_sniff(pdf, 5) == AD_FILE_PDF, NULL);
    check("ZIP (also docx/keynote)", ad_cpio_sniff(zip, 4) == AD_FILE_ZIP, NULL);
    check("HEIC -- what an iPhone sends by default", ad_cpio_sniff(heic, 16) == AD_FILE_HEIC, NULL);
    check("HEIF mif1 brand", ad_cpio_sniff(mif1, 16) == AD_FILE_HEIC, NULL);
    check("unknown bytes are OTHER, not an error", ad_cpio_sniff((const uint8_t*)"hello", 5) == AD_FILE_OTHER, NULL);
    check("a 2-byte payload does not read past it", ad_cpio_sniff(PNG, 2) == AD_FILE_OTHER, NULL);
    check("a zero-length payload is OTHER", ad_cpio_sniff(PNG, 0) == AD_FILE_OTHER, NULL);
    const uint8_t adbl[8] = {0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00};
    check("AppleDouble sidecar, by magic", ad_cpio_sniff(adbl, 8) == AD_FILE_APPLEDOUBLE, NULL);
  }

  /* ---- the shape a Finder-sourced transfer actually has: one sidecar per file ---- */
  puts("ad_cpio.h -- AppleDouble sidecars (measured: 3 files arrive as 6 entries)");
  {
    const uint8_t adbl[8] = {0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00};
    N = 0;
    odc("./NSIRD_Finder_a/photo.png", AD_CPIO_S_IFREG | 0644, PNG, 8);
    odc("./NSIRD_Finder_a/._photo.png", AD_CPIO_S_IFREG | 0644, adbl, 8);
    odc("./NSIRD_Finder_b/QX.svg", AD_CPIO_S_IFREG | 0644, "<svg/>", 6);
    odc("./NSIRD_Finder_b/._QX.svg", AD_CPIO_S_IFREG | 0644, adbl, 8);
    trailer_odc();
    n = walk(s, 8, &tr);
    snprintf(d, sizeof d, "n=%d %s/%s %s/%s", n, s[0].name, ad_cpio_type_name(s[0].type), s[1].name,
             ad_cpio_type_name(s[1].type));
    check("all four entries delivered -- sidecars are labelled, not hidden", n == 4 && !tr, d);
    check("the photo is a PNG", n == 4 && s[0].type == AD_FILE_PNG, NULL);
    check("its sidecar is APPLEDOUBLE", n == 4 && s[1].type == AD_FILE_APPLEDOUBLE, NULL);
    check("the SVG is OTHER, not a sidecar", n == 4 && s[2].type == AD_FILE_OTHER, NULL);
    check("the SVG's sidecar is APPLEDOUBLE", n == 4 && s[3].type == AD_FILE_APPLEDOUBLE, NULL);
    check("basename survives the NSIRD_Finder_* directory",
          n == 4 && strcmp(s[0].name, "photo.png") == 0 && strcmp(s[2].name, "QX.svg") == 0, NULL);
  }

  puts("ad_cpio.h -- the name convention, as a cross-check on the magic");
  check("._name is a sidecar by name", ad_cpio_name_is_sidecar("./d/._x.png", 11), NULL);
  check("name.png is not", !ad_cpio_name_is_sidecar("./d/x.png", 9), NULL);
  check("a leading dot alone is not", !ad_cpio_name_is_sidecar(".", 1), NULL);
  check("a bare '.' directory is not", !ad_cpio_name_is_sidecar("./.", 3), NULL);

  printf("\n%d checks, %d failed\n", ran, fails);
  return fails ? 1 : 0;
}
