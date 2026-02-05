#include "ScheduledWakeManager.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_sntp.h>

#include <cstring>

#include "CrossPointSettings.h"

// Initialize static instance
ScheduledWakeManager ScheduledWakeManager::instance;

// NTP server to use
static const char* NTP_SERVER = "pool.ntp.org";

bool ScheduledWakeManager::syncTimeWithNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%lu] [SWM] Cannot sync NTP: WiFi not connected\n", millis());
    return false;
  }

  Serial.printf("[%lu] [SWM] Syncing time with NTP server...\n", millis());

  // Configure timezone offset from settings
  const int8_t tzOffset = SETTINGS.scheduledWakeTimezoneOffset;
  const long gmtOffsetSec = tzOffset * 3600L;

  // Configure SNTP
  configTime(gmtOffsetSec, 0, NTP_SERVER);

  // Wait for time to sync (up to 10 seconds)
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20) {
    delay(500);
    retries++;
  }

  if (retries >= 20) {
    Serial.printf("[%lu] [SWM] NTP sync failed after timeout\n", millis());
    return false;
  }

  timeSynced = true;
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  Serial.printf("[%lu] [SWM] Time synced: %s (TZ offset: %d hours)\n", millis(), timeStr, tzOffset);

  return true;
}

time_t ScheduledWakeManager::getCurrentTime() const {
  if (!timeSynced) {
    return 0;
  }
  return time(nullptr);
}

time_t ScheduledWakeManager::getNextWakeTime() const {
  if (!SETTINGS.scheduledWakeEnabled || !timeSynced) {
    return 0;
  }

  time_t now = getCurrentTime();
  if (now == 0) {
    return 0;
  }

  const struct tm* timeinfo = localtime(&now);
  if (!timeinfo) {
    return 0;
  }

  const uint8_t targetHour = SETTINGS.scheduledWakeHour;
  const uint8_t targetMinute = SETTINGS.scheduledWakeMinute;
  const uint8_t enabledDays = SETTINGS.scheduledWakeDays;

  // Start checking from today
  struct tm nextWake = *timeinfo;
  nextWake.tm_sec = 0;
  nextWake.tm_hour = targetHour;
  nextWake.tm_min = targetMinute;

  // Check up to 8 days ahead (to find next enabled day)
  for (int dayOffset = 0; dayOffset < 8; dayOffset++) {
    // Calculate the candidate time
    time_t candidate = mktime(&nextWake);

    // Get the day of week for this candidate (0=Sunday)
    const struct tm* candidateInfo = localtime(&candidate);
    uint8_t dayMask = 1 << candidateInfo->tm_wday;

    // Check if this day is enabled and the time is in the future
    if ((enabledDays & dayMask) && candidate > now) {
      return candidate;
    }

    // Move to next day
    nextWake.tm_mday++;
    mktime(&nextWake);  // Normalize the time structure
  }

  return 0;  // No valid wake time found
}

uint64_t ScheduledWakeManager::getSecondsUntilNextWake() const {
  time_t nextWake = getNextWakeTime();
  if (nextWake == 0) {
    return 0;
  }

  time_t now = getCurrentTime();
  if (nextWake <= now) {
    return 0;
  }

  return static_cast<uint64_t>(nextWake - now);
}

bool ScheduledWakeManager::isScheduledWakeReady() const {
  return SETTINGS.scheduledWakeEnabled && timeSynced && getNextWakeTime() > 0;
}

bool ScheduledWakeManager::setTimerWakeup() {
  if (!isScheduledWakeReady()) {
    Serial.printf("[%lu] [SWM] Cannot set timer wakeup: not ready\n", millis());
    return false;
  }

  uint64_t secondsUntilWake = getSecondsUntilNextWake();
  if (secondsUntilWake == 0) {
    Serial.printf("[%lu] [SWM] Cannot set timer wakeup: no valid wake time\n", millis());
    return false;
  }

  // Convert to microseconds for ESP32 timer
  uint64_t microseconds = secondsUntilWake * 1000000ULL;

  // Enable timer wakeup
  esp_err_t result = esp_sleep_enable_timer_wakeup(microseconds);
  if (result != ESP_OK) {
    Serial.printf("[%lu] [SWM] Failed to set timer wakeup: %d\n", millis(), result);
    return false;
  }

  // Log the scheduled wake time
  time_t nextWake = getNextWakeTime();
  char timeStr[64];
  formatTime(nextWake, timeStr, sizeof(timeStr));
  Serial.printf("[%lu] [SWM] Timer wakeup set for %s (in %llu seconds)\n", millis(), timeStr, secondsUntilWake);

  return true;
}

bool ScheduledWakeManager::wasWokenByTimer() const {
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  return wakeupCause == ESP_SLEEP_WAKEUP_TIMER;
}

void ScheduledWakeManager::setScheduledWakeBoot() {
  isScheduledWakeBoot = true;
  scheduledWakeBootTime = millis();
  Serial.printf("[%lu] [SWM] Scheduled wake boot detected\n", millis());
}

bool ScheduledWakeManager::shouldAutoShutdown() const {
  if (!isScheduledWakeBoot) {
    return false;
  }

  const uint8_t autoOffMinutes = SETTINGS.scheduledWakeAutoOffMinutes;
  if (autoOffMinutes == 0) {
    return false;  // Auto-shutdown disabled
  }

  unsigned long elapsedMs = millis() - scheduledWakeBootTime;
  unsigned long timeoutMs = static_cast<unsigned long>(autoOffMinutes) * 60UL * 1000UL;

  return elapsedMs >= timeoutMs;
}

void ScheduledWakeManager::formatTime(time_t t, char* buffer, size_t bufferSize) {
  const struct tm* timeinfo = localtime(&t);
  if (timeinfo) {
    strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", timeinfo);
  } else {
    snprintf(buffer, bufferSize, "Invalid time");
  }
}

const char* ScheduledWakeManager::getDayName(uint8_t dayIndex) {
  if (dayIndex < 7) {
    static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return days[dayIndex];
  }
  return "???";
}

bool ScheduledWakeManager::loadConfigFromFile() {
  const char* configPath = getConfigFilePath();

  FsFile configFile;
  if (!SdMan.openFileForRead("SWM", configPath, configFile)) {
    Serial.printf("[%lu] [SWM] Config file not found: %s\n", millis(), configPath);
    return false;
  }

  Serial.printf("[%lu] [SWM] Loading scheduled wake config from: %s\n", millis(), configPath);

  char line[128];
  while (configFile.available()) {
    // Read line
    int len = 0;
    while (configFile.available() && len < (int)sizeof(line) - 1) {
      char c = configFile.read();
      if (c == '\n' || c == '\r') {
        break;
      }
      line[len++] = c;
    }
    line[len] = '\0';

    // Skip empty lines and comments
    if (len == 0 || line[0] == '#') {
      continue;
    }

    // Parse key=value
    char* equals = strchr(line, '=');
    if (!equals) {
      continue;
    }

    *equals = '\0';
    const char* key = line;
    const char* value = equals + 1;

    // Trim whitespace from key and value
    while (*key == ' ' || *key == '\t') key++;
    while (*value == ' ' || *value == '\t') value++;

    // Parse each setting
    if (strcmp(key, "enabled") == 0) {
      SETTINGS.scheduledWakeEnabled = (uint8_t)atoi(value);
      Serial.printf("[%lu] [SWM]   enabled=%d\n", millis(), SETTINGS.scheduledWakeEnabled);
    } else if (strcmp(key, "hour") == 0) {
      int h = atoi(value);
      if (h >= 0 && h < 24) {
        SETTINGS.scheduledWakeHour = (uint8_t)h;
        Serial.printf("[%lu] [SWM]   hour=%d\n", millis(), SETTINGS.scheduledWakeHour);
      }
    } else if (strcmp(key, "minute") == 0) {
      int m = atoi(value);
      if (m >= 0 && m < 60) {
        SETTINGS.scheduledWakeMinute = (uint8_t)m;
        Serial.printf("[%lu] [SWM]   minute=%d\n", millis(), SETTINGS.scheduledWakeMinute);
      }
    } else if (strcmp(key, "days") == 0) {
      SETTINGS.scheduledWakeDays = (uint8_t)atoi(value);
      Serial.printf("[%lu] [SWM]   days=%d (0x%02X)\n", millis(), SETTINGS.scheduledWakeDays,
                    SETTINGS.scheduledWakeDays);
    } else if (strcmp(key, "auto_off_minutes") == 0) {
      SETTINGS.scheduledWakeAutoOffMinutes = (uint8_t)atoi(value);
      Serial.printf("[%lu] [SWM]   auto_off_minutes=%d\n", millis(), SETTINGS.scheduledWakeAutoOffMinutes);
    } else if (strcmp(key, "wifi_ssid") == 0) {
      strncpy(SETTINGS.scheduledWakeWifiSSID, value, sizeof(SETTINGS.scheduledWakeWifiSSID) - 1);
      SETTINGS.scheduledWakeWifiSSID[sizeof(SETTINGS.scheduledWakeWifiSSID) - 1] = '\0';
      Serial.printf("[%lu] [SWM]   wifi_ssid=%s\n", millis(), SETTINGS.scheduledWakeWifiSSID);
    } else if (strcmp(key, "timezone_offset") == 0) {
      SETTINGS.scheduledWakeTimezoneOffset = (int8_t)atoi(value);
      Serial.printf("[%lu] [SWM]   timezone_offset=%d\n", millis(), SETTINGS.scheduledWakeTimezoneOffset);
    }
  }

  configFile.close();

  // Save updated settings to persist them
  SETTINGS.saveToFile();

  Serial.printf("[%lu] [SWM] Config loaded successfully\n", millis());
  return true;
}

bool ScheduledWakeManager::createTemplateConfigFile() {
  const char* configPath = getConfigFilePath();

  // Check if file already exists
  if (SdMan.fileExists(configPath)) {
    Serial.printf("[%lu] [SWM] Config file already exists: %s\n", millis(), configPath);
    return true;
  }

  // Ensure directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile configFile;
  if (!SdMan.openFileForWrite("SWM", configPath, configFile)) {
    Serial.printf("[%lu] [SWM] Failed to create config file: %s\n", millis(), configPath);
    return false;
  }

  // Write template config
  configFile.println("# CrossPoint Scheduled Wake Configuration");
  configFile.println("# Edit this file to configure automatic wake-up for file sync");
  configFile.println("#");
  configFile.println("# enabled: 0=disabled, 1=enabled");
  configFile.println("enabled=0");
  configFile.println("");
  configFile.println("# Wake time (24-hour format)");
  configFile.println("hour=8");
  configFile.println("minute=0");
  configFile.println("");
  configFile.println("# Days of week bitmask:");
  configFile.println("#   Sun=1, Mon=2, Tue=4, Wed=8, Thu=16, Fri=32, Sat=64");
  configFile.println("#   All days = 127, Weekdays only = 62, Weekends only = 65");
  configFile.println("days=127");
  configFile.println("");
  configFile.println("# Auto-shutdown after this many minutes (0=disabled)");
  configFile.println("auto_off_minutes=30");
  configFile.println("");
  configFile.println("# WiFi network to connect to (must match saved network)");
  configFile.println("wifi_ssid=");
  configFile.println("");
  configFile.println("# Timezone offset from UTC in hours");
  configFile.println("#   Examples: PST=-8, EST=-5, UTC=0, CET=1, IST=5");
  configFile.println("timezone_offset=-8");

  configFile.close();

  Serial.printf("[%lu] [SWM] Created template config file: %s\n", millis(), configPath);
  return true;
}
