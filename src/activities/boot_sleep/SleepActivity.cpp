#include "SleepActivity.h"

#include <ArduinoJson.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FeatureFlags.h"
#include "SleepExtensionHooks.h"
#include "SpiBusMutex.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "network/BackgroundWifiService.h"
#include "util/PokemonBookDataStore.h"

namespace {

// Supported image extensions for sleep images
#if ENABLE_IMAGE_SLEEP
const char* SLEEP_IMAGE_EXTENSIONS[] = {".bmp", ".png", ".jpg", ".jpeg"};
#else
const char* SLEEP_IMAGE_EXTENSIONS[] = {".bmp"};  // BMP only when PNG/JPEG disabled
#endif
constexpr int NUM_SLEEP_IMAGE_EXTENSIONS = sizeof(SLEEP_IMAGE_EXTENSIONS) / sizeof(SLEEP_IMAGE_EXTENSIONS[0]);

bool isSupportedSleepImage(const std::string& filename) {
  if (filename.length() < 4) return false;
  std::string lowerFilename = filename;
  std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (int i = 0; i < NUM_SLEEP_IMAGE_EXTENSIONS; i++) {
    size_t extLen = strlen(SLEEP_IMAGE_EXTENSIONS[i]);
    if (lowerFilename.length() >= extLen &&
        lowerFilename.substr(lowerFilename.length() - extLen) == SLEEP_IMAGE_EXTENSIONS[i]) {
      return true;
    }
  }
  return false;
}

bool isBmpFile(const std::string& filename) {
  if (filename.length() < 4) return false;
  std::string lowerFilename = filename;
  std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowerFilename.substr(lowerFilename.length() - 4) == ".bmp";
}

namespace SleepCacheMutex {
StaticSemaphore_t mutexBuffer;
SemaphoreHandle_t get() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&mutexBuffer);
  return mutex;
}
void lock() {
  auto mutex = get();
  if (mutex) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  }
}
void unlock() {
  auto mutex = get();
  if (mutex) {
    xSemaphoreGiveRecursive(mutex);
  }
}
struct Guard {
  Guard() { lock(); }
  ~Guard() { unlock(); }
};
}  // namespace SleepCacheMutex

struct SleepImageCache {
  bool scanned = false;
  uint8_t sourceMode = 0xFF;
  std::vector<std::string> validFiles;
};

SleepImageCache sleepImageCache;

static constexpr char SLEEP_CACHE_FILE[] = "/.crosspoint/sleep_cache.bin";
static constexpr uint8_t SLEEP_CACHE_VERSION = 1;

static bool loadSleepImageCacheFromFile(SleepImageCache& cache) {
  SpiBusMutex::Guard guard;
  HalFile f;
  if (!Storage.openFileForRead("SLP", SLEEP_CACHE_FILE, f)) return false;

  uint8_t version, sourceMode;
  uint16_t count;
  if (f.read(&version, 1) != 1 || version != SLEEP_CACHE_VERSION) {
    f.close();
    return false;
  }
  if (f.read(&sourceMode, 1) != 1) {
    f.close();
    return false;
  }
  if (f.read(&count, 2) != 2) {
    f.close();
    return false;
  }

  cache.validFiles.clear();
  cache.validFiles.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    uint16_t len;
    if (f.read(&len, 2) != 2 || len > 500) {
      f.close();
      return false;
    }
    std::string path(len, '\0');
    if (static_cast<uint16_t>(f.read(path.data(), len)) != len) {
      f.close();
      return false;
    }
    cache.validFiles.push_back(std::move(path));
  }
  f.close();
  cache.sourceMode = sourceMode;
  cache.scanned = true;
  return true;
}

static void saveSleepImageCacheToFile(const SleepImageCache& cache) {
  SpiBusMutex::Guard guard;
  Storage.mkdir("/.crosspoint");
  HalFile f;
  if (!Storage.openFileForWrite("SLP", SLEEP_CACHE_FILE, f)) return;
  const uint8_t version = SLEEP_CACHE_VERSION;
  f.write(&version, 1);
  f.write(&cache.sourceMode, 1);
  const uint16_t count = static_cast<uint16_t>(std::min(cache.validFiles.size(), size_t(0xFFFF)));
  f.write(&count, 2);
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t len = static_cast<uint16_t>(cache.validFiles[i].size());
    f.write(&len, 2);
    f.write(cache.validFiles[i].data(), len);
  }
  f.close();
}

bool tryRenderExternalSleepApp(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  const bool rendered = SleepExtensionHooks::renderExternalSleepScreen(renderer, mappedInput);
  if (rendered) {
    LOG_DBG("SLP", "External sleep app rendered screen");
  }
  return rendered;
}

const char* getSleepSourceName(const uint8_t sourceMode) {
  switch (sourceMode) {
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_POKEDEX:
      return "pokedex";
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_ALL:
      return "all";
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_SLEEP:
    default:
      return "sleep";
  }
}

std::string getSleepSourcePath(const uint8_t sourceMode) {
  switch (sourceMode) {
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_POKEDEX:
      return "/sleep/pokedex";
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_ALL:
      return "/sleep";
    case CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_SLEEP:
    default: {
      auto hiddenDir = Storage.open("/.sleep");
      if (hiddenDir && hiddenDir.isDirectory()) {
        hiddenDir.close();
        return "/.sleep";
      }
      if (hiddenDir) {
        hiddenDir.close();
      }
      return "/sleep";
    }
  }
}

bool shouldScanRecursively(const uint8_t sourceMode) {
  return sourceMode == CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_ALL;
}

std::string getEntryName(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos) {
    return path;
  }
  return path.substr(lastSlash + 1);
}

std::string joinPath(const std::string& directoryPath, const std::string& entryName) {
  if (entryName.empty()) return directoryPath;
  if (entryName[0] == '/') return entryName;
  if (directoryPath.empty() || directoryPath == "/") return "/" + entryName;
  if (directoryPath.back() == '/') return directoryPath + entryName;
  return directoryPath + "/" + entryName;
}

void scanSleepImagesInDirectory(const std::string& directoryPath, const bool recursive,
                                std::vector<std::string>& filesOut) {
  auto dir = Storage.open(directoryPath.c_str());
  if (!(dir && dir.isDirectory())) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string entryName(name);
    const std::string leafName = getEntryName(entryName);
    if (leafName.empty() || leafName[0] == '.') {
      file.close();
      continue;
    }

    const std::string fullPath = joinPath(directoryPath, entryName);
    if (file.isDirectory()) {
      file.close();
      if (recursive) {
        scanSleepImagesInDirectory(fullPath, true, filesOut);
      }
      continue;
    }

    if (!isSupportedSleepImage(leafName)) {
      file.close();
      continue;
    }

    if (isBmpFile(leafName)) {
      Bitmap bitmap(file, true);
      const auto err = bitmap.parseHeaders();
      if (err == BmpReaderError::Ok) {
        filesOut.emplace_back(fullPath);
      } else {
        LOG_ERR("SLP", "Invalid BMP in %s: %s (%s)", directoryPath.c_str(), leafName.c_str(),
                Bitmap::errorToString(err));
      }
      file.close();
      continue;
    }

    const ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(fullPath);
    if (decoder) {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(fullPath, dims) && dims.width > 0 && dims.height > 0) {
        filesOut.emplace_back(fullPath);
        LOG_DBG("SLP", "Valid %s: %s (%dx%d)", decoder->getFormatName(), fullPath.c_str(), dims.width, dims.height);
      } else {
        LOG_ERR("SLP", "Invalid image: %s (could not read dimensions)", fullPath.c_str());
      }
    }
    file.close();
  }
  dir.close();
}

void validateSleepImagesOnce() {
  SleepCacheMutex::Guard guard;
  uint8_t sourceMode = SETTINGS.sleepScreenSource;
  if (sourceMode >= CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SCREEN_SOURCE_COUNT) {
    sourceMode = CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_SLEEP;
  }

  if (sleepImageCache.scanned && sleepImageCache.sourceMode == sourceMode) {
    return;
  }

  // Try loading from persistent cache first
  if (loadSleepImageCacheFromFile(sleepImageCache) && sleepImageCache.sourceMode == sourceMode) {
    LOG_INF("SLP", "Loaded %d sleep images from cache", (int)sleepImageCache.validFiles.size());
    return;
  }

  sleepImageCache.scanned = false;
  sleepImageCache.sourceMode = sourceMode;
  sleepImageCache.validFiles.clear();

  const std::string sourcePath = getSleepSourcePath(sourceMode);
  const bool recursive = shouldScanRecursively(sourceMode);
  scanSleepImagesInDirectory(sourcePath, recursive, sleepImageCache.validFiles);

  sleepImageCache.scanned = true;
  LOG_INF("SLP", "Source '%s' found %d valid sleep images", getSleepSourceName(sourceMode),
          (int)sleepImageCache.validFiles.size());
  saveSleepImageCacheToFile(sleepImageCache);
}
}  // namespace

void invalidateSleepImageCache() {
  SleepCacheMutex::Guard guard;
  sleepImageCache.scanned = false;
  sleepImageCache.sourceMode = 0xFF;
  sleepImageCache.validFiles.clear();
  {
    SpiBusMutex::Guard spiGuard;
    Storage.remove(SLEEP_CACHE_FILE);
  }
  LOG_INF("SLP", "Sleep image cache invalidated");
}

int validateAndCountSleepImages() {
  invalidateSleepImageCache();
  validateSleepImagesOnce();
  SleepCacheMutex::Guard guard;
  return static_cast<int>(sleepImageCache.validFiles.size());
}

void SleepActivity::onEnter() {
  Activity::onEnter();
  // Skip the "Entering Sleep..." popup to avoid unnecessary screen refresh
  // The sleep screen will be displayed immediately anyway

  // Optional extension point for third-party sleep apps.
  if (tryRenderExternalSleepApp(renderer, mappedInput)) {
    return;
  }

  // Transparent mode: preserve current screen content, just overlay lock icon
  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT) {
    return renderTransparentSleepScreen();
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  SpiBusMutex::Guard guard;
  SleepCacheMutex::Guard cacheGuard;

  // Use pinned cover if set
  if (SETTINGS.sleepPinnedPath[0] != '\0') {
    const std::string pinnedPath(SETTINGS.sleepPinnedPath);
    LOG_INF("SLP", "Using pinned sleep cover: %s", pinnedPath.c_str());
    if (isBmpFile(pinnedPath)) {
      FsFile file;
      if (Storage.openFileForRead("SLP", pinnedPath, file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          file.close();
          return;
        }
        file.close();
        // File in cache is invalid - delete cache so it's rebuilt next time
        LOG_WRN("SLP", "Cached image invalid, clearing cache");
        Storage.remove(SLEEP_CACHE_PATH);
      }
    } else {
      renderImageSleepScreen(pinnedPath);
      return;
    }
    LOG_WRN("SLP", "Pinned sleep cover failed, falling back to random");
  }

#if ENABLE_POKEMON_PARTY
  if (SETTINGS.sleepScreenSource == CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_POKEDEX &&
      !APP_STATE.openEpubPath.empty()) {
    JsonDocument pokemonDoc;
    if (PokemonBookDataStore::loadPokemonDocument(APP_STATE.openEpubPath, pokemonDoc)) {
      const char* sleepImagePath = pokemonDoc["pokemon"]["sleepImagePath"] | "";
      if (sleepImagePath[0] != '\0') {
        const std::string partySleepImagePath(sleepImagePath);
        LOG_INF("SLP", "Using Pokemon party sleep image: %s", partySleepImagePath.c_str());
        if (isBmpFile(partySleepImagePath)) {
          FsFile file;
          if (Storage.openFileForRead("SLP", partySleepImagePath, file)) {
            Bitmap bitmap(file, true);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              renderBitmapSleepScreen(bitmap);
              file.close();
              return;
            }
            file.close();
          }
        } else if (isSupportedSleepImage(partySleepImagePath)) {
          renderImageSleepScreen(partySleepImagePath);
          return;
        }
        LOG_WRN("SLP", "Pokemon party sleep image failed, falling back to random");
      }
    }
  }
#endif

  validateSleepImagesOnce();
  const auto numFiles = sleepImageCache.validFiles.size();
  if (numFiles > 0) {
    // Generate a random number between 0 and numFiles-1
    auto randomFileIndex = random(numFiles);
    // If we picked the same image as last time, pick the next one
    if (numFiles > 1 && randomFileIndex == APP_STATE.lastSleepImage) {
      randomFileIndex = (randomFileIndex + 1) % numFiles;
    }
    // Only save to file if the selection actually changed
    const bool selectionChanged = (APP_STATE.lastSleepImage != randomFileIndex);
    APP_STATE.lastSleepImage = randomFileIndex;
    if (selectionChanged) {
      APP_STATE.saveToFile();
    }
    const auto& filename = sleepImageCache.validFiles[randomFileIndex];
    LOG_INF("SLP", "Loading: %s", filename.c_str());

    if (isBmpFile(filename)) {
      // Use existing BMP rendering path
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          file.close();
          return;
        }
        LOG_ERR("SLP", "Invalid BMP: %s", filename.c_str());
        file.close();
      }
    } else {
      // Use new PNG/JPEG rendering path
      renderImageSleepScreen(filename);
      return;
    }
  }

  // Legacy fallback for source "Sleep": root-level /sleep.bmp|png|jpg|jpeg.
  if (SETTINGS.sleepScreenSource == CrossPointSettings::SLEEP_SCREEN_SOURCE::SLEEP_SOURCE_SLEEP) {
    const char* rootSleepImages[] = {"/sleep.bmp", "/sleep.png", "/sleep.jpg", "/sleep.jpeg"};
    for (const char* sleepImagePath : rootSleepImages) {
      if (isBmpFile(sleepImagePath)) {
        FsFile file;
        if (Storage.openFileForRead("SLP", sleepImagePath, file)) {
          Bitmap bitmap(file, true);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            LOG_INF("SLP", "Loading: %s", sleepImagePath);
            renderBitmapSleepScreen(bitmap);
            file.close();
            return;
          }
          file.close();
        }
      } else {
        const ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(sleepImagePath);
        if (decoder) {
          ImageDimensions dims = {0, 0};
          if (decoder->getDimensions(sleepImagePath, dims) && dims.width > 0 && dims.height > 0) {
            LOG_INF("SLP", "Loading: %s", sleepImagePath);
            renderImageSleepScreen(sleepImagePath);
            return;
          }
        }
      }
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::drawLockIcon(const int cx, const int cy) const {
  // White background badge so icon is visible over any content
  renderer.fillRect(cx - 13, cy - 13, 26, 22, false);

  // Shackle (U-shape above body): two vertical lines + horizontal top
  renderer.drawLine(cx - 5, cy - 1, cx - 5, cy - 10);
  renderer.drawLine(cx + 5, cy - 1, cx + 5, cy - 10);
  renderer.drawLine(cx - 5, cy - 10, cx + 5, cy - 10);

  // Body: filled black rectangle with white interior
  renderer.fillRect(cx - 9, cy, 18, 12, true);
  renderer.fillRect(cx - 8, cy + 1, 16, 10, false);

  // Keyhole slot (small black mark in center of body)
  renderer.fillRect(cx - 1, cy + 3, 3, 5, true);
}

void SleepActivity::renderTransparentSleepScreen() const {
  // Preserve current e-ink content: do NOT clear the screen.
  // Just draw a small lock icon in the bottom status bar area.
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  drawLockIcon(pageWidth / 2, pageHeight - 14);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Make sleep screen dark unless light is selected in settings
  const bool lightScreen = (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) ||
                           (SETTINGS.sleepScreen == CrossPointSettings::FOLLOW_THEME && !SETTINGS.darkMode);

  if (!lightScreen) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderImageSleepScreen(const std::string& imagePath) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("SLP", "No decoder for: %s", imagePath.c_str());
    return renderDefaultSleepScreen();
  }

  ImageDimensions dims = {0, 0};
  if (!decoder->getDimensions(imagePath, dims) || dims.width <= 0 || dims.height <= 0) {
    LOG_ERR("SLP", "Could not get dimensions for: %s", imagePath.c_str());
    return renderDefaultSleepScreen();
  }

  LOG_INF("SLP", "Image %dx%d, screen %dx%d", dims.width, dims.height, pageWidth, pageHeight);

  // Calculate scale and position
  float scaleX = (dims.width > pageWidth) ? static_cast<float>(pageWidth) / dims.width : 1.0f;
  float scaleY = (dims.height > pageHeight) ? static_cast<float>(pageHeight) / dims.height : 1.0f;
  float scale = (scaleX < scaleY) ? scaleX : scaleY;
  if (scale > 1.0f) scale = 1.0f;

  int displayWidth = static_cast<int>(dims.width * scale);
  int displayHeight = static_cast<int>(dims.height * scale);

  // Center the image
  int x = (pageWidth - displayWidth) / 2;
  int y = (pageHeight - displayHeight) / 2;

  LOG_INF("SLP", "Rendering at %d,%d size %dx%d (scale %.2f)", x, y, displayWidth, displayHeight, scale);

  // Clear screen and prepare for rendering
  renderer.clearScreen();

  // Check if grayscale is enabled (no filter selected)
  const bool useGrayscale = SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  // Configure render settings
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = pageWidth;
  config.maxHeight = pageHeight;
  config.useGrayscale = useGrayscale;
  config.useDithering = true;

  // Render the image to framebuffer (BW pass)
  renderer.setRenderMode(GfxRenderer::BW);
  if (!decoder->decodeToFramebuffer(imagePath, renderer, config)) {
    LOG_ERR("SLP", "Failed to decode: %s", imagePath.c_str());
    return renderDefaultSleepScreen();
  }

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  // If grayscale is enabled, do additional passes for 4-level grayscale
  if (useGrayscale) {
    // LSB pass
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    decoder->decodeToFramebuffer(imagePath, renderer, config);
    renderer.copyGrayscaleLsbBuffers();

    // MSB pass
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    decoder->decodeToFramebuffer(imagePath, renderer, config);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}
