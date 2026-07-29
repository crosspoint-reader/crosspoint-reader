#if CROSSPOINT_AUDIO_PLAYER

#include "AudioPlayerActivity.h"

#include <Audio.h>
#include <AudioManager.h>
#include <BoardConfig.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SdFatFS.h>

#include "CrossPointSettings.h"
#include "HalDisplay.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t AUDIO_TASK_STACK = 16384;  // decode (FLAC worst) + I2S write
constexpr UBaseType_t AUDIO_TASK_PRIO = 5;
constexpr BaseType_t AUDIO_TASK_CORE = 0;  // UI/render stay on core 1
constexpr size_t PLAYLIST_MAX = 500;
constexpr unsigned long EOF_GRACE_MS = 2500;   // connecttoFS -> isRunning settle time
constexpr unsigned long TIME_REDRAW_MS = 5000;  // e-ink partial refresh cadence

class MutexLock {
 public:
  explicit MutexLock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~MutexLock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};

// Codec control (I2C init + analog volume) — one instance for the firmware.
// Its I2S path is never used; ESP32-audioI2S owns I2S.
freeink::AudioManager& codecManager() {
  static freeink::AudioManager mgr;
  return mgr;
}

void formatTime(uint32_t seconds, char* out, size_t outLen) {
  if (seconds >= 3600) {
    snprintf(out, outLen, "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600), (unsigned long)((seconds / 60) % 60),
             (unsigned long)(seconds % 60));
  } else {
    snprintf(out, outLen, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  }
}
}  // namespace

AudioPlayerActivity::AudioPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("AudioPlayer", renderer, mappedInput), initialPath(std::move(path)) {}

AudioPlayerActivity::~AudioPlayerActivity() = default;

void AudioPlayerActivity::onEnter() {
  Activity::onEnter();

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  barX = 20;
  barW = w - 40;
  barH = 12;
  barY = h / 2 + 10;
  ctrlY = barY + 40;
  ctrlH = 44;
  volY = ctrlY + ctrlH + 16;
  volH = 36;

  audioMutex = xSemaphoreCreateMutex();
  if (!audioMutex) {
    LOG_ERR("AUDIO", "mutex alloc failed");
    finish();
    return;
  }

  if (!codecManager().begin()) {
    LOG_ERR("AUDIO", "codec init failed");
    openFailed = true;
    requestUpdate(true);
    return;
  }
  codecManager().setVolume(SETTINGS.audioVolume);

  audio = makeUniqueNoThrow<Audio>();
  if (!audio) {
    LOG_ERR("AUDIO", "OOM: Audio object");
    openFailed = true;
    requestUpdate(true);
    return;
  }
  const auto& pins = BoardConfig::ACTIVE.audio;
  audio->setPinout(pins.bclk, pins.lrclk, pins.dout, pins.mclk);
  audio->setVolume(21);  // digital full scale; loudness is the codec's analog volume
  audioReady = true;

  buildPlaylist();

  taskStop = false;
  taskDone = false;
  if (xTaskCreatePinnedToCore(&audioTaskTrampoline, "audio_ui", AUDIO_TASK_STACK, this, AUDIO_TASK_PRIO, &audioTask,
                              AUDIO_TASK_CORE) != pdPASS) {
    LOG_ERR("AUDIO", "task create failed");
    audioTask = nullptr;
    openFailed = true;
    requestUpdate(true);
    return;
  }

  openTrack(trackIndex);
  requestUpdate(true);
}

void AudioPlayerActivity::onExit() {
  if (audioTask) {
    taskStop = true;
    while (!taskDone) vTaskDelay(1);
    audioTask = nullptr;
  }
  if (audio) {
    audio->stopSong();
    audio.reset();
  }
  if (audioMutex) {
    vSemaphoreDelete(audioMutex);
    audioMutex = nullptr;
  }
  SETTINGS.saveToFile();  // persist volume changes (single write, on exit only)
  Activity::onExit();
}

void AudioPlayerActivity::audioTaskTrampoline(void* arg) { static_cast<AudioPlayerActivity*>(arg)->audioTaskLoop(); }

void AudioPlayerActivity::audioTaskLoop() {
  while (!taskStop) {
    {
      MutexLock lock(audioMutex);
      if (audio) audio->loop();
    }
    vTaskDelay(1);
  }
  taskDone = true;
  vTaskDelete(nullptr);
}

void AudioPlayerActivity::buildPlaylist() {
  playlist.clear();
  const std::string folder = FsHelpers::extractFolderPath(initialPath);
  auto dir = Storage.open(folder.c_str());
  if (dir && dir.isDirectory()) {
    char nameBuf[256];
    while (playlist.size() < PLAYLIST_MAX) {
      auto entry = dir.openNextFile();
      if (!entry) break;
      if (entry.isDirectory()) continue;
      entry.getName(nameBuf, sizeof(nameBuf));
      if (nameBuf[0] == '.') continue;
      if (!FsHelpers::hasAudioExtension(std::string_view(nameBuf))) continue;
      playlist.push_back(folder == "/" ? "/" + std::string(nameBuf) : folder + "/" + std::string(nameBuf));
    }
  }
  FsHelpers::sortFileList(playlist);
  trackIndex = 0;
  for (size_t i = 0; i < playlist.size(); i++) {
    if (playlist[i] == initialPath) {
      trackIndex = i;
      break;
    }
  }
  if (playlist.empty()) playlist.push_back(initialPath);
}

bool AudioPlayerActivity::openTrack(size_t index) {
  if (!audioReady || index >= playlist.size()) return false;
  trackIndex = index;
  bool ok;
  {
    MutexLock lock(audioMutex);
    audio->stopSong();
    ok = audio->connecttoFS(sdFatFS(), playlist[index].c_str());
  }
  playing = ok;
  paused = false;
  openFailed = !ok;
  trackStartedAt = millis();
  lastDrawnSecond = ~0u;
  if (!ok) LOG_ERR("AUDIO", "connecttoFS failed: %s", playlist[index].c_str());
  return ok;
}

void AudioPlayerActivity::togglePause() {
  if (!audioReady || !playing) return;
  {
    MutexLock lock(audioMutex);
    audio->pauseResume();
  }
  paused = !paused;
  requestUpdate(true);
}

void AudioPlayerActivity::nextTrack(int direction) {
  if (playlist.empty()) return;
  const int next = static_cast<int>(trackIndex) + direction;
  if (next < 0 || next >= static_cast<int>(playlist.size())) return;
  openTrack(static_cast<size_t>(next));
  requestUpdate(true);
}

void AudioPlayerActivity::seekToFraction(float frac) {
  if (!audioReady || !playing) return;
  MutexLock lock(audioMutex);
  const uint32_t duration = audio->getAudioFileDuration();
  if (duration == 0) return;
  audio->setAudioPlayTime(static_cast<uint16_t>(frac * duration));
}

void AudioPlayerActivity::changeVolume(int delta) {
  int vol = static_cast<int>(SETTINGS.audioVolume) + delta;
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  if (vol == SETTINGS.audioVolume) return;
  SETTINGS.audioVolume = static_cast<uint8_t>(vol);
  codecManager().setVolume(SETTINGS.audioVolume);
  requestUpdate(true);
}

void AudioPlayerActivity::handleTouch() {
  int tx, ty;
  if (!mappedInput.wasScreenTapped(tx, ty)) return;

  // Progress bar (with a fat hit band): tap to seek
  if (ty >= barY - 12 && ty <= barY + barH + 12 && tx >= barX && tx <= barX + barW) {
    seekToFraction(static_cast<float>(tx - barX) / static_cast<float>(barW));
    requestUpdate(true);
    return;
  }
  const int w = renderer.getScreenWidth();
  // Controls row: [prev][play/pause][next]
  if (ty >= ctrlY && ty <= ctrlY + ctrlH) {
    if (tx < w / 3) {
      nextTrack(-1);
    } else if (tx < 2 * w / 3) {
      togglePause();
    } else {
      nextTrack(+1);
    }
    return;
  }
  // Volume row: [-] [value] [+]
  if (ty >= volY && ty <= volY + volH) {
    if (tx < w / 3) {
      changeVolume(-10);
    } else if (tx >= 2 * w / 3) {
      changeVolume(+10);
    }
    return;
  }
}

void AudioPlayerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    togglePause();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    nextTrack(-1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    nextTrack(+1);
  }
  handleTouch();

  // End-of-track: the decode task stops the stream; advance (or stop at the end).
  if (playing && !paused && millis() - trackStartedAt > EOF_GRACE_MS) {
    bool running;
    {
      MutexLock lock(audioMutex);
      running = audio && audio->isRunning();
    }
    if (!running) {
      if (trackIndex + 1 < playlist.size()) {
        openTrack(trackIndex + 1);
        requestUpdate(true);
      } else {
        playing = false;
        requestUpdate(true);
      }
    }
  }

  // Periodic clock/progress repaint while playing (e-ink partial refresh).
  if (playing && !paused && millis() - lastTimeRefresh >= TIME_REDRAW_MS) {
    requestUpdate();
  }
}

void AudioPlayerActivity::render(RenderLock&&) {
  lastTimeRefresh = millis();
  const int w = renderer.getScreenWidth();

  renderer.clearScreen();

  // Track name (filename without folder), wrapped to two lines max
  std::string name = playlist.empty() ? initialPath : playlist[trackIndex];
  const auto slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);

  int y = 24;
  const std::string line = renderer.truncatedText(UI_12_FONT_ID, name.c_str(), w - 24, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 8;

  char buf[64];
  snprintf(buf, sizeof(buf), "%s %u/%u", tr(STR_TRACK), (unsigned)(trackIndex + 1), (unsigned)playlist.size());
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);

  if (openFailed) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_AUDIO_OPEN_FAILED));
  } else {
    // Times + progress
    uint32_t pos = 0, dur = 0;
    if (audioReady) {
      MutexLock lock(audioMutex);
      pos = audio->getAudioCurrentTime();
      dur = audio->getAudioFileDuration();
    }
    lastDrawnSecond = pos;

    char tPos[16], tDur[16];
    formatTime(pos, tPos, sizeof(tPos));
    formatTime(dur, tDur, sizeof(tDur));
    snprintf(buf, sizeof(buf), "%s / %s", tPos, tDur);
    renderer.drawCenteredText(UI_12_FONT_ID, barY - renderer.getLineHeight(UI_12_FONT_ID) - 10, buf, true,
                              EpdFontFamily::BOLD);

    // Progress bar
    renderer.drawRect(barX, barY, barW, barH);
    if (dur > 0) {
      const int fillW = static_cast<int>(static_cast<uint64_t>(barW - 4) * pos / dur);
      renderer.fillRect(barX + 2, barY + 2, fillW, barH - 4);
    }

    // Controls row: three boxes
    const int boxW = (w - 48) / 3;
    const char* mid = (playing && !paused) ? "||" : ">";
    const char* ctrls[3] = {"|<", mid, ">|"};
    for (int i = 0; i < 3; i++) {
      const int bx = 12 + i * (boxW + 12);
      renderer.drawRect(bx, ctrlY, boxW, ctrlH);
      const int textY = ctrlY + (ctrlH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
      const int textW = renderer.getTextWidth(UI_12_FONT_ID, ctrls[i], EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, bx + (boxW - textW) / 2, textY, ctrls[i], true, EpdFontFamily::BOLD);
    }

    // Volume row: [-] nn% [+]
    const int vBoxW = (w - 48) / 3;
    const char* volLabels[3] = {"-", nullptr, "+"};
    for (int i = 0; i < 3; i += 2) {
      const int bx = 12 + i * (vBoxW + 12);
      renderer.drawRect(bx, volY, vBoxW, volH);
      const int textY = volY + (volH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
      const int textW = renderer.getTextWidth(UI_12_FONT_ID, volLabels[i], EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, bx + (vBoxW - textW) / 2, textY, volLabels[i], true, EpdFontFamily::BOLD);
    }
    snprintf(buf, sizeof(buf), "%s %u%%", tr(STR_VOLUME), (unsigned)SETTINGS.audioVolume);
    const int vTextY = volY + (volH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    const int vTextW = renderer.getTextWidth(SMALL_FONT_ID, buf);
    renderer.drawText(SMALL_FONT_ID, (w - vTextW) / 2, vTextY, buf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), (playing && !paused) ? tr(STR_PAUSE) : tr(STR_PLAY), "|<", ">|");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // CROSSPOINT_AUDIO_PLAYER
