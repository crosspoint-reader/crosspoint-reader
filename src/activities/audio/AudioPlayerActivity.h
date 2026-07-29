#pragma once

// Audio player activity (murphy builds only). Decode via ESP32-audioI2S
// (MP3/WAV/FLAC/M4A/AAC/OGG/Opus) feeding I2S; the ES8388 codec is
// initialised and volume-controlled through the SDK AudioManager (I2C),
// which never touches I2S itself — the two stacks split cleanly.
//
// Threading: a dedicated FreeRTOS task pumps Audio::loop() (decode+I2S
// write). ESP32-audioI2S is not thread-safe, so every access to the Audio
// object — the task's loop() call and all UI-thread control calls — takes
// audioMutex.

#if CROSSPOINT_AUDIO_PLAYER

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class Audio;

class AudioPlayerActivity : public Activity {
 public:
  AudioPlayerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path);
  // Out-of-line: unique_ptr<Audio> needs the complete type at destruction.
  ~AudioPlayerActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Deep sleep kills I2S mid-song; only allow auto-sleep when paused/stopped.
  bool preventAutoSleep() override { return playing && !paused; }

 private:
  static void audioTaskTrampoline(void* arg);
  void audioTaskLoop();

  void buildPlaylist();
  bool openTrack(size_t index);
  void togglePause();
  void nextTrack(int direction);
  void seekToFraction(float frac);
  void changeVolume(int delta);
  void handleTouch();

  std::string initialPath;
  std::vector<std::string> playlist;  // full paths, natural-sorted
  size_t trackIndex = 0;

  std::unique_ptr<Audio> audio;
  SemaphoreHandle_t audioMutex = nullptr;
  TaskHandle_t audioTask = nullptr;
  volatile bool taskStop = false;
  volatile bool taskDone = false;

  bool audioReady = false;  // codec + Audio object up
  bool playing = false;     // a track is loaded
  bool paused = false;
  bool openFailed = false;
  unsigned long trackStartedAt = 0;  // millis() at connect, guards EOF detection
  uint32_t lastDrawnSecond = ~0u;
  unsigned long lastTimeRefresh = 0;

  // Layout (computed in onEnter from screen size)
  int barX = 0, barY = 0, barW = 0, barH = 0;
  int ctrlY = 0, ctrlH = 0;
  int volY = 0, volH = 0;
};

#endif  // CROSSPOINT_AUDIO_PLAYER
