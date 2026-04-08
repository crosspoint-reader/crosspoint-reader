# CrossPoint JP

Xteink X3/X4 向けの日本語EPUB閲覧に特化したフォーク。

本家 [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) (v1.2.0) をベースに、[CJKフォーク](https://github.com/aBER0724/crosspoint-reader-cjk)の改善を統合し、日本語の読書体験を向上させています。

<img src="./docs/images/cover.jpg" width="400" alt="縦書き表示"> <img src="./docs/images/home.jpg" width="400" alt="ホーム画面">

## 注意事項

- **動作保証はありません。** 自己責任でご利用ください
- 現状 **Xteink X3 でのみ動作確認**しています。X4 での動作は未確認です
- **日本語専用フォークです。** 中国語・韓国語のUIおよびフォントサポートは廃止しています。中国語環境をお使いの方は [CJKフォーク](https://github.com/aBER0724/crosspoint-reader-cjk) をご利用ください

## 主な機能・改善点

### 日本語の読書体験

- **縦書き表示** — `vertical-rl` 対応、句読点・括弧の自動変換
- **日本語フォント** — BIZ UDGothic、NotoSansJP 等をデバイスから直接ダウンロード
- **EPUB内の画像・テーブル** — JPEG/PNG画像やHTMLテーブルを本文中にそのまま表示
- **文字組みの改善** — 段落インデント、濁音の表示修正、縦横独立の行間設定

### 青空文庫

- デバイスからWiFi経由で青空文庫の作品を検索・ダウンロード
- お気に入り作家を登録して、ホーム画面からすぐにアクセス

### Sleep画面

- お気に入りの画像をスリープ画面に設定（WebUIからアップロード）
- 当月カレンダーをオーバーレイ表示（配置位置は設定で変更可能、初回のみWiFi接続による時刻同期が必要）

### その他

- Xteink X3 ハードウェアサポート（4階調グレースケール対応）
- WiFi接続時のNTP時刻自動同期
- 各種バグ修正・安定性改善

## インストール

### Web Flasher（推奨）

**[CrossPoint JP Flasher](https://zrn-ns.github.io/crosspoint-jp/)** からブラウザ上で簡単にインストールできます。

1. XteinkデバイスをUSB-Cケーブルでパソコンに接続
2. Chrome または Edge で [CrossPoint JP Flasher](https://zrn-ns.github.io/crosspoint-jp/) を開く
3. ファームウェア（開発版 / 安定版）を選択し「インストール」をクリック
4. シリアルポート選択ダイアログでデバイスを選択

対応ブラウザ: Chrome 89+, Edge 89+

⚠️後続のセクションに記載の「フォントのインストール」を実施しないと、書籍内の日本語フォントが正常に表示されません⚠️

### 手動インストール

1. [Releases ページ](https://github.com/zrn-ns/crosspoint-jp/releases)から最新の Dev Build を開く
2. `firmware.bin`, `bootloader.bin`, `partitions.bin` をダウンロード
3. Xteink をUSB-Cでパソコンに接続
4. esptool.py または https://xteink.dve.al/ から書き込み

## フォントのインストール

デバイスの **設定 > フォントをダウンロード** から日本語フォントをダウンロードできます（WiFi接続が必要）。
ダウンロードは同じフォント名を2回選択することでインストールが可能です。

### おすすめフォント

| フォント | 特徴 |
|---------|------|
| **BIZ UDGothic** | モリサワのユニバーサルデザインゴシック体。可読性が高く、電子ペーパーとの相性が良い。おすすめ |
| **NotoSansJP** | Google製のゴシック体。文字カバレッジが広い |

ダウンロード後、**設定 > 読書設定 > フォントファミリー** からフォントを選択してください。

## 対応フォーマット

| フォーマット | 拡張子 | 説明 |
|---|---|---|
| **EPUB** | `.epub` | 主要な電子書籍形式。フルサポート（目次、脚注、画像、縦書き等） |
| **XTC/XTCH** | `.xtc`, `.xtch` | Xteink独自形式。事前レンダリング済みページをそのまま表示 |
| **BMP** | `.bmp` | モノクロBMP画像ビューア |

> **注:** TXT/Markdown形式はSDカードフォント使用時のテキストレイアウト計算にパフォーマンス上の問題があるため、現在非対応です。

## おすすめEPUB入手先

- **デバイスから直接ダウンロード**: 設定メニュー > 青空文庫 からWiFi経由で検索・ダウンロード可能
- [青空文庫 EPUB変換](https://aozora.orihasam.com/#index.html) - 著作権切れの日本文学をEPUB形式で入手可能（PC経由）

## 操作方法

詳細な操作方法については [USER_GUIDE.md](./USER_GUIDE.md) を参照してください。

## ベースとなったプロジェクト

| プロジェクト | 内容 |
|---|---|
| [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) | 本家 (v1.2.0 ベース) |
| [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) | CJKフォーク（文字間隔修正、段落インデント等） |
| [PR #1392](https://github.com/crosspoint-reader/crosspoint-reader/pull/1392) | SDカードフォント（22ファミリー、CJK対応） |
| [PR #875](https://github.com/crosspoint-reader/crosspoint-reader/pull/875) | Xteink X3 ハードウェアサポート |

## ライセンス

[MIT License](./LICENSE)
