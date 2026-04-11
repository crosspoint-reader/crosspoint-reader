# DS3231 RTC 時刻保持 + X4 カレンダー無効化 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** X3 の DS3231 RTC で MCU 電源断を跨いだ時刻保持を実現し、スリープ画面カレンダーに正しい日付を表示する。X4 ではカレンダー機能を無効化する。

**Architecture:** 新規 HAL クラス `HalRTC` が DS3231 との I2C 通信を担う。起動時に DS3231 から時刻を読み取りシステムクロックに設定、NTP 同期成功時に DS3231 へ書き戻す。X4 ではカレンダー設定を設定画面から非表示にし、描画もスキップする。

**Tech Stack:** ESP32-C3 / Arduino Wire (I2C) / DS3231 (addr 0x68) / BCD エンコーディング

**Spec:** `docs/superpowers/specs/2026-04-11-ds3231-rtc-timekeeping-design.md`

---

### Task 1: HalRTC クラスの作成

**Files:**
- Create: `lib/hal/HalRTC.h`
- Create: `lib/hal/HalRTC.cpp`

- [ ] **Step 1: HalRTC.h を作成**

```cpp
// lib/hal/HalRTC.h
#pragma once

#include <ctime>

class HalRTC {
  bool _available = false;

 public:
  // DS3231 の存在を確認。X4 では false を返す。
  // I2C は HalPowerManager::begin() で初期化済みの前提。
  bool begin();

  // begin() で DS3231 が検出されたか
  bool isAvailable() const;

  // DS3231 から UTC 時刻を読み取り struct tm に格納
  bool readTime(struct tm& tm) const;

  // struct tm (UTC) を DS3231 に書き込み
  bool writeTime(const struct tm& tm) const;
};

extern HalRTC halRTC;
```

- [ ] **Step 2: HalRTC.cpp を作成**

```cpp
// lib/hal/HalRTC.cpp
#include "HalRTC.h"

#include <Logging.h>
#include <Wire.h>

#include "HalGPIO.h"

HalRTC halRTC;

static constexpr uint8_t DS3231_ADDR = 0x68;

static uint8_t toBCD(uint8_t val) { return ((val / 10) << 4) | (val % 10); }
static uint8_t fromBCD(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }

bool HalRTC::begin() {
  if (gpio.deviceIsX4()) {
    _available = false;
    return false;
  }

  // DS3231 の存在確認: 秒レジスタを読んで BCD として妥当か検証
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    LOG_DBG("RTC", "DS3231 not found");
    _available = false;
    return false;
  }
  if (Wire.requestFrom(DS3231_ADDR, static_cast<uint8_t>(1)) < 1) {
    _available = false;
    return false;
  }
  const uint8_t sec = Wire.read();
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  if (tens > 5 || ones > 9) {
    LOG_ERR("RTC", "DS3231 seconds invalid: 0x%02x", sec);
    _available = false;
    return false;
  }

  _available = true;
  LOG_DBG("RTC", "DS3231 detected");
  return true;
}

bool HalRTC::isAvailable() const { return _available; }

bool HalRTC::readTime(struct tm& tm) const {
  if (!_available) return false;

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) {
    LOG_ERR("RTC", "DS3231 read seek failed");
    return false;
  }
  if (Wire.requestFrom(DS3231_ADDR, static_cast<uint8_t>(7)) < 7) {
    LOG_ERR("RTC", "DS3231 read failed");
    return false;
  }

  const uint8_t sec = Wire.read();
  const uint8_t min = Wire.read();
  const uint8_t hour = Wire.read();
  Wire.read();  // 曜日 (使用しない)
  const uint8_t day = Wire.read();
  const uint8_t month = Wire.read();
  const uint8_t year = Wire.read();

  tm.tm_sec = fromBCD(sec & 0x7F);
  tm.tm_min = fromBCD(min & 0x7F);
  tm.tm_hour = fromBCD(hour & 0x3F);  // 24H モード前提
  tm.tm_mday = fromBCD(day & 0x3F);
  tm.tm_mon = fromBCD(month & 0x1F) - 1;  // struct tm は 0-11
  tm.tm_year = fromBCD(year) + 100;        // struct tm は 1900 ベース、DS3231 は 2000 ベース
  tm.tm_isdst = 0;

  // 妥当性チェック: 2024年以降かつ月日が範囲内
  if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) {
    LOG_ERR("RTC", "DS3231 time invalid: %d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return false;
  }

  LOG_DBG("RTC", "DS3231 read: %d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return true;
}

bool HalRTC::writeTime(const struct tm& tm) const {
  if (!_available) return false;

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x00);                              // レジスタ開始アドレス
  Wire.write(toBCD(tm.tm_sec));                  // 0x00: 秒
  Wire.write(toBCD(tm.tm_min));                  // 0x01: 分
  Wire.write(toBCD(tm.tm_hour));                 // 0x02: 時 (24H)
  Wire.write(toBCD(tm.tm_wday + 1));             // 0x03: 曜日 (1-7)
  Wire.write(toBCD(tm.tm_mday));                 // 0x04: 日
  Wire.write(toBCD(tm.tm_mon + 1));              // 0x05: 月 (1-12)
  Wire.write(toBCD((tm.tm_year + 1900) % 100));  // 0x06: 年 (下2桁)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("RTC", "DS3231 write failed");
    return false;
  }

  LOG_DBG("RTC", "DS3231 write: %d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return true;
}
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`（HalRTC は `lib/hal/` 配下のため PlatformIO が自動検出）

- [ ] **Step 4: コミット**

```bash
git add lib/hal/HalRTC.h lib/hal/HalRTC.cpp
git commit -m "✨ DS3231 RTC用のHALクラス（HalRTC）を追加"
```

---

### Task 2: 起動時に DS3231 から時刻を復元

**Files:**
- Modify: `src/main.cpp:1-44` (include 追加、halRTC.begin() 呼び出し)
- Modify: `src/main.cpp:365-400` (時刻復元ロジックに DS3231 ブランチ追加)

- [ ] **Step 1: main.cpp に HalRTC の include を追加**

`src/main.cpp` の include セクション（`#include <HalStorage.h>` の付近）に追加:

```cpp
#include <HalRTC.h>
```

- [ ] **Step 2: setup() 内で halRTC.begin() を呼ぶ**

`src/main.cpp` の `setup()` 内、`powerManager.begin();`（282行目）の直後に追加:

```cpp
  halRTC.begin();
```

`HalPowerManager::begin()` が X3 の I2C を初期化した後に呼ぶ必要がある。

- [ ] **Step 3: 時刻復元ロジックに DS3231 ブランチを最優先で追加**

`src/main.cpp` の時刻復元ブロック（376行目付近）を修正。既存の `if (unixTimeAtSleep >= ...)` の前に DS3231 読み取りを挿入:

```cpp
  {
    const time_t bootTime = time(nullptr);
    struct tm rtcTm;
    if (halRTC.readTime(rtcTm)) {
      // DS3231 から UTC 時刻を復元（最も信頼性が高い）
      const time_t rtcTime = timegm(&rtcTm);
      if (rtcTime >= 1704067200) {
        struct timeval tv = {.tv_sec = rtcTime, .tv_usec = 0};
        settimeofday(&tv, nullptr);
        LOG_DBG("MAIN", "Restored time from DS3231: %ld (boot=%ld)", (long)rtcTime, (long)bootTime);
      }
    } else if (unixTimeAtSleep >= 1704067200 && rtcTicksAtSleep > 0) {
      // RTCスローカウンタのデルタからスリープ経過時間を計算（USB給電時）
      const uint64_t rtcNow = rtc_time_get();
      const uint64_t rtcElapsed = rtcNow - rtcTicksAtSleep;
      const uint32_t rtcFreq = esp_clk_slowclk_cal_get();
      const time_t elapsedSec = static_cast<time_t>(((rtcElapsed * rtcFreq) >> 19) / 1000000);
      const time_t restoredTime = unixTimeAtSleep + elapsedSec;
      struct timeval tv = {.tv_sec = restoredTime, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      LOG_DBG("MAIN", "Restored time (RTC delta): saved=%ld +%lds = %ld (ESP-IDF boot=%ld)", (long)unixTimeAtSleep,
              (long)elapsedSec, (long)restoredTime, (long)bootTime);
    } else if (bootTime >= 1704067200) {
      LOG_DBG("MAIN", "No RTC data, using ESP-IDF restored time: %ld", (long)bootTime);
    } else if (APP_STATE.lastKnownTime >= 1704067200) {
      struct timeval tv = {.tv_sec = static_cast<time_t>(APP_STATE.lastKnownTime), .tv_usec = 0};
      settimeofday(&tv, nullptr);
      LOG_DBG("MAIN", "Restored time from file (no RTC): %lu", (unsigned long)APP_STATE.lastKnownTime);
    }
  }
```

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`

- [ ] **Step 5: コミット**

```bash
git add src/main.cpp
git commit -m "👍 起動時にDS3231から時刻を復元するロジックを追加（Issue #40）"
```

---

### Task 3: NTP 同期成功時に DS3231 へ書き込み

**Files:**
- Modify: `src/activities/network/WifiSelectionActivity.cpp:259-261`
- Modify: `src/activities/reader/KOReaderSyncActivity.cpp:36-39`

- [ ] **Step 1: WifiSelectionActivity に DS3231 書き込みを追加**

`src/activities/network/WifiSelectionActivity.cpp` の先頭 include セクションに追加:

```cpp
#include <HalRTC.h>
```

同ファイルの NTP 同期成功ログ（259行目 `LOG_DBG("WIFI", "NTP time synced (JST)");`）の直後に追加:

```cpp
      // DS3231 に UTC 時刻を書き込み
      if (halRTC.isAvailable()) {
        const time_t now = time(nullptr);
        struct tm utc;
        gmtime_r(&now, &utc);
        halRTC.writeTime(utc);
      }
```

- [ ] **Step 2: KOReaderSyncActivity に DS3231 書き込みを追加**

`src/activities/reader/KOReaderSyncActivity.cpp` の先頭 include セクションに追加:

```cpp
#include <HalRTC.h>
```

同ファイルの `syncTimeWithNTP()` 関数内、NTP 同期成功分岐（36-38行目 `if (retry < maxRetries)`）を修正:

```cpp
  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
    // DS3231 に UTC 時刻を書き込み
    if (halRTC.isAvailable()) {
      const time_t now = time(nullptr);
      struct tm utc;
      gmtime_r(&now, &utc);
      halRTC.writeTime(utc);
    }
  } else {
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`

- [ ] **Step 4: コミット**

```bash
git add src/activities/network/WifiSelectionActivity.cpp src/activities/reader/KOReaderSyncActivity.cpp
git commit -m "👍 NTP同期成功時にDS3231へUTC時刻を書き込み（Issue #40）"
```

---

### Task 4: X4 でカレンダー機能を無効化

**Files:**
- Modify: `src/activities/boot_sleep/SleepActivity.cpp:30`
- Modify: `src/activities/settings/SettingsActivity.cpp:40-51`

- [ ] **Step 1: SleepActivity でカレンダー描画を X3 のみに限定**

`src/activities/boot_sleep/SleepActivity.cpp` の先頭 include セクションに追加:

```cpp
#include "HalGPIO.h"
```

同ファイル 30 行目を修正:

変更前:
```cpp
  calendarPending = SETTINGS.sleepCalendar && isTimeValid();
```

変更後:
```cpp
  // X4 では DS3231 がないため、電源断後に正確な日付を保持できない → カレンダー無効
  calendarPending = SETTINGS.sleepCalendar && isTimeValid() && gpio.deviceIsX3();
```

- [ ] **Step 2: SettingsActivity でカレンダー設定を X4 では非表示に**

`src/activities/settings/SettingsActivity.cpp` の先頭 include セクションに `HalGPIO.h` がなければ追加:

```cpp
#include "HalGPIO.h"
```

同ファイルの `rebuildSettingsLists()` 内のループ（40行目付近）で、既存の `if (setting.category == StrId::STR_NONE_OPT) continue;` の直後に追加:

```cpp
    // X4 ではカレンダー設定を非表示（DS3231 非搭載のため正確な日付を保持できない）
    if (gpio.deviceIsX4() && (setting.nameId == StrId::STR_SLEEP_CALENDAR ||
                              setting.nameId == StrId::STR_SLEEP_CALENDAR_POSITION)) {
      continue;
    }
```

- [ ] **Step 3: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: `SUCCESS`

- [ ] **Step 4: コミット**

```bash
git add src/activities/boot_sleep/SleepActivity.cpp src/activities/settings/SettingsActivity.cpp
git commit -m "👍 X4でカレンダー機能を無効化（DS3231非搭載のため、Issue #40）"
```

---

### Task 5: コードフォーマット + 最終ビルド確認

**Files:**
- All modified files

- [ ] **Step 1: clang-format 実行**

```bash
find src lib/hal -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

- [ ] **Step 2: フルビルド確認**

Run: `pio run -t clean && pio run 2>&1 | tail -10`
Expected: `SUCCESS`、警告なし

- [ ] **Step 3: 差分確認**

```bash
git diff
```

フォーマット変更があればコミット:

```bash
git add -A
git commit -m "🎨 clang-formatによるコードフォーマット修正"
```

---

## Done 判定基準

- [ ] `pio run` が 0 errors / 0 warnings でビルド成功
- [ ] X3: 起動時に DS3231 から時刻読み取りのログ出力を確認（シリアル）
- [ ] X3: WiFi 接続 → NTP 同期 → DS3231 書き込みのログ出力を確認
- [ ] X3: スリープ → 復帰後のカレンダーが正しい日付を表示
- [ ] X4: 設定画面にカレンダー関連設定が表示されないことを確認
- [ ] X4: スリープ画面にカレンダーが表示されないことを確認
- [ ] 差分に意図しない変更が含まれていない

※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
