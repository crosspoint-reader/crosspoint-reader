# 傾きによるページ送り機能 実装プラン

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** X3のQMI8658 IMUを使って端末の傾きでページ送り/戻しを実現する

**Architecture:** HalIMU（新規）でQMI8658の加速度データを読み取り、MappedInputManagerの傾き検知状態マシンでページ送りイベントに変換し、既存のwasPressed/wasReleasedにOR結合する。Reader Activity側の変更は不要。EpubReaderMenuActivityのクイックメニューにON/OFFトグルを追加する。

**Tech Stack:** ESP32-C3 Arduino, Wire (I2C), QMI8658 IMU

---

## Done 判定基準

- [ ] `pio run -t clean && pio run` が0エラー/0警告
- [ ] X3実機でEPUBリーダー中にクイックメニューから「傾きでページ送り」をONにできる
- [ ] ONの状態で左右に傾けると1ページ送り/戻しが発生する
- [ ] 水平に戻さないと次の傾きイベントが発生しない（1傾き=1ページ）
- [ ] X4では設定項目が表示されない
- [ ] ボタンによるページ送りが既存通り動作する
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用

---

### Task 1: HalIMU ドライバ — ヘッダ

**Files:**
- Create: `lib/hal/HalIMU.h`

- [ ] **Step 1: HalIMU.h を作成**

QMI8658 IMUの加速度データ読み取りドライバ。X3専用。

```cpp
#pragma once

#include <cstdint>

// QMI8658 IMU driver for tilt-based page turning (X3 only).
// Reads accelerometer data via I2C. Gyroscope is disabled to save power.
class HalIMU {
 public:
  HalIMU() = default;

  // Initialize QMI8658 accelerometer. Returns false on X4 or if chip not found.
  // Precondition: Wire.begin() must have been called (done by HalPowerManager for X3).
  bool begin();

  // Read current accelerometer data. Call once per main loop iteration.
  void update();

  // Put QMI8658 into standby mode (call before deep sleep).
  void standby();

  // X-axis acceleration (raw 16-bit signed value, ±2G range).
  // Positive = tilt right, Negative = tilt left (in Portrait orientation).
  int16_t getAccelX() const { return accelX; }

  // Y-axis acceleration (raw 16-bit signed value, ±2G range).
  // Used for landscape orientation tilt detection.
  int16_t getAccelY() const { return accelY; }

  // True if begin() succeeded and chip is active.
  bool isAvailable() const { return available; }

 private:
  int16_t accelX = 0;
  int16_t accelY = 0;
  bool available = false;
  uint8_t chipAddr = 0;  // Resolved I2C address (0x6B or 0x6A)

  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t* outValue);
  bool readReg16LE(uint8_t reg, int16_t* outValue);
};

extern HalIMU imu;
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: ヘッダのみなのでリンクエラーは出ない（まだcppからincludeされていない）

- [ ] **Step 3: コミット**

```bash
git add lib/hal/HalIMU.h
git commit -m "✨ HalIMU ヘッダを追加（QMI8658加速度ドライバ、Issue #20）"
```

---

### Task 2: HalIMU ドライバ — 実装

**Files:**
- Create: `lib/hal/HalIMU.cpp`

- [ ] **Step 1: HalIMU.cpp を作成**

QMI8658のデータシートに基づくレジスタ操作。I2Cアドレス定数は `HalGPIO.h` で既に定義済み（`I2C_ADDR_QMI8658`, `I2C_ADDR_QMI8658_ALT`, `QMI8658_WHO_AM_I_REG`, `QMI8658_WHO_AM_I_VALUE`）なのでそのまま利用する。

```cpp
#include "HalIMU.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <Wire.h>

// QMI8658 register addresses
namespace {
constexpr uint8_t REG_CTRL1 = 0x02;   // SPI/I2C interface and sensor enable
constexpr uint8_t REG_CTRL2 = 0x03;   // Accelerometer settings (ODR, range)
constexpr uint8_t REG_CTRL7 = 0x15;   // Enable/disable accel and gyro
constexpr uint8_t REG_AX_L = 0x35;    // Accel X low byte
// AX_H = 0x36, AY_L = 0x37, AY_H = 0x38 (read as burst from REG_AX_L)

// CTRL2 config: ±2G range (bits 6:4 = 000), ODR = 15.625Hz (bits 3:0 = 0011)
constexpr uint8_t CTRL2_ACCEL_2G_15HZ = 0x03;

// CTRL7: enable accelerometer only (bit 0 = 1, bit 1 = 0 for gyro off)
constexpr uint8_t CTRL7_ACCEL_ONLY = 0x01;

// CTRL7: disable both (standby)
constexpr uint8_t CTRL7_STANDBY = 0x00;
}  // namespace

// Global instance
HalIMU imu;

bool HalIMU::writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(chipAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool HalIMU::readReg(uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(chipAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(chipAddr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *outValue = Wire.read();
  return true;
}

bool HalIMU::readReg16LE(uint8_t reg, int16_t* outValue) {
  Wire.beginTransmission(chipAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(chipAddr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  return true;
}

bool HalIMU::begin() {
  if (!gpio.deviceIsX3()) {
    LOG_DBG("IMU", "Not X3, skipping IMU init");
    return false;
  }

  // Try primary address, then fallback
  uint8_t whoami = 0;
  chipAddr = I2C_ADDR_QMI8658;
  if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    chipAddr = I2C_ADDR_QMI8658_ALT;
    if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
      LOG_ERR("IMU", "QMI8658 not found");
      chipAddr = 0;
      return false;
    }
  }

  // Configure accelerometer: ±2G, 15.625Hz ODR
  if (!writeReg(REG_CTRL2, CTRL2_ACCEL_2G_15HZ)) {
    LOG_ERR("IMU", "Failed to configure CTRL2");
    return false;
  }

  // Enable accelerometer only (gyro disabled)
  if (!writeReg(REG_CTRL7, CTRL7_ACCEL_ONLY)) {
    LOG_ERR("IMU", "Failed to configure CTRL7");
    return false;
  }

  available = true;
  LOG_INF("IMU", "QMI8658 initialized at 0x%02X", chipAddr);
  return true;
}

void HalIMU::update() {
  if (!available) return;

  // Burst read 4 bytes: AX_L, AX_H, AY_L, AY_H
  Wire.beginTransmission(chipAddr);
  Wire.write(REG_AX_L);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(chipAddr, static_cast<uint8_t>(4), static_cast<uint8_t>(true)) < 4) {
    while (Wire.available()) Wire.read();
    return;
  }
  const uint8_t axl = Wire.read();
  const uint8_t axh = Wire.read();
  const uint8_t ayl = Wire.read();
  const uint8_t ayh = Wire.read();
  accelX = static_cast<int16_t>((static_cast<uint16_t>(axh) << 8) | axl);
  accelY = static_cast<int16_t>((static_cast<uint16_t>(ayh) << 8) | ayl);
}

void HalIMU::standby() {
  if (!available) return;
  writeReg(REG_CTRL7, CTRL7_STANDBY);
  available = false;
  LOG_DBG("IMU", "Standby");
}
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功（HalIMU.cppはlib/hal/に配置されるため自動的にコンパイル対象になるが、まだmainからincludeしていないのでリンクに含まれない可能性がある。エラーがなければOK）

- [ ] **Step 3: コミット**

```bash
git add lib/hal/HalIMU.cpp
git commit -m "✨ HalIMU実装を追加（QMI8658加速度読み取り、Issue #20）"
```

---

### Task 3: i18n翻訳文字列の追加

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `lib/I18n/translations/japanese.yaml`

- [ ] **Step 1: english.yaml の末尾に追加**

```yaml
STR_TILT_PAGE_TURN: "Tilt Page Turn"
```

`STR_DEBUG_DISPLAY` の後に追加する。

- [ ] **Step 2: japanese.yaml の末尾に追加**

```yaml
STR_TILT_PAGE_TURN: "傾きでページ送り"
```

`STR_DEBUG_DISPLAY` の後に追加する。

- [ ] **Step 3: i18nヘッダ再生成**

Run: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
Expected: 正常終了、`I18nKeys.h` に `STR_TILT_PAGE_TURN` が追加される

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 5: コミット**

注意: 生成ファイル（`I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`）は`.gitignore`に含まれているためコミット対象外。

```bash
git add lib/I18n/translations/english.yaml lib/I18n/translations/japanese.yaml
git commit -m "✨ 傾きページ送りのi18n文字列を追加（Issue #20）"
```

---

### Task 4: CrossPointSettings に tiltPageTurn フィールドを追加

**Files:**
- Modify: `src/CrossPointSettings.h:248` — `debugDisplay` の後に追加
- Modify: `src/JsonSettingsIO.cpp:118-122` — saveSettingsに手動フィールド追加
- Modify: `src/JsonSettingsIO.cpp:210-215` — loadSettingsに手動フィールド追加

- [ ] **Step 1: CrossPointSettings.h にメンバ追加**

`src/CrossPointSettings.h` の `uint8_t debugDisplay = 0;` の後（249行目付近）に追加:

```cpp
  // Tilt-based page turning using QMI8658 IMU (X3 only, 0=OFF, 1=ON)
  uint8_t tiltPageTurn = 0;
```

- [ ] **Step 2: JsonSettingsIO.cpp の saveSettings に追加**

`src/JsonSettingsIO.cpp` の `saveSettings` 関数内、`doc["frontButtonRight"] = s.frontButtonRight;` の後（122行目付近）に追加:

```cpp
  // Tilt page turn (X3 only, not in SettingsList)
  doc["tiltPageTurn"] = s.tiltPageTurn;
```

- [ ] **Step 3: JsonSettingsIO.cpp の loadSettings に追加**

`src/JsonSettingsIO.cpp` の `loadSettings` 関数内、`s.frontButtonRight = clamp(...)` の後に追加:

```cpp
  // Tilt page turn (X3 only, not in SettingsList)
  s.tiltPageTurn = clamp(doc["tiltPageTurn"] | (uint8_t)0, 2, 0);
```

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 5: コミット**

```bash
git add src/CrossPointSettings.h src/JsonSettingsIO.cpp
git commit -m "✨ CrossPointSettingsにtiltPageTurnフィールドを追加（Issue #20）"
```

---

### Task 5: MappedInputManager に傾き検知を統合

**Files:**
- Modify: `src/MappedInputManager.h`
- Modify: `src/MappedInputManager.cpp`

- [ ] **Step 1: MappedInputManager.h に傾き検知メンバを追加**

`src/MappedInputManager.h` を以下のように修正:

ヘッダにinclude追加:
```cpp
#pragma once

#include <HalGPIO.h>
#include <HalIMU.h>
```

クラス定義にメンバ追加（`private:` セクション内、`Orientation effectiveOrientation` の後）:

```cpp
  // Tilt page turn state machine
  enum class TiltState : uint8_t { IDLE, COOLDOWN };
  TiltState tiltState = TiltState::IDLE;
  bool tiltPageForward = false;  // One-shot event: tilt triggered PageForward
  bool tiltPageBack = false;     // One-shot event: tilt triggered PageBack

  void updateTilt();
  bool wasTiltTriggered(Button button) const;
```

`update()` メソッドの宣言を変更（`const` を外す — tilt状態を更新するため）:
```cpp
  void update() { gpio.update(); updateTilt(); }
```

同様に `update()` から `const` を外す必要がある。

- [ ] **Step 2: MappedInputManager.cpp に傾き検知ロジックを実装**

`src/MappedInputManager.cpp` に以下を追加:

ヘッダにinclude追加:
```cpp
#include "CrossPointSettings.h"
#include <HalIMU.h>
```

（`CrossPointSettings.h` は既にinclude済みなので `HalIMU.h` のみ追加）

ファイル末尾に以下を追加:

```cpp
namespace {
// QMI8658 ±2G range: 16384 LSB/G
// threshold_high = 0.4G ≈ 6554 LSB
// threshold_low  = 0.2G ≈ 3277 LSB
constexpr int16_t TILT_THRESHOLD_HIGH = 6554;
constexpr int16_t TILT_THRESHOLD_LOW = 3277;
}  // namespace

void MappedInputManager::updateTilt() {
  // Clear one-shot events from previous frame
  tiltPageForward = false;
  tiltPageBack = false;

  if (!SETTINGS.tiltPageTurn || !imu.isAvailable()) return;

  imu.update();

  // Select axis and sign based on screen orientation
  int16_t accel = 0;
  bool invertDirection = false;
  switch (effectiveOrientation) {
    case Orientation::Portrait:
      accel = imu.getAccelX();
      break;
    case Orientation::PortraitInverted:
      accel = imu.getAccelX();
      invertDirection = true;
      break;
    case Orientation::LandscapeClockwise:
      accel = imu.getAccelY();
      break;
    case Orientation::LandscapeCounterClockwise:
      accel = imu.getAccelY();
      invertDirection = true;
      break;
  }

  const int16_t absAccel = (accel < 0) ? -accel : accel;

  switch (tiltState) {
    case TiltState::IDLE:
      if (absAccel > TILT_THRESHOLD_HIGH) {
        // Trigger page turn
        const bool forward = invertDirection ? (accel < 0) : (accel > 0);
        if (forward) {
          tiltPageForward = true;
        } else {
          tiltPageBack = true;
        }
        tiltState = TiltState::COOLDOWN;
      }
      break;

    case TiltState::COOLDOWN:
      // Wait until device returns to near-horizontal
      if (absAccel < TILT_THRESHOLD_LOW) {
        tiltState = TiltState::IDLE;
      }
      break;
  }
}

bool MappedInputManager::wasTiltTriggered(const Button button) const {
  if (button == Button::PageForward) return tiltPageForward;
  if (button == Button::PageBack) return tiltPageBack;
  return false;
}
```

- [ ] **Step 3: wasPressed / wasReleased に傾きイベントをOR結合**

`src/MappedInputManager.cpp` の既存の `wasPressed` / `wasReleased` を修正:

変更前:
```cpp
bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }
```

変更後:
```cpp
bool MappedInputManager::wasPressed(const Button button) const {
  return mapButton(button, &HalGPIO::wasPressed) || wasTiltTriggered(button);
}

bool MappedInputManager::wasReleased(const Button button) const {
  return mapButton(button, &HalGPIO::wasReleased) || wasTiltTriggered(button);
}
```

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 5: コミット**

```bash
git add src/MappedInputManager.h src/MappedInputManager.cpp
git commit -m "👍 MappedInputManagerに傾き検知を統合（Issue #20）"
```

---

### Task 6: main.cpp に HalIMU 初期化を追加

**Files:**
- Modify: `src/main.cpp:8` — include追加
- Modify: `src/main.cpp:349` — 初期化コード追加

- [ ] **Step 1: include追加**

`src/main.cpp` のincludeセクション（`#include <HalGPIO.h>` の後、8行目付近）に追加:

```cpp
#include <HalIMU.h>
```

- [ ] **Step 2: SETTINGS.loadFromFile() の後に IMU 初期化を追加**

`src/main.cpp` の `SETTINGS.loadFromFile();`（349行目）の後に追加:

```cpp
  // Initialize IMU for tilt page turn (X3 only, skipped if setting is off or device is X4)
  if (SETTINGS.tiltPageTurn) {
    imu.begin();
  }
```

- [ ] **Step 3: ディープスリープ前のスタンバイ処理を追加**

`HalPowerManager::startDeepSleep` がディープスリープの直前に呼ばれる。ここにIMUスタンバイを追加する。

`lib/hal/HalPowerManager.cpp` の `void HalPowerManager::startDeepSleep(HalGPIO& gpio) const` 関数内、`esp_sleep_config_gpio_isolate();` の前（79行目付近）に追加:

```cpp
  // Put IMU into standby before sleep to reduce power consumption
  imu.standby();
```

また `lib/hal/HalPowerManager.cpp` の先頭にinclude追加:

```cpp
#include <HalIMU.h>
```

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 5: コミット**

```bash
git add src/main.cpp lib/hal/HalPowerManager.cpp
git commit -m "👍 main.cppにHalIMU初期化、スリープ前スタンバイを追加（Issue #20）"
```

---

### Task 7: EpubReaderMenuActivity にクイックメニュー項目を追加

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.h:14-31`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp:26-47`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp:180-200`

- [ ] **Step 1: MenuAction enum に TILT_PAGE_TURN を追加**

`src/activities/reader/EpubReaderMenuActivity.h` の `enum class MenuAction` に、`DELETE_CACHE` の後に追加:

```cpp
    DELETE_CACHE,
    TILT_PAGE_TURN
```

（既存の `DELETE_CACHE` の末尾にカンマを追加し、`TILT_PAGE_TURN` を追加）

- [ ] **Step 2: buildMenuItems() にメニュー項目を追加**

`src/activities/reader/EpubReaderMenuActivity.cpp` の `buildMenuItems` 関数内に、include追加:

```cpp
#include <HalGPIO.h>
```

`buildMenuItems` 関数の `items.push_back({MenuAction::DELETE_CACHE, ...})` の後、`return items;` の前に追加:

```cpp
  if (gpio.deviceIsX3()) {
    items.push_back({MenuAction::TILT_PAGE_TURN, StrId::STR_TILT_PAGE_TURN});
  }
```

- [ ] **Step 3: getMenuItemValue() に値表示を追加**

`src/activities/reader/EpubReaderMenuActivity.cpp` の `getMenuItemValue` 関数内、`switch (action)` のケースに追加:

```cpp
    case MenuAction::TILT_PAGE_TURN:
      return SETTINGS.tiltPageTurn ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
```

`default:` ケースの前に追加する。

- [ ] **Step 4: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 5: コミット**

```bash
git add src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderMenuActivity.cpp
git commit -m "✨ クイックメニューに傾きページ送り項目を追加（Issue #20）"
```

---

### Task 8: EpubReaderActivity にメニュー結果ハンドラを追加

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp:468-637`

- [ ] **Step 1: onReaderMenuConfirm に TILT_PAGE_TURN ケースを追加**

`src/activities/reader/EpubReaderActivity.cpp` のincludeセクションに追加:

```cpp
#include <HalIMU.h>
```

`onReaderMenuConfirm` 関数内の `switch (action)` に、`SYNC` ケースの `break;` の後（635行目付近）、関数の閉じ `}` の前に追加:

```cpp
    case EpubReaderMenuActivity::MenuAction::TILT_PAGE_TURN: {
      SETTINGS.tiltPageTurn = !SETTINGS.tiltPageTurn;
      SETTINGS.saveToFile();
      // Start or stop IMU based on new setting
      if (SETTINGS.tiltPageTurn) {
        if (!imu.isAvailable()) {
          imu.begin();
        }
      } else {
        imu.standby();
      }
      break;
    }
```

- [ ] **Step 2: ビルド確認**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 3: コミット**

```bash
git add src/activities/reader/EpubReaderActivity.cpp
git commit -m "👍 傾きページ送りのON/OFF切り替えハンドラを追加（Issue #20）"
```

---

### Task 9: フルビルド検証とコードフォーマット

**Files:**
- All modified files

- [ ] **Step 1: クリーンビルド**

Run: `pio run -t clean && pio run 2>&1 | tail -20`
Expected: 0 errors, 0 warnings

- [ ] **Step 2: コードフォーマット**

Run: `find src lib/hal -name "*.cpp" -o -name "*.h" | xargs clang-format -i`

- [ ] **Step 3: フォーマット後の再ビルド**

Run: `pio run 2>&1 | tail -5`
Expected: コンパイル成功

- [ ] **Step 4: 差分確認**

Run: `git diff`
Expected: clang-formatによるフォーマット修正のみ（意図しない変更がないこと）

- [ ] **Step 5: フォーマット修正があればコミット**

```bash
git add -A
git diff --cached --stat
# フォーマット修正がある場合のみ:
git commit -m "🎨 clang-formatによるコードフォーマット修正"
```

---

## 実機テスト手順（人間が実施）

以下は実機を使ったテストで、ファームウェアをX3にフラッシュした後に実施する。

1. **X3にフラッシュ**: `pio run -t upload`
2. **EPUBを開く**: 任意のEPUBファイルを開く
3. **クイックメニュー確認**: Confirmボタンでメニューを開き、末尾付近に「傾きでページ送り」（Tilt Page Turn）が表示されることを確認
4. **ONにする**: 「傾きでページ送り」を選択してONにする（値が「ON」に変わる）
5. **傾きテスト**: メニューを閉じ、端末を右に傾けてページが進むことを確認
6. **戻りテスト**: 水平に戻してから左に傾けてページが戻ることを確認
7. **1傾き1ページ**: 傾けたまま保持しても1ページしか送られないことを確認
8. **水平復帰**: 水平に戻してから再度傾けると再びページが送られることを確認
9. **ボタン併用**: ボタンでのページ送りが引き続き動作することを確認
10. **OFFテスト**: メニューからOFFに切り替え、傾けてもページが送られないことを確認
11. **向きテスト**: 画面向きを変更（Portrait/Inverted/Landscape CW/CCW）して傾きが正しく動作することを確認
12. **再起動テスト**: デバイスを再起動し、設定が保持されていることを確認
13. **メモリ確認**: シリアルログで `Free heap` が大幅に減っていないことを確認（10秒ごとに出力される）

---

## ファイル構造まとめ

| ファイル | 操作 | 責務 |
|---------|------|------|
| `lib/hal/HalIMU.h` | 新規 | QMI8658ドライバ インターフェース |
| `lib/hal/HalIMU.cpp` | 新規 | QMI8658ドライバ 実装（I2C通信、レジスタ操作） |
| `src/MappedInputManager.h` | 修正 | 傾き状態マシンのメンバ追加 |
| `src/MappedInputManager.cpp` | 修正 | 傾き検知ロジック、wasPressed/wasReleasedへのOR結合 |
| `src/CrossPointSettings.h` | 修正 | `tiltPageTurn` メンバ追加 |
| `src/JsonSettingsIO.cpp` | 修正 | JSON永続化にtiltPageTurnフィールド追加 |
| `src/activities/reader/EpubReaderMenuActivity.h` | 修正 | `MenuAction::TILT_PAGE_TURN` 追加 |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | 修正 | メニュー項目追加、値表示 |
| `src/activities/reader/EpubReaderActivity.cpp` | 修正 | ON/OFF切り替えハンドラ |
| `lib/I18n/translations/english.yaml` | 修正 | 英語翻訳追加 |
| `lib/I18n/translations/japanese.yaml` | 修正 | 日本語翻訳追加 |
| `src/main.cpp` | 修正 | IMU初期化呼び出し |
| `lib/hal/HalPowerManager.cpp` | 修正 | スリープ前のIMUスタンバイ |
