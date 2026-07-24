#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <optional>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "Epub/SourceIdentityStore.h"
#include "EpubReaderActivity.h"
#include "PerBookReaderSettingsBridge.h"
#include "PerBookReaderSettingsStore.h"
#include "ReadingStatsCompletionTransaction.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path, PerBookReaderSettings& globalSettings,
                                               PerBookReaderSettings& bookSettings, bool& settingsWritable) {
  if (!recoverInterruptedBookFileReplacement(path)) {
    LOG_ERR("READER", "Could not recover interrupted EPUB replacement: %s", path.c_str());
    return nullptr;
  }
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }

  globalSettings = captureReaderSettings();
  bookSettings = globalSettings;
  // An interrupted cache clear may have already moved the settings/statistics
  // files into a sibling staging directory. Recover them before any loader can
  // create replacement state; if recovery is ambiguous, fail closed.
  if (!recoverBookCacheUserState(epub->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged per-book state: %s", epub->getCachePath().c_str());
    return nullptr;
  }

  Epub::SourceBindingStatus bindingStatus = epub->inspectSourceBinding();
  if (bindingStatus == Epub::SourceBindingStatus::Mismatch) {
    const std::string staleCachePath = epub->getCachePath();
    // Quarantine all old path-keyed state before the new EPUB can inherit it.
    // A partial cleanup is a hard failure: the old identity remains the proof
    // needed to retry safely on a later open.
    if (!resetBookUserStateAfterReplacement(path) || Storage.exists(staleCachePath.c_str())) {
      LOG_ERR("READER", "Could not quarantine stale state for replaced EPUB: %s", path.c_str());
      return nullptr;
    }
    epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) return nullptr;
    bindingStatus = Epub::SourceBindingStatus::Missing;
  } else if (bindingStatus == Epub::SourceBindingStatus::NewerVersion ||
             bindingStatus == Epub::SourceBindingStatus::Invalid ||
             bindingStatus == Epub::SourceBindingStatus::IoError) {
    LOG_ERR("READER", "EPUB source identity cannot be handled safely (status %u)",
            static_cast<unsigned>(bindingStatus));
    return nullptr;
  }

  if (bindingStatus == Epub::SourceBindingStatus::Missing) {
    // One-time migration for books whose cache/user state predates source
    // identities (including a cache cleared on older firmware). A replacement
    // before this first adoption is fundamentally unknowable.
    LOG_DBG("READER", "Adopting source identity for legacy EPUB state: %s", path.c_str());
  }
  if (!epub->bindCurrentSource() || epub->inspectSourceBinding() != Epub::SourceBindingStatus::Match) {
    LOG_ERR("READER", "Could not persist EPUB source identity: %s", path.c_str());
    return nullptr;
  }

  // CrossInk's legacy file remains authoritative and immutable. Migration is
  // best-effort after source identity is proven; any failure only falls back
  // to CrossVi/global settings and must never prevent the book from opening.
  const auto migrationStatus = PerBookReaderSettingsStore::migrateCrossInk(epub->getCachePath(), globalSettings);
  switch (migrationStatus) {
    case PerBookReaderSettingsStore::MigrationStatus::MIGRATED:
      LOG_DBG("READER", "Migrated CrossInk reader settings: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::NEWER_CROSSINK_VERSION:
      LOG_ERR("READER", "Preserving unsupported newer CrossInk reader settings: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::INVALID_LEGACY_FILE:
      LOG_ERR("READER", "Preserving invalid CrossInk reader settings without migration: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::BACKUP_CONFLICT:
      LOG_ERR("READER", "CrossInk reader settings backup conflicts; preserving both files: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::IO_ERROR:
    case PerBookReaderSettingsStore::MigrationStatus::SAVE_FAILED:
      LOG_ERR("READER", "Could not safely migrate CrossInk reader settings (status %u): %s",
              static_cast<unsigned>(migrationStatus), path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::INVALID_DEFAULTS:
      LOG_ERR("READER", "Current reader defaults are invalid; CrossInk settings were preserved: %s", path.c_str());
      break;
    case PerBookReaderSettingsStore::MigrationStatus::NO_LEGACY_FILE:
    case PerBookReaderSettingsStore::MigrationStatus::CROSSVI_FILE_PRESENT:
      break;
  }

  const BookMetadataCache::LoadStatus cacheStatus = epub->inspectCache();
  if (cacheStatus == BookMetadataCache::LoadStatus::NewerVersion ||
      cacheStatus == BookMetadataCache::LoadStatus::IoError) {
    LOG_ERR("READER", "EPUB cache cannot be handled safely (status %u)", static_cast<unsigned>(cacheStatus));
    return nullptr;
  }

  const auto settingsStatus = PerBookReaderSettingsStore::load(epub->getCachePath(), bookSettings);
  settingsWritable = settingsStatus != PerBookReaderSettingsStore::LoadStatus::NEWER_VERSION &&
                     settingsStatus != PerBookReaderSettingsStore::LoadStatus::IO_ERROR;
  if (settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED ||
      settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP ||
      settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP) {
    applyEffectiveBookReaderSettings(globalSettings, bookSettings);
  } else {
    bookSettings = globalSettings;
    applyEffectiveBookReaderSettings(globalSettings, bookSettings);
  }
  // A per-book SD font must be active before layout starts. Invalid book-only
  // choices are cleared in memory without leaking the override to settings.json.
  sdFontSystem.ensureLoaded(renderer, false);
  // Missing, legacy, or malformed derived metadata is rebuilt below. Show the
  // indexing popup for all of those cases, not only a physically missing file.
  const bool uncached = cacheStatus != BookMetadataCache::LoadStatus::Loaded;
  if (uncached) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.epubSafeMode != 0 || SETTINGS.embeddedStyle == 0);
  }
  if (loaded) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  applyReaderSettings(globalSettings);
  sdFontSystem.ensureLoaded(renderer);
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!recoverInterruptedBookFileReplacement(path)) {
    LOG_ERR("READER", "Could not recover interrupted XTC replacement: %s", path.c_str());
    return nullptr;
  }
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (!recoverBookCacheUserState(xtc->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged XTC state: %s", xtc->getCachePath().c_str());
    return nullptr;
  }
  if (!xtc->load()) {
    LOG_ERR("READER", "Failed to load XTC");
    return nullptr;
  }

  ZipFile::SourceIdentity currentIdentity;
  if (!xtc->getSourceIdentity(currentIdentity)) {
    LOG_ERR("READER", "Could not identify XTC source: %s", path.c_str());
    return nullptr;
  }

  ZipFile::SourceIdentity storedIdentity;
  const SourceIdentityStore::LoadStatus identityStatus =
      SourceIdentityStore::load(xtc->getCachePath(), storedIdentity);
  switch (identityStatus) {
    case SourceIdentityStore::LoadStatus::Primary:
    case SourceIdentityStore::LoadStatus::Backup:
    case SourceIdentityStore::LoadStatus::Temp:
      if (storedIdentity != currentIdentity) {
        const std::string staleCachePath = xtc->getCachePath();
        xtc.reset();
        if (!resetBookUserStateAfterReplacement(path) || Storage.exists(staleCachePath.c_str())) {
          LOG_ERR("READER", "Could not quarantine stale XTC state: %s", path.c_str());
          return nullptr;
        }
        xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
        if (!xtc || !xtc->load() || !xtc->getSourceIdentity(currentIdentity)) {
          LOG_ERR("READER", "Could not reload replacement XTC: %s", path.c_str());
          return nullptr;
        }
      }
      break;
    case SourceIdentityStore::LoadStatus::Missing:
      LOG_DBG("READER", "Adopting source identity for legacy XTC state: %s", path.c_str());
      break;
    case SourceIdentityStore::LoadStatus::NewerVersion:
    case SourceIdentityStore::LoadStatus::Invalid:
    case SourceIdentityStore::LoadStatus::IoError:
      LOG_ERR("READER", "XTC source identity cannot be handled safely (status %u)",
              static_cast<unsigned>(identityStatus));
      return nullptr;
  }

  const SourceIdentityStore::SaveStatus saved = SourceIdentityStore::save(xtc->getCachePath(), currentIdentity);
  if (saved != SourceIdentityStore::SaveStatus::Saved && saved != SourceIdentityStore::SaveStatus::Unchanged) {
    LOG_ERR("READER", "Could not persist XTC source identity: %s", path.c_str());
    return nullptr;
  }
  ZipFile::SourceIdentity verifiedIdentity;
  if (SourceIdentityStore::load(xtc->getCachePath(), verifiedIdentity) != SourceIdentityStore::LoadStatus::Primary ||
      verifiedIdentity != currentIdentity) {
    LOG_ERR("READER", "Could not verify XTC source identity: %s", path.c_str());
    return nullptr;
  }
  return xtc;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path, PerBookReaderSettings& globalSettings,
                                             PerBookReaderSettings& bookSettings, bool& settingsWritable) {
  if (!recoverInterruptedBookFileReplacement(path)) {
    LOG_ERR("READER", "Could not recover interrupted text replacement: %s", path.c_str());
    return nullptr;
  }
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  globalSettings = captureReaderSettings();
  bookSettings = globalSettings;
  if (!recoverBookCacheUserState(txt->getCachePath(), path)) {
    LOG_ERR("READER", "Could not recover staged TXT state: %s", txt->getCachePath().c_str());
    return nullptr;
  }
  if (!txt->load()) {
    LOG_ERR("READER", "Failed to load TXT");
    return nullptr;
  }

  ZipFile::SourceIdentity currentIdentity;
  if (!txt->getSourceIdentity(currentIdentity)) {
    LOG_ERR("READER", "Could not identify TXT source: %s", path.c_str());
    return nullptr;
  }

  ZipFile::SourceIdentity storedIdentity;
  SourceIdentityStore::LoadStatus identityStatus = SourceIdentityStore::load(txt->getCachePath(), storedIdentity);
  switch (identityStatus) {
    case SourceIdentityStore::LoadStatus::Primary:
    case SourceIdentityStore::LoadStatus::Backup:
    case SourceIdentityStore::LoadStatus::Temp:
      if (storedIdentity != currentIdentity) {
        const std::string staleCachePath = txt->getCachePath();
        txt.reset();
        // The path now contains different bytes. Quarantine every path-keyed
        // state file before the replacement can inherit progress/statistics.
        if (!resetBookUserStateAfterReplacement(path) || Storage.exists(staleCachePath.c_str())) {
          LOG_ERR("READER", "Could not quarantine stale TXT state: %s", path.c_str());
          return nullptr;
        }
        txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
        if (!txt || !txt->load() || !txt->getSourceIdentity(currentIdentity)) {
          LOG_ERR("READER", "Could not reload replacement TXT: %s", path.c_str());
          return nullptr;
        }
      }
      break;
    case SourceIdentityStore::LoadStatus::Missing:
      // One-time adoption for caches created before TXT source bindings. A
      // replacement made before this first upgraded open is unknowable.
      LOG_DBG("READER", "Adopting source identity for legacy TXT state: %s", path.c_str());
      break;
    case SourceIdentityStore::LoadStatus::NewerVersion:
    case SourceIdentityStore::LoadStatus::Invalid:
    case SourceIdentityStore::LoadStatus::IoError:
      LOG_ERR("READER", "TXT source identity cannot be handled safely (status %u)",
              static_cast<unsigned>(identityStatus));
      return nullptr;
  }

  const SourceIdentityStore::SaveStatus saved = SourceIdentityStore::save(txt->getCachePath(), currentIdentity);
  if (saved != SourceIdentityStore::SaveStatus::Saved && saved != SourceIdentityStore::SaveStatus::Unchanged) {
    LOG_ERR("READER", "Could not persist TXT source identity: %s", path.c_str());
    return nullptr;
  }
  ZipFile::SourceIdentity verifiedIdentity;
  if (SourceIdentityStore::load(txt->getCachePath(), verifiedIdentity) != SourceIdentityStore::LoadStatus::Primary ||
      verifiedIdentity != currentIdentity) {
    LOG_ERR("READER", "Could not verify TXT source identity: %s", path.c_str());
    return nullptr;
  }

  const PerBookReaderSettingsStore::LoadStatus settingsStatus =
      PerBookReaderSettingsStore::load(txt->getCachePath(), bookSettings);
  settingsWritable = settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED ||
                     settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP ||
                     settingsStatus == PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP ||
                     settingsStatus == PerBookReaderSettingsStore::LoadStatus::MISSING;
  if (settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED &&
      settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP &&
      settingsStatus != PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP) {
    bookSettings = globalSettings;
  }
  applyReaderSettings(bookSettings.hasReaderOverrides ? bookSettings : globalSettings);
  sdFontSystem.ensureLoaded(renderer, false);
  return txt;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub, PerBookReaderSettings globalSettings,
                                      PerBookReaderSettings bookSettings, const bool settingsWritable) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(
      std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub), std::move(globalSettings),
                                           std::move(bookSettings), settingsWritable, std::move(initialClippingJump)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt, PerBookReaderSettings globalSettings,
                                     PerBookReaderSettings bookSettings, const bool settingsWritable) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(
      renderer, mappedInput, std::move(txt), std::move(globalSettings), std::move(bookSettings), settingsWritable,
      std::move(initialClippingJump)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Direct resume can enter Reader before Home gets a chance to finish a
  // power-interrupted completion transaction. Recover first so the book-file
  // move/delete guard does not reject an otherwise healthy book open.
  if (ReadingStatsCompletionTransaction::recoverPending() ==
      ReadingStatsCompletionTransaction::RecoveryResult::Blocked) {
    LOG_ERR("READER", "Pending reading-statistics transaction remains blocked");
  }

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  if (initialClippingJump) {
    const bool epubTarget = initialClippingJump->bookType == "epub" && FsHelpers::hasEpubExtension(initialBookPath);
    const bool textTarget = initialClippingJump->bookType == "txt" && isTxtFile(initialBookPath);
    if (initialClippingJump->bookPath != initialBookPath || (!epubTarget && !textTarget)) {
    // The overload is only a transport. ReaderActivity remains the dispatch
    // boundary and never forwards a clipping target into a different reader
    // type.
    LOG_ERR("READER", "Rejected clipping jump for mismatched reader dispatch: %s", initialBookPath.c_str());
      initialClippingJump.reset();
    }
  }

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    sdFontSystem.ensureLoaded(renderer);
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    sdFontSystem.ensureLoaded(renderer);
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    sdFontSystem.ensureLoaded(renderer);
    PerBookReaderSettings globalSettings;
    PerBookReaderSettings bookSettings;
    bool settingsWritable = true;
    auto txt = loadTxt(initialBookPath, globalSettings, bookSettings, settingsWritable);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt), std::move(globalSettings), std::move(bookSettings), settingsWritable);
  } else {
    PerBookReaderSettings globalSettings;
    PerBookReaderSettings bookSettings;
    bool settingsWritable = true;
    auto epub = loadEpub(initialBookPath, globalSettings, bookSettings, settingsWritable);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub), std::move(globalSettings), std::move(bookSettings), settingsWritable);
  }
}

void ReaderActivity::onGoBack() { finish(); }
