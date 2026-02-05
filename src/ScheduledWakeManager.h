#pragma once

#include <cstdint>
#include <ctime>

/**
 * ScheduledWakeManager handles scheduled wake functionality:
 * - NTP time synchronization
 * - Computing next wake time based on settings
 * - Setting up ESP32 timer wakeup
 */
class ScheduledWakeManager {
 private:
  // Private constructor for singleton
  ScheduledWakeManager() = default;
  static ScheduledWakeManager instance;

  // Track if time has been synced
  bool timeSynced = false;

  // Track if this boot was from a scheduled wake
  bool isScheduledWakeBoot = false;

  // Auto-shutdown tracking
  unsigned long scheduledWakeBootTime = 0;

 public:
  // Delete copy constructor and assignment
  ScheduledWakeManager(const ScheduledWakeManager&) = delete;
  ScheduledWakeManager& operator=(const ScheduledWakeManager&) = delete;

  // Get singleton instance
  static ScheduledWakeManager& getInstance() { return instance; }

  /**
   * Synchronize time with NTP server.
   * Should be called after WiFi connects.
   * @return true if sync successful
   */
  bool syncTimeWithNTP();

  /**
   * Check if time has been synced via NTP.
   * @return true if time is valid
   */
  bool isTimeSynced() const { return timeSynced; }

  /**
   * Get current time as time_t.
   * @return current time, or 0 if not synced
   */
  time_t getCurrentTime() const;

  /**
   * Get the next scheduled wake time based on settings.
   * @return time_t of next wake, or 0 if disabled or invalid
   */
  time_t getNextWakeTime() const;

  /**
   * Calculate seconds until next scheduled wake.
   * @return seconds until wake, or 0 if disabled/invalid
   */
  uint64_t getSecondsUntilNextWake() const;

  /**
   * Check if scheduled wake is enabled and properly configured.
   * @return true if enabled and time is synced
   */
  bool isScheduledWakeReady() const;

  /**
   * Configure ESP32 timer wakeup for scheduled wake.
   * Should be called before entering deep sleep.
   * @return true if timer was set successfully
   */
  bool setTimerWakeup();

  /**
   * Check if the current boot was caused by a scheduled timer wakeup.
   * @return true if woken by timer
   */
  bool wasWokenByTimer() const;

  /**
   * Mark this boot as a scheduled wake boot.
   * Called from main.cpp when timer wakeup is detected.
   */
  void setScheduledWakeBoot();

  /**
   * Check if this is a scheduled wake boot.
   * @return true if booted from scheduled wake
   */
  bool isScheduledWake() const { return isScheduledWakeBoot; }

  /**
   * Check if auto-shutdown timeout has been reached.
   * @return true if should shut down
   */
  bool shouldAutoShutdown() const;

  /**
   * Get formatted time string for display.
   * @param t time to format
   * @param buffer output buffer
   * @param bufferSize size of buffer
   */
  static void formatTime(time_t t, char* buffer, size_t bufferSize);

  /**
   * Get day name for bitmask index (0=Sunday, 6=Saturday).
   */
  static const char* getDayName(uint8_t dayIndex);

  /**
   * Load scheduled wake settings from config file on SD card.
   * File: /.crosspoint/scheduled_wake.conf
   *
   * Format (key=value, one per line):
   *   enabled=1           # 0=disabled, 1=enabled
   *   hour=8              # 0-23
   *   minute=0            # 0-59
   *   days=127            # Bitmask: Sun=1, Mon=2, Tue=4, Wed=8, Thu=16, Fri=32, Sat=64
   *   auto_off_minutes=30 # Auto-shutdown after sync (0=disabled)
   *   wifi_ssid=MyNetwork # WiFi network to connect to
   *   timezone_offset=-8  # Hours from UTC (e.g., -8 for PST, -5 for EST)
   *
   * @return true if config file was loaded successfully
   */
  bool loadConfigFromFile();

  /**
   * Create a template config file if it doesn't exist.
   * @return true if file was created or already exists
   */
  bool createTemplateConfigFile();

  /**
   * Get the config file path.
   */
  static const char* getConfigFilePath() { return "/.crosspoint/scheduled_wake.conf"; }
};

// Helper macro to access scheduled wake manager
#define SCHEDULED_WAKE ScheduledWakeManager::getInstance()
