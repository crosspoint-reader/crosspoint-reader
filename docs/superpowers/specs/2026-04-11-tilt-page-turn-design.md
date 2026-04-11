# 傾きによるページ送り機能 設計書

**Issue**: zrn-ns/crosspoint-jp#20
**日付**: 2026-04-11
**対象デバイス**: Xteink X3のみ（QMI8658 IMU搭載）

## Context

Xteink X3の公式FWには、本体を左右に傾けることでページ送り/戻しができる機能がある。
X3にはQST QMI8658 6軸IMU（加速度+ジャイロ）がI2Cで接続されており、現在のファームウェアではデバイス検出（WHO_AM_I）にのみ使用されている。
加速度データの読み取りを実装し、端末の傾きをページ送りトリガーとして活用する。

## 設計

### 1. QMI8658ドライバ層 — HalIMU

`lib/hal/HalIMU.h/cpp` を新規作成する。

**責務:**
- QMI8658の初期化（加速度センサーモードのみ有効化）
- X軸加速度の定期読み取り
- X3専用 — X4では `begin()` が即 `return false`

**加速度センサーのみ有効化する理由:**
傾き検知は重力加速度のX成分だけで実現できる。ジャイロスコープは不要であり、無効化することで消費電力を抑える。

**QMI8658レジスタ操作:**

| レジスタ | アドレス | 用途 |
|---------|---------|------|
| CTRL1 | 0x02 | センサー有効化設定 |
| CTRL2 | 0x03 | 加速度センサーODR・レンジ設定 |
| CTRL7 | 0x15 | 加速度/ジャイロ有効無効制御 |
| AX_L / AX_H | 0x35-0x36 | X軸加速度データ（16bit符号付き） |
| AY_L / AY_H | 0x37-0x38 | Y軸加速度データ（横向き対応用） |

**設定値:**
- レンジ: ±2G（傾き検知には十分）
- ODR: 15.625Hz（低消費電力、ページ送り検知には十分な応答速度）
- ジャイロ: 無効

**I2Cバス共有:**
既存のDS3231 RTC、BQ27220燃料ゲージと同じI2Cバス（GPIO20/GPIO0, 400kHz）を使用。
`Wire` ライブラリをそのまま利用する。

**インターフェース:**
```cpp
class HalIMU {
public:
  bool begin();           // QMI8658初期化。X4ではfalseを返す
  void update();          // 加速度データ読み取り（毎ループ呼び出し）
  void standby();         // スタンバイモードに移行（ディープスリープ前）
  int16_t getAccelX() const;  // X軸加速度（生値）
  int16_t getAccelY() const;  // Y軸加速度（横向き対応用）
  bool isAvailable() const;   // 初期化成功したか
};
```

**RAM追加:** クラスメンバ数個分のみ（~16バイト）。バッファ不要。

### 2. 傾き検知とイベント変換

**MappedInputManager** に傾き→ページ送りイベント変換ロジックを統合する。

#### 状態遷移マシン

```
IDLE ──(|accel| > threshold_high)──→ TILTED
  ↑                                     │
  │                                     │（即座にイベント発火）
  │                                     ↓
  └──(|accel| < threshold_low)──── COOLDOWN
```

- **IDLE**: 水平状態。`|accelX|` (or `|accelY|` in landscape) < `threshold_low`
- **TILTED**: 傾き検知。閾値を超えた瞬間に方向を記録し、1回のページ送りイベントを発火
- **COOLDOWN**: 水平に戻るまで次のイベントを抑制

**ヒステリシス:**
- `threshold_high`（傾き開始）: 0.4G相当の生値
- `threshold_low`（復帰）: 0.2G相当の生値
- 2つの閾値に差を設けることでチャタリング（傾き境界での振動）を防止
- 実機テストでチューニングが必要な値

**方向判定:**
- accel > +threshold_high → PageForward（右傾き）
- accel < -threshold_high → PageBack（左傾き）

#### MappedInputManager統合

**既存の `wasPressed()` / `wasReleased()` に傾きイベントをOR結合する。**

```cpp
bool MappedInputManager::wasPressed(Button button) const {
  bool result = mapButton(button, &HalGPIO::wasPressed);  // 既存ボタン
  // ジャイロイベントをOR
  if (tiltEnabled && (button == Button::PageForward || button == Button::PageBack)) {
    result |= wasTiltTriggered(button);
  }
  return result;
}
```

- `wasReleased()` も同様にOR結合（`longPressChapterSkip` モードとの互換性）
- ジャイロイベントは `update()` 内で生成し、1フレーム限り有効（次の `update()` でクリア）
- ボタンとジャイロが同時にトリガーされても1回のページ送りとして扱う

**利点:** Reader系Activity（Epub/Txt/Xtc）の `ReaderUtils::detectPageTurn()` がそのまま動作し、変更不要。

#### 画面向きとの連携

端末の物理的な傾き方向は画面向き設定に応じて軸と方向を切り替える必要がある。

| 画面向き | 読み取り軸 | PageForward方向 |
|---------|-----------|----------------|
| Portrait | X軸 | +X（右傾き） |
| Inverted | X軸 | -X（物理的には右傾き、但し上下逆） |
| Landscape CW | Y軸 | +Y |
| Landscape CCW | Y軸 | -Y |

`MappedInputManager` が既に `effectiveOrientation` を保持しているため、この情報を使って軸と方向を切り替える。

### 3. 設定と有効化

**CrossPointSettings に追加:**
```cpp
uint8_t tiltPageTurn = 0;  // 0=OFF, 1=ON, デフォルトOFF
```

**EpubReaderMenuActivity のクイックメニューに項目追加:**
- `MenuAction::TILT_PAGE_TURN` を追加
- ON/OFFトグル型（`STYLE_FIRST_LINE_INDENT` と同じパターン）
- メニュー右側に現在の状態（ON/OFF）を表示
- **X3でのみ項目を表示**（`gpio.deviceIsX3()` で条件分岐）

**TxtReaderActivity / XtcReaderActivity:**
- TxtReaderにはクイックメニューが存在しないため、初期版では非対応
- XtcReaderも同様（章選択のみ）
- ただし、MappedInputManager統合方式のため、これらのReaderでも `tiltPageTurn` 設定がONであればページ送り自体は動作する（メニューからのON/OFF切り替えができないだけ）

**i18n:**
- `STR_TILT_PAGE_TURN` を各言語YAMLに追加
- 日本語: 「傾きでページ送り」
- 英語: 「Tilt to Turn Pages」

### 4. 省電力

- **機能OFF時**: QMI8658の初期化自体をスキップ（I2C通信ゼロ、消費電力追加なし）
- **機能ON時**: `update()` 毎にI2C読み取り1回（4バイト、~50μsのバスタイム）
- **ディープスリープ時**: `HalIMU::standby()` でQMI8658をスタンバイモードに設定
- **設定OFF切り替え時**: 即座にスタンバイモードに移行

### 5. 変更ファイル一覧

| ファイル | 変更種別 | 内容 |
|---------|---------|------|
| `lib/hal/HalIMU.h` | 新規 | QMI8658ドライバ ヘッダ |
| `lib/hal/HalIMU.cpp` | 新規 | QMI8658ドライバ 実装 |
| `src/MappedInputManager.h` | 修正 | HalIMU参照、傾きイベントメンバ追加 |
| `src/MappedInputManager.cpp` | 修正 | 傾き検知ロジック、wasPressed/wasReleasedへのOR結合 |
| `src/CrossPointSettings.h` | 修正 | `tiltPageTurn` 設定項目追加 |
| `src/activities/reader/EpubReaderMenuActivity.h` | 修正 | `MenuAction::TILT_PAGE_TURN` 追加 |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | 修正 | メニュー項目追加、値表示 |
| `src/activities/reader/EpubReaderActivity.cpp` | 修正 | メニュー結果ハンドラに傾き設定切り替え処理 |
| `lib/I18n/translations/*.yaml` | 修正 | `STR_TILT_PAGE_TURN` 翻訳追加 |
| `src/main.cpp` | 修正 | HalIMU初期化呼び出し |

**変更不要なファイル:**
- `src/activities/reader/ReaderUtils.h` — 変更不要
- `src/activities/reader/TxtReaderActivity.cpp` — 変更不要
- `src/activities/reader/XtcReaderActivity.cpp` — 変更不要
- `lib/hal/HalGPIO.h/cpp` — 変更不要（QMI8658定義は既存）

### 6. 検証方法

**ビルド検証:**
- `pio run -t clean && pio run` が0エラー/0警告

**実機検証（X3）:**
- [ ] EPUBリーダーのクイックメニューに「傾きでページ送り」が表示される
- [ ] ONにして左右に傾けるとページが送られる
- [ ] 水平に戻してから再度傾けると再度ページが送られる（1回1ページ）
- [ ] 傾けたまま保持しても連続送りにならない
- [ ] 4方向の画面向きすべてで正しく動作
- [ ] ボタンによるページ送りが引き続き正常に動作
- [ ] OFFにすると傾けてもページが送られない
- [ ] 設定がデバイス再起動後も保持される

**実機検証（X4）:**
- [ ] クイックメニューに傾き項目が表示されない

**メモリ検証:**
- [ ] `ESP.getFreeHeap()` が機能ON/OFFで大差ない（追加RAM < 100B想定）

**省電力検証:**
- [ ] 機能OFF時にI2C通信が発生しないことをシリアルログで確認
