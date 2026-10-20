#pragma once
class BuildArena;  // lib/Memory

#include <BuildArena.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>

#include <memory>

#include "../Activity.h"
#include "Epub/ThumbResult.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class CoverThumbSession;  // Epub/CoverThumbSession.h
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  // `scratch`: lent to Epub::load for its inflate rings (first-open indexing borrows the
  // secondary framebuffer for it); null loads from the heap as before. `progress`/`progressCtx`:
  // told how far a cache build has got (Epub::setBuildProgress).
  static std::unique_ptr<Epub> loadEpub(const std::string& path, BuildArena* scratch = nullptr,
                                        void (*progress)(void* ctx, uint8_t percent) = nullptr,
                                        void* progressCtx = nullptr);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isMdFile(const std::string& path);
  static bool isImageFile(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToMdReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  static std::string sidecarCoverPath(const std::string& bookPath);
  static std::string bookCacheDir(const std::string& bookPath);

  // Grid cover thumbnails. The on-disk result is source-agnostic and identical no matter
  // where the cover came from: "<bookCacheDir>/thumb_<W>x<H>.bmp" (or "thumb_<H>.bmp" for the
  // single-height themes), registered in RecentBooks via the template coverThumbPlaceholder()
  // returns. A sidecar image beside the book always takes precedence over the embedded cover
  // as the *source*; the embedded cover is only parsed when no sidecar exists.
  static std::string coverThumbPlaceholder(const std::string& bookPath);
  // Produce (or reuse) the cover thumbnail BMP for a book. Returns ThumbResult so the caller can
  // tell a structural absence (no cover / unsupported — safe to record permanently) from a
  // transient failure (retry next pass/boot). A sidecar image beside the book, when present and
  // convertible, always yields Ok and clears any stale sentinel first.
  // `scratch` (here and on the session starters below): a lent region -- HomeActivity lends
  // the secondary framebuffer for the whole cover pass -- for the OPF inflate ring, the cover
  // extraction ring and the decoders' working memory. Null uses the heap as before.
  // crop=false fits the whole cover inside width x height (the cover grids' thumbnail) instead of
  // filling the box and cropping the overflow (the carousel's).
  static ThumbResult ensureCoverThumb(const std::string& bookPath, int width, int height, BuildArena* scratch = nullptr,
                                      bool crop = true);
  // Every (width, height) in `sizes` for this book, from ONE decode of an embedded EPUB JPEG cover
  // (the Lyra carousel's two thumbnail sizes used to cost two full decodes). A sidecar image, an XTC
  // or a TXT source keeps the per-size path. Ok only when every size is complete.
  // `sliced` (Home's cover pass): when the cover is a JPEG the sliced converter takes, the
  // conversion is STARTED instead of run here and handed over in *sliced, with nothing written yet;
  // the result is then TransientFail and the caller drives the session (see CoverThumbSession).
  // Otherwise *sliced stays null and the result is the one-shot conversion's.
  static ThumbResult ensureCoverThumbs(const std::string& bookPath, const std::pair<int, int>* sizes, int count,
                                       BuildArena* scratch = nullptr,
                                       std::unique_ptr<CoverThumbSession>* sliced = nullptr);
  static ThumbResult ensureCoverThumb(const std::string& bookPath, int height, BuildArena* scratch = nullptr);
  // True only if a cover thumbnail BMP exists AND holds all its declared pixel rows. A thumbnail
  // whose write was interrupted (reboot/abort mid-decode) is left truncated on the SD card; it
  // passes a naive size>0 check but fails to draw partway (GFX "Failed to read row N"). Treating
  // such a file as invalid lets the caller regenerate it instead of drawing/keeping it forever.
  // When expectedWidth/Height are given (> 0), a BMP LARGER than that in either dimension is
  // also invalid: older builds' crop mode kept the overfill dimension, and drawing such a thumb
  // 1:1-sized slots forces a rescale of the dithered image (visible grid). Regeneration with the
  // current converters yields an exact-size thumb.
  static bool isCoverThumbComplete(const std::string& path, int expectedWidth = 0, int expectedHeight = 0);
  // Write a minimal but VALID 1-bit BMP (white box with a black frame) at the thumbnail path, used
  // to mark a book that has no extractable cover. Being a complete BMP it passes
  // isCoverThumbComplete(), so the cover loops treat the book as resolved and never re-open the EPUB
  // to rediscover the absence — without the 0-byte-file "mess" (we now treat empty files as invalid).
  static bool writeCoverPlaceholderBmp(const std::string& path);

  // Sliced extraction of a ZIP entry to a file, one chunk per continueStep() call.
  // Used to extract an embedded PNG cover (cover.img) without blocking loop() for
  // the full ~35-second decompress. Owns the ZipFile (which EntryReader references).
  class CoverExtractSession {
   public:
    enum class Status { Running, Done, Error };

    // Begin extracting zipEntryPath from epubPath into a staging file. The completed
    // file is atomically renamed to destPath so decoders never observe partial bytes.
    // Returns false if the entry cannot be opened.
    bool begin(const std::string& epubPath, const std::string& zipEntryPath, const std::string& destPath,
               BuildArena* scratch = nullptr);

    // Decompress up to chunkBytes into destPath. Call repeatedly until not Running.
    Status continueStep(size_t chunkBytes = 4096);

    size_t bytesProduced() const;
    size_t totalBytes() const;

    CoverExtractSession() = default;
    ~CoverExtractSession();
    // Owns a malloc'd buf_ and an open destination file, so copying one would
    // double-free and double-close. Only ever held by unique_ptr; make that a
    // compile error rather than relying on nobody trying.
    CoverExtractSession(const CoverExtractSession&) = delete;
    CoverExtractSession& operator=(const CoverExtractSession&) = delete;

   private:
    void releaseChunk();

    std::unique_ptr<ZipFile> zip_;
    std::unique_ptr<ZipFile::EntryReader> reader_;
    FsFile dst_;
    std::string finalPath_;
    std::string destPath_;
    BuildArena* scratch_ = nullptr;
    BuildArena::Block chunkBlock_;  // reserved after the reader's block, released before it
    uint8_t* buf_ = nullptr;
    bool bufInArena_ = false;
    size_t requestedBytes_ = 0;  // what the caller asked for
    size_t chunkBytes_ = 0;      // what buf_ holds: the request, or less when memory was short
  };

  // Begin a sliced ZIP extraction for the embedded cover of bookPath.
  // Returns nullptr if the book has no extractable embedded PNG cover, or if
  // cover.img is already cached. On success the caller drives the session via
  // continueStep() each loop() tick until Done, then calls beginPngThumbSession.
  static std::unique_ptr<CoverExtractSession> beginCoverExtractSession(const std::string& bookPath,
                                                                       BuildArena* scratch = nullptr);

  // Open FsFiles that must outlive a PngDecodeSession (session borrows pointers to them).
  struct PngThumbFiles {
    FsFile src;  // source PNG (sidecar or cover.img)
    FsFile dst;  // destination BMP (opened for write)
    void close() {
      if (src.isOpen()) src.close();
      if (dst.isOpen()) dst.close();
    }
  };

  // Start a sliced PNG decode for bookPath at the given thumb dimensions. Writes the
  // "thumb_<W>x<H>.bmp" (multi-size) form. Returns nullptr if the cover is not a PNG, is already
  // cached, or setup fails. On success, *filesOut owns the open FsFiles; caller must keep them
  // alive until the session completes and then close them.
  // On failure (nullptr return), the thumb file is left as a 0-byte sentinel.
  static std::unique_ptr<PngDecodeSession> beginPngThumbSession(const std::string& bookPath, int width, int height,
                                                                PngThumbFiles& filesOut, BuildArena* scratch = nullptr,
                                                                bool crop = true);

  // Single-height variant: writes the "thumb_<H>.bmp" form used by the non-carousel themes, at
  // width = H*0.6 (matching the synchronous single-height decode). Same contract as above.
  static std::unique_ptr<PngDecodeSession> beginPngThumbSession(const std::string& bookPath, int height,
                                                                PngThumbFiles& filesOut, BuildArena* scratch = nullptr);

  // Render a sidecar image (or copy a sidecar BMP) into a scaled 1-bit BMP at
  // "<cacheDir>/<fileName>". Returns the written path, or "" on failure.
  static std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width,
                                         int height, const std::string& fileName, bool crop = true);

  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath)
      : Activity("Reader", renderer, mappedInput), initialBookPath(std::move(initialBookPath)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
