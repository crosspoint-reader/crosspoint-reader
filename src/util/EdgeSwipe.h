#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace edge_swipe {
enum class Edge : uint8_t { None, Top, Bottom, Left };
enum class Stage : uint8_t { Idle, Peek, Armed };

struct Config {
  static constexpr float DEFAULT_DPI = 220.0f;
  static constexpr float COMPACT_PANEL_DPI = 235.0f;
  static constexpr float EDGE_MM = 3.5f;
  static constexpr float PEEK_MM = 2.0f;
  static constexpr float ARMED_MM = 7.0f;
  static constexpr float FLICK_MM_PER_SECOND = 100.0f;
  static constexpr float TAB_WIDTH_MM = 7.0f;
  static constexpr float TAB_DEPTH_MM = 3.5f;
  static constexpr float MARGIN_MM = 0.5f;
  static constexpr int LOCK_PX = 10;
  static constexpr uint32_t VELOCITY_MS = 100;
  static constexpr uint32_t MIN_REFRESH_MS = 150;
  static constexpr uint32_t CLEANUP_MS = 200;
  static constexpr unsigned MAX_REFRESHES = 3;
  static constexpr unsigned SNAPSHOT_BYTES = 1024;
  static int pixels(float mm, float dpi) { return static_cast<int>(mm * dpi / 25.4f + 0.5f); }
};

struct State {
  Edge edge = Edge::None;
  Stage stage = Stage::Idle;
  int position = 0;
  int travel = 0;
  int velocity = 0;  // inward pixels / second over the last 100 ms
  uint32_t contact = 0;
  bool tracking = false;
  bool claimed = false;
  bool commit = false;
  bool committed = false;  // terminal outcome, retained until the next contact
};

// Input and state are in the live logical screen orientation.
class Recognizer {
 public:
  void begin(int x, int y, int w, int h, float deviceDpi, uint32_t now) {
    state = {};
    state.contact = ++contact;
    state.tracking = true;
    startX = x;
    startY = y;
    height = h;
    dpi = deviceDpi;
    count = 0;
    const int zone = Config::pixels(Config::EDGE_MM, dpi);
    rejected = x < 0 || x >= w || y < 0 || y >= h || (x > zone && y > zone && y < h - 1 - zone);
    startedAt = now;
  }

  void move(int x, int y, uint32_t now) {
    if (!state.tracking || rejected) return;
    const int dx = x - startX;
    const int dy = y - startY;
    if (state.edge == Edge::None) {
      if (std::max(std::abs(dx), std::abs(dy)) < Config::LOCK_PX) return;
      const int zone = Config::pixels(Config::EDGE_MM, dpi);
      // Only the reported down point qualifies. Extrapolation is unsafe without
      // measured controller latency: it would steal ordinary in-screen swipes.
      if (std::abs(dx) > std::abs(dy)) {
        if (startX <= zone && dx > 0) state.edge = Edge::Left;
      } else {
        if (startY <= zone && dy > 0) state.edge = Edge::Top;
        if (startY >= height - 1 - zone && dy < 0) state.edge = Edge::Bottom;
      }
      if (state.edge == Edge::None) {
        rejected = true;
        return;
      }
      state.claimed = true;
      state.position = (state.edge == Edge::Top || state.edge == Edge::Bottom) ? startX : startY;
      addSample(0, startedAt);
    }
    switch (state.edge) {
      case Edge::Left:
        state.travel = dx;
        break;
      case Edge::Top:
        state.travel = dy;
        break;
      case Edge::Bottom:
        state.travel = -dy;
        break;
      case Edge::None:
        break;
    }
    addSample(state.travel, now);
    state.stage = state.travel >= Config::pixels(Config::ARMED_MM, dpi)  ? Stage::Armed
                  : state.travel >= Config::pixels(Config::PEEK_MM, dpi) ? Stage::Peek
                                                                         : Stage::Idle;
  }

  void release(uint32_t now) {
    state.commit = false;
    if (state.claimed) {
      addSample(state.travel, now);
      state.commit = state.stage == Stage::Armed || (state.travel >= Config::pixels(Config::PEEK_MM, dpi) &&
                                                     state.velocity > Config::pixels(Config::FLICK_MM_PER_SECOND, dpi));
    }
    state.committed = state.commit;
    state.tracking = false;
    state.stage = Stage::Idle;
  }
  void cancel() {
    state.tracking = false;
    state.stage = Stage::Idle;
    state.commit = false;
    state.committed = false;
  }
  void nextFrame() {
    if (!state.tracking) {
      state.claimed = false;
      state.commit = false;
    }
  }
  const State& getState() const { return state; }
  bool pendingDirection() const { return state.tracking && !rejected && state.edge == Edge::None; }

 private:
  struct Sample {
    int travel;
    uint32_t time;
  };
  // Polling retains one sample per 10 ms, enough for a 100 ms window.
  Sample samples[12]{};
  unsigned count = 0;
  uint32_t contact = 0;
  uint32_t startedAt = 0;
  int startX = 0, startY = 0, height = 0;
  float dpi = Config::DEFAULT_DPI;
  bool rejected = false;
  State state;

  void addSample(int travel, uint32_t now) {
    while (count && now - samples[0].time > Config::VELOCITY_MS) {
      for (unsigned i = 1; i < count; ++i) samples[i - 1] = samples[i];
      --count;
    }
    if (count && now - samples[count - 1].time < 10) {
      if (count > 1) samples[count - 1] = {travel, now};
    } else {
      if (count == 12) {
        for (unsigned i = 1; i < count; ++i) samples[i - 1] = samples[i];
        --count;
      }
      samples[count++] = {travel, now};
    }
    state.velocity =
        count && now != samples[0].time
            ? static_cast<int>((static_cast<int64_t>(travel - samples[0].travel) * 1000) / (now - samples[0].time))
            : 0;
  }
};
}  // namespace edge_swipe
