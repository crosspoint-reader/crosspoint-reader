# CrossPoint Reader CJK

[English](./README.md) | [中文](./README-ZH.md) | **[日本語](./README-JA.md)**

このリポジトリで AI コーディングエージェントを使う場合は、先に [AGENTS.md](./AGENTS.md) を参照してください。

> [daveallie/crosspoint-reader](https://github.com/daveallie/crosspoint-reader) をベースにした **Xteink X4** 電子ペーパーリーダー用ファームウェアの CJK 適用版です。

本プロジェクトは、オリジナルの CrossPoint Reader をベースに CJK 対応を行い、多言語インターフェースと CJK フォントのレンダリングをサポートしています。

![](./docs/images/cover.jpg)

## ✨ CJK 版の新機能

### 🌏 多言語インターフェース対応 (I18n)

- **完全ローカライズ**：中国語、英語、日本語の3言語インターフェースに対応
- 設定からいつでも表示言語の切り替えが可能
- すべてのメニュー、プロンプト、設定項目が完全にローカライズ済み
- 文字列 ID に基づく動的な翻訳システムを採用

### 📝 CJK フォントシステム

- **外部フォント対応**：
  - **読書用フォント**：書籍コンテンツ用 (サイズとフォントファミリーを選択可能)
  - **UI フォント**：メニュー、タイトル、インターフェース用
  - フォント共有オプション：読書用フォントを UI フォントとして使用し、メモリを節約可能
- LRU キャッシュ最適化により、CJK レンダリング性能を向上
- 内蔵 ASCII 文字フォールバック機能により、メモリ使用量を削減
- サンプルフォント：
  - [Source Han Sans CN (思源黒体)](fonts/SourceHanSansCN-Bold_20_20x20.bin) (UI フォント)
  - [King Hwa Old Song (京華老宋体)](fonts/KingHwaOldSong_38_33x39.bin) (読書用フォント)

### 🎨 テーマと表示

- **Lyra テーマ**：角丸選択ハイライト、スクロールバーページネーション、洗練されたレイアウトを備えたモダンな UI テーマ。すべての CJK ページに完全対応
- **ダーク/ライトモード切り替え**：リーダー画面と UI 両方に適用
- 画面リフレッシュを行わない滑らかなテーマ切り替え

### 📖 読書レイアウト

- **字下げ**：CSS `text-indent` による段落インデントのオン/オフ切り替え
- CJK 文字の実際の幅に基づいて字下げ幅を計算
- **ストリーミング CSS パーサー**：大きなスタイルシートでもメモリ不足になりません
- **CJK 文字間隔修正**：隣接する CJK 文字間の余分なスペースを除去

## 📦 機能一覧

- [x] EPUB 解析とレンダリング (EPUB 2 および EPUB 3)
- [x] EPUB 画像の alt テキスト表示
- [x] TXT テキストファイル読書対応
- [x] 読書進捗の保存
- [x] ファイルブラウザ (ネストされたフォルダに対応)
- [x] カスタムスリープ画面 (表紙表示対応)
- [x] KOReader 読書進捗同期
- [x] WiFi ファイルアップロード
- [x] WiFi OTA ファームウェア更新
- [x] WiFi 認証情報管理 (Web UI からスキャン、保存、削除)
- [x] AP モードの改善 (Captive Portal 対応)
- [x] ダーク/ライトモード切り替え
- [x] 段落の字下げ
- [x] 多言語ハイフネーション対応
- [x] フォント、レイアウト、表示スタイルのカスタマイズ
  - [x] 外部フォントシステム (読書 + UI フォント)
- [x] 画面回転 (UI/リーダーの方向を個別設定)
  - [x] 読書画面の回転 (縦画面、横画面 CW/CCW、反転)
  - [x] UI 画面の回転 (縦画面、反転)
- [x] Calibre ワイヤレス接続と Web ライブラリ統合
- [x] 表紙画像表示
- [x] Lyra テーマ (すべての CJK ページに対応)
- [x] KOReader 読書進捗同期
- [x] ストリーミング CSS パーサー (大型 EPUB スタイルシートのメモリ不足を防止)

詳細な操作説明については、[ユーザーガイド](./USER_GUIDE.md) をご参照ください。

## 📥 インストール方法

### Web 書き込み (推奨)

1. USB-C ケーブルで Xteink X4 を PC に接続します。
2. https://xteink-flasher-cjk.vercel.app/ にアクセスし、**「Flash CrossPoint CJK firmware」** をクリックして最新の CJK ファームウェアを直接書き込みます。

> **ヒント**：公式ファームウェアやオリジナル CrossPoint ファームウェアへの復元も同じページで行えます。起動パーティションの切り替えは https://xteink-flasher-cjk.vercel.app/debug をご利用ください。

### 手動ビルド

#### 必須環境

* **PlatformIO Core** (`pio`)
* Python 3.8以上
* USB-C ケーブル
* Xteink X4

#### コードの取得

```bash
git clone --recursive https://github.com/aBER0724/crosspoint-reader-cjk

# クローン後にサブモジュールが不足している場合
git submodule update --init --recursive
```

#### ビルドと書き込み

デバイスを接続した後、以下を実行します：

```bash
pio run --target upload
```

## 🔠 フォント関連

### フォント生成

- `tools/generate_cjk_ui_font.py`

- [DotInk (点墨)](https://apps.apple.com/us/app/dotink-eink-assistant/id6754073002) (推奨：DotInk を使用して読書用フォントを生成すると、デバイス上でのプレビュー効果が確認しやすくなります)

### フォント設定

1. SD カードのルートディレクトリに `fonts/` フォルダを作成します。
2. `.bin` 形式のフォントファイルをフォルダに入れます。
3. 設定画面で「リーダー/リーダーフォント」または「ディスプレイ/UI フォント」を選択します。

**フォントファイル命名規則**：`FontName_size_WxH.bin`

例：
- `SourceHanSansCN-Medium_20_20x20.bin` (UI用: 20pt, 20x20)
- `KingHwaOldSong_38_33x39.bin` (読書用: 38pt, 33x39)

**フォントの説明**：
- **読書用フォント**：書籍の本文テキストに使用されます。
- **UI フォント**：メニュー、タイトル、インターフェース要素に使用されます。

> メモリの制約上、内蔵フォントには非常に小さな中国語文字セット（UI 表示に必要な文字のみ）が含まれています。  
> より良い使用体験を得るために、より完全な UI フォントと読書用フォントを SD カードに保存することを推奨します。  
> フォントの生成が難しい場合は、上記で提供されているサンプルフォントをダウンロードして使用してください。

## ℹ️ よくある質問 (FAQ)

1. ページ数の多い章は、初回オープン時にインデックス作成に時間がかかる場合があります。ストリーミング CSS パーサーにより大幅に改善されましたが、非常に大きな章では数秒かかることがあります。
    > 再起動してもインデックス作成画面で止まり、長時間完了せず他のページに戻れない場合は、ファームウェアを再書き込みしてください。
2. 特定の画面でフリーズした場合は、デバイスを再起動してみてください。
3. ESP32-C3 のメモリは非常に限られています。大きな CJK フォントファイルを UI フォントと読書用フォントの両方に同時使用すると、メモリ不足でクラッシュする場合があります。UI フォントは 20pt 以下を推奨します。
4. 新しい本を追加した後、初めてホーム画面を開くと、表紙のサムネイルが生成されます。「読み込み中」のポップアップが数秒間表示されることがありますが、これは正常な動作であり、フリーズではありません。

## 🤝 コントリビューション

初めてこのリポジトリに参加する場合は、まず [Contributing Docs](./docs/contributing/README.md) をご確認ください。

PR を作成する前に、ローカルで以下のチェックを実行することを推奨します。

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

ハイフネーション関連の変更を行った場合は、追加で次を実行してください。

```sh
./test/run_hyphenation_eval.sh english
```

- PR タイトルは semantic 形式を推奨し、`.github/PULL_REQUEST_TEMPLATE.md` を記入してください。
- AI コーディングエージェントを使う場合は、[AGENTS.md](./AGENTS.md) も参照してください。

## 📜 謝辞

- [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) - オリジナルプロジェクト
- [open-x4-sdk](https://github.com/daveallie/open-x4-sdk) - Xteink X4 開発 SDK

---

**免責事項**：本プロジェクトは Xteink や X4 ハードウェア製造元とは無関係であり、コミュニティによるプロジェクトです。
