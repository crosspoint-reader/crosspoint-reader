# DS3231 RTC による時刻保持 + X4 カレンダー無効化

Issue: zrn-ns/crosspoint-jp#40

## 背景

スリープ画面のカレンダーが正しい日付を表示できない。バッテリー駆動のスリープで GPIO13 の MOSFET ラッチが解放され MCU の電源が完全に断たれるため、ESP32 の RTC カウンタや `RTC_DATA_ATTR` 変数がすべて消失する。復帰はパワーオンリセットとなり、SD カードに保存したスリープ時点の時刻しか復元できない。

Xteink X3 には DS3231（バッテリーバックアップ付き高精度 RTC、±2ppm）が I2C バス上に搭載されているが、現在はデバイス判別プローブ（`HalGPIO.cpp:78`）でしか使われていない。

## 対応方針

- **X3**: DS3231 を時刻保持に活用し、MCU 電源断を跨いで正確な時刻を維持する
- **X4**: DS3231 非搭載のため、カレンダー機能を無効化する

## 設計

### 1. 新規 HAL クラス: `HalRTC`

**ファイル**: `lib/hal/HalRTC.h`, `lib/hal/HalRTC.cpp`

DS3231 との I2C 通信を担う薄い HAL クラス。既存の HalDisplay / HalGPIO / HalStorage / HalPowerManager と同じパターンに従う。

#### 公開インターフェース

```cpp
class HalRTC {
public:
  // DS3231 の検出と I2C 初期化。X4 では false を返す。
  bool begin();

  // DS3231 が利用可能か（begin() 成功済み）
  bool isAvailable() const;

  // DS3231 から UTC 時刻を読み取り struct tm に格納
  bool readTime(struct tm& tm) const;

  // struct tm (UTC) を DS3231 に書き込み
  bool writeTime(const struct tm& tm) const;
};

extern HalRTC halRTC;  // シングルトン
```

#### DS3231 レジスタマッピング

| レジスタ | 内容 | 形式 |
|---------|------|------|
| 0x00 | 秒 | BCD (0-59) |
| 0x01 | 分 | BCD (0-59) |
| 0x02 | 時 | BCD (0-23), 24H モード |
| 0x03 | 曜日 | 1-7（使用しない、mktime で算出） |
| 0x04 | 日 | BCD (1-31) |
| 0x05 | 月/世紀 | BCD (1-12), bit7 = century |
| 0x06 | 年 | BCD (0-99), 2000 年ベース |

#### BCD 変換

```cpp
static uint8_t toBCD(uint8_t val) { return ((val / 10) << 4) | (val % 10); }
static uint8_t fromBCD(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
```

#### 時刻の基準

- DS3231 には **UTC** を保持する
- ローカル時刻変換はシステム側の `setenv("TZ", "JST-9", 1)` + `tzset()` に任せる
- `readTime()` は UTC の `struct tm` を返し、呼び出し側で `timegm()` → `settimeofday()` する

#### I2C アクセス

- `Wire` ライブラリを使用（X3 の I2C は `HalPowerManager::begin()` で初期化済み）
- 読み取り: レジスタ 0x00 にシーク → 7 バイト連続リード
- 書き込み: レジスタ 0x00 から 7 バイト連続ライト

### 2. 起動時の時刻復元 (`src/main.cpp` setup())

既存の復元ロジックの最優先ブランチとして DS3231 読み取りを追加する。

```
1. DS3231 から読み取り成功 → settimeofday()     ← 新規（最優先）
2. RTC_DATA_ATTR デルタ計算 → settimeofday()     ← 既存（USB 給電時）
3. ESP-IDF 内部復元が有効   → そのまま使用       ← 既存
4. ファイルフォールバック   → settimeofday()     ← 既存
```

DS3231 読み取りは `HalPowerManager::begin()` の後（I2C 初期化済み）、`APP_STATE.loadFromFile()` の付近で行う。

### 3. NTP 同期成功時の DS3231 書き込み

以下の 2 箇所で NTP 同期成功後に DS3231 へ書き込む:

- `src/activities/network/WifiSelectionActivity.cpp` — `sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED` の直後
- `src/activities/reader/KOReaderSyncActivity.cpp` — 同上

```cpp
if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
  LOG_DBG("WIFI", "NTP time synced (JST)");
  // DS3231 に UTC 時刻を書き込み
  if (halRTC.isAvailable()) {
    const time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    halRTC.writeTime(utc);
  }
}
```

### 4. X4 カレンダー無効化

#### SleepActivity（カレンダー描画の抑制）

`src/activities/boot_sleep/SleepActivity.cpp` の `onEnter()`:

```cpp
// X4 では DS3231 がないため、電源断後に正確な日付を保持できない → カレンダー無効
calendarPending = SETTINGS.sleepCalendar && isTimeValid() && gpio.deviceIsX3();
```

#### SettingsActivity（設定項目の非表示）

`src/activities/settings/SettingsActivity.cpp` の `rebuildSettingsLists()` で、X4 の場合にカレンダー関連設定をスキップ:

```cpp
for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
  if (setting.category == StrId::STR_NONE_OPT) continue;
  // X4 ではカレンダー設定を非表示
  if (gpio.deviceIsX4() &&
      (setting.nameId == StrId::STR_SLEEP_CALENDAR ||
       setting.nameId == StrId::STR_SLEEP_CALENDAR_POSITION)) {
    continue;
  }
  // ... 既存の分類ロジック
}
```

設定ファイルに `sleepCalendar=1` が残っていても、SleepActivity 側で `deviceIsX3()` をチェックするため無視される。

## 変更対象ファイル

| ファイル | 変更内容 |
|---------|---------|
| `lib/hal/HalRTC.h` | 新規作成 |
| `lib/hal/HalRTC.cpp` | 新規作成 |
| `src/main.cpp` | setup() に DS3231 読み取りブランチ追加、`HalRTC` の begin() 呼び出し |
| `src/activities/network/WifiSelectionActivity.cpp` | NTP 同期後に DS3231 書き込み |
| `src/activities/reader/KOReaderSyncActivity.cpp` | NTP 同期後に DS3231 書き込み |
| `src/activities/boot_sleep/SleepActivity.cpp` | X4 でカレンダー無効化 |
| `src/activities/settings/SettingsActivity.cpp` | X4 でカレンダー設定非表示 |
