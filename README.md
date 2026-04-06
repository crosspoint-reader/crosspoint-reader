# CrossPoint JP

Xteink X3/X4 向けの日本語EPUB閲覧に特化したファームウェアフォーク。

本家 [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) (v1.2.0) をベースに、[CJKフォーク](https://github.com/aBER0724/crosspoint-reader-cjk)の改善を統合し、日本語の読書体験を向上させています。

![](./docs/images/cover.jpg)

## 注意事項

- **動作保証はありません。** 自己責任でご利用ください
- 現状 **Xteink X3 でのみ動作確認**しています。X4 での動作は未確認です
- 公式 CrossPoint に CJK サポートが導入された場合、本フォークはお役御免となる予定です

## 主な機能・改善点

### 縦書き表示

- EPUBの `writing-mode: vertical-rl` に対応
- OpenType `vert` 代替グリフによる句読点・括弧の縦書き変換
- 縦書き/横書きで独立した行間隔設定

### テーブル表示

- EPUB内のHTMLテーブルをグリッドレイアウトで描画
- 本文フォントでテーブルを表示

### 日本語フォント

- SDカードフォント対応（`.cpfont` 形式）
- BIZ UDGothic、NotoSansJP 等の日本語フォントをデバイスからダウンロード可能
- フォントファイルは日本語(JIS X 0213)に絞り、ファイルサイズを最適化

### レンダリング品質の向上

- CJK文字間スペースの修正
- 段落インデント対応
- 仮名のNFC正規化（濁音・半濁音の分離表示修正）

### その他

- Xteink X3 ハードウェアサポート
- カーニング・リガチャ対応（本家 v1.2.0 由来）
- 脚注ナビゲーション（本家 v1.2.0 由来）
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

ダウンロード後、**Settings > Reader > Font** からフォントを選択してください。

## おすすめEPUB入手先

- [青空文庫 EPUB変換](https://aozora.orihasam.com/#index.html) - 著作権切れの日本文学をEPUB形式で入手可能

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
