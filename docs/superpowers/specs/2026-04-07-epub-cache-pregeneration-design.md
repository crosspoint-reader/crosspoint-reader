# EPUB全キャッシュ事前生成 設計Spec

## 概要

キャッシュがない状態でEPUBを開いた時、ユーザーに確認ダイアログを表示し、承認されたら全セクション＋全画像のキャッシュを一括生成する。プログレスバーで進捗を表示。

## 動機

現状、セクション（章のレイアウト）と画像（JPEG→BMP変換）のキャッシュはページ遷移時にオンデマンド生成される。各セクションの生成に数秒、画像変換に10-20秒かかるため、初回の読書体験が著しく悪い。全キャッシュを事前に生成すれば、以降の閲覧が全ページ高速になる。

## フロー

```
本を開く → Epub::load() → book.bin作成
  ↓
sectionsディレクトリが空 or 存在しない？
  ├─ No → 通常のリーダー画面へ（キャッシュ済み）
  └─ Yes ↓
確認ダイアログ「キャッシュを生成しますか？」
  ├─ キャンセル → 通常のリーダー画面へ（オンデマンド生成）
  └─ 承認 ↓
全spine itemsをループ (i = 0 .. spineCount-1):
  1. Section::createSectionFile() — セクションキャッシュ生成
  2. 生成されたPageImage内のJPEGファイルに対してBMP変換 — 画像キャッシュ生成
  3. プログレスバー更新: (i+1) / spineCount
  4. セクションオブジェクトを破棄（メモリ解放）
  ↓
完了 → 通常のリーダー画面へ
```

## 実装箇所

### キャッシュ有無の判定

`EpubReaderActivity::onEnter()` 内、最初のsection読み込みの前に判定。`sectionsDir` が空かどうかで判定する。

### 確認ダイアログ

既存の `ConfirmationActivity` を使用。メッセージ: 「キャッシュを生成しますか？（初回のみ）」

### キャッシュ生成ループ

`EpubReaderActivity` に新メソッド `pregenerateCache()` を追加。

```cpp
void EpubReaderActivity::pregenerateCache() {
  const int spineCount = epub->getSpineItemsCount();
  for (int i = 0; i < spineCount; i++) {
    // プログレスバー描画
    renderPregenerateProgress(i, spineCount);

    // セクション生成（既存のcreateSection相当のロジック）
    Section section(epub, i, renderer);
    if (!section.loadSectionFile(...)) {
      section.createSectionFile(...);
    }

    // 画像キャッシュ: セクション内のPageImageを走査し、
    // JPEG画像のBMPキャッシュが未生成なら生成
    // （ImageBlock::renderの初回パスと同等のロジック）

    // メモリ解放（Sectionデストラクタで自動）
  }
}
```

### プログレスバー表示

セクション生成ごとに画面を更新。既存の `renderer.fillRect` でプログレスバーを描画。

```
┌─────────────────────────────┐
│                             │
│    キャッシュを生成中...      │
│                             │
│    ████████░░░░░░  5/13     │
│                             │
└─────────────────────────────┘
```

- バー: `fillRect` で塗りつぶし幅を `(i / spineCount) * barWidth` で計算
- テキスト: `drawText` で「キャッシュを生成中...」と「N/M」を表示
- E-inkのため `FAST_REFRESH` で更新（ゴースト許容）

### 画像キャッシュの生成

セクション作成後、セクションファイルを読み込んでPageImage要素を走査する代わりに、SDカード上の画像ファイル（`img_<spine>_<n>.jpg`）を直接チェックし、対応する `.pxc.bmp` がなければ `JpegToBmpConverter` で変換する。

```cpp
// セクション生成後、画像キャッシュを生成
const std::string imgPrefix = epub->getCachePath() + "/img_" + std::to_string(i) + "_";
for (int j = 0; ; j++) {
  std::string jpgPath = imgPrefix + std::to_string(j) + ".jpg";
  if (!Storage.exists(jpgPath.c_str())) break;

  std::string bmpCachePath = jpgPath.substr(0, jpgPath.rfind('.')) + ".pxc.bmp";
  if (Storage.exists(bmpCachePath.c_str())) continue;

  // BMP変換
  FsFile jpegFile, bmpFile;
  if (Storage.openFileForRead("PRE", jpgPath, jpegFile) &&
      Storage.openFileForWrite("PRE", bmpCachePath, bmpFile)) {
    JpegToBmpConverter::jpegFileToBmpStreamWithSize(jpegFile, bmpFile, maxWidth, maxHeight);
    jpegFile.close();
    bmpFile.close();
  }
}
```

注意: 画像のmaxWidth/maxHeightはセクション生成時のviewport寸法と一致させる必要がある。

## メモリ考慮

- 各セクション生成後にSectionオブジェクトを破棄し、ヒープを回復
- 画像変換は1ファイルずつ処理（picojpegのメモリ使用量は~10KB）
- CSSパーサーは全セクションで共有（`epub->getCssParser()`）

## キャンセル時の動作

確認ダイアログでキャンセルした場合、現在と同じオンデマンド生成に戻る。本の読み込み自体はブロックしない。

## i18n

- 確認ダイアログ: `STR_GENERATE_CACHE` （「キャッシュを生成しますか？」）
- プログレス: `STR_GENERATING_CACHE` （「キャッシュを生成中...」）

## Done判定基準

- [ ] キャッシュがない状態で本を開くと確認ダイアログが表示される
- [ ] 承認後、全セクション＋画像のキャッシュが生成される
- [ ] プログレスバーが進捗を正しく表示する
- [ ] キャンセル時は従来通りオンデマンド生成で動作する
- [ ] 生成完了後、全ページが高速に表示される
- [ ] メモリ不足でクラッシュしない
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
