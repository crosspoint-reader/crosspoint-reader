# 青空文庫ダウンロード機能 設計書

**Issue**: zrn-ns/crosspoint-jp#16
**日付**: 2026-04-07
**ステータス**: 承認済み

## 概要

デバイスからWiFi経由で青空文庫の書籍をEPUB形式でダウンロードできる機能を開発する。バックエンドはVercel Serverless Functionsでホストし、オンデマンドでTXT→EPUB変換を行う。

## アーキテクチャ

```
┌──────────────┐     HTTPS/JSON      ┌─────────────────────┐     HTTP      ┌───────────────┐
│  ESP32-C3    │ ◄──────────────────► │  Vercel API         │ ◄──────────► │  青空文庫     │
│  デバイス     │  メタデータ/EPUB     │  (Serverless Func)  │   TXT取得    │  サーバー      │
│              │                      │                     │              │               │
│  /Aozora/    │                      │  ・作家/作品一覧API  │              │  ・ZIPファイル  │
│  (SD Card)   │                      │  ・EPUB変換API      │              │  ・CSVインデックス│
└──────────────┘                      └─────────────────────┘              └───────────────┘
```

**方式選定理由**: ページネーションAPI型（アプローチC）を採用。
- ESP32-C3の380KB RAM制約に最適（1リクエスト数KBのJSON）
- 19,000作品のマニフェスト一括取得はメモリ的に非現実的
- E-Inkの画面遷移が1-2秒かかるため、API呼び出し待ちは相対的に目立たない

## Vercel API設計

### リポジトリ構成

```
aozora-epub-api/                  ← 新規リポジトリ (zrn-ns/aozora-epub-api)
├── api/
│   ├── authors.ts                ← 作家一覧API
│   ├── works.ts                  ← 作品一覧API（複数検索軸対応）
│   └── convert.ts               ← EPUB変換API
├── lib/
│   ├── aozora-index.ts           ← CSVインデックス読み込み・検索
│   ├── aozora-parser.ts          ← 青空文庫注記パーサー
│   ├── epub-builder.ts           ← EPUB3生成
│   └── chapter-splitter.ts       ← 見出しベース章分割
├── vercel.json
├── package.json
└── tsconfig.json
```

**技術スタック**: Node.js / TypeScript（Vercel Serverless Functions）

### データソース

青空文庫公式CSV（`list_person_all_extended_utf8.zip`）を起動時にフェッチしメモリに保持。Cold startで数秒かかるが初回のみ。

### エンドポイント

#### `GET /api/authors?kana_prefix={カタカナ行}`

作家一覧を50音の行でフィルタして返す。

**リクエスト例**: `GET /api/authors?kana_prefix=ア`

**レスポンス**:
```json
{
  "authors": [
    { "id": 23, "name": "芥川龍之介", "kana": "アクタガワ リュウノスケ", "work_count": 371 },
    { "id": 148, "name": "有島武郎", "kana": "アリシマ タケオ", "work_count": 65 }
  ]
}
```

#### `GET /api/works`

複数の検索軸に対応する作品一覧API。

| パラメータ | 用途 | 例 |
|-----------|------|-----|
| `author_id={id}` | 作家IDで絞り込み | `?author_id=23` |
| `kana_prefix={カタカナ行}` | 作品名50音検索 | `?kana_prefix=ラ` |
| `ndc={分類番号}` | ジャンル検索 | `?ndc=913` |
| `sort=newest&limit={N}` | 新着作品 | `?sort=newest&limit=50` |

**レスポンス**:
```json
{
  "works": [
    { "id": 456, "title": "羅生門", "kana": "ラショウモン", "ndc": "913" },
    { "id": 789, "title": "鼻", "kana": "ハナ", "ndc": "913" }
  ]
}
```

#### `GET /api/convert?work_id={id}`

指定作品をEPUB3に変換してバイナリ返却。

**レスポンスヘッダー**:
```
Content-Type: application/epub+zip
Content-Disposition: attachment; filename="456_rashomon.epub"
```

**変換フロー**:
1. CSVインデックスからZIP URL取得
2. 青空文庫サーバーからZIPダウンロード
3. ZIP内のTXTファイルを抽出（Shift_JIS → UTF-8変換）
4. 青空文庫注記パース
5. 見出し注記で章分割
6. EPUB3構造生成
7. ZIPに固めてレスポンス返却

### エラーレスポンス

```json
{
  "error": "WORK_NOT_FOUND",
  "message": "指定された作品が見つかりません"
}
```

| HTTPステータス | error コード | 原因 |
|--------------|-------------|------|
| 400 | `INVALID_PARAMS` | パラメータ不正 |
| 404 | `WORK_NOT_FOUND` | 作品ID不存在 |
| 502 | `AOZORA_FETCH_FAILED` | 青空文庫サーバーからの取得失敗 |
| 500 | `CONVERSION_FAILED` | EPUB変換中のエラー |

### キャッシュ戦略

| エンドポイント | Cache-Control | 理由 |
|--------------|---------------|------|
| authors, works | `s-maxage=86400`（24時間） | 青空文庫の更新頻度は低い |
| convert | `s-maxage=604800`（7日間） | 同一作品の再変換を避ける |

### 認証

なし（公開API）。Vercelのレート制限に依存。

## デバイス側設計

### Activity構成

`AozoraActivity`（単一Activity + ステートマシン方式）:

```
SettingsActivity
  └── [ACTION: "青空文庫"] → AozoraActivity 起動

AozoraActivity
  ├── state: TOP_MENU
  │   ├── "作家から探す"      → KANA_SELECT (mode=AUTHOR)
  │   ├── "作品名から探す"    → KANA_SELECT (mode=TITLE)
  │   ├── "ジャンルから探す"  → GENRE_SELECT
  │   ├── "新着作品"          → WORK_LIST (newest)
  │   └── "ダウンロード済み"  → DOWNLOADED_LIST
  │
  ├── state: KANA_SELECT
  │   └── あ行〜わ行 選択 → API → AUTHOR_LIST or WORK_LIST
  │
  ├── state: GENRE_SELECT
  │   └── 小説/詩歌/随筆... 選択 → API → WORK_LIST
  │
  ├── state: AUTHOR_LIST
  │   └── 作家選択 → API → WORK_LIST
  │
  ├── state: WORK_LIST
  │   └── 作品選択 → WORK_DETAIL
  │
  ├── state: WORK_DETAIL
  │   ├── "ダウンロード" → DOWNLOADING
  │   └── (済みなら) "削除" → 確認 → 削除実行
  │
  ├── state: DOWNLOADING
  │   └── 進捗表示 → 完了 → WORK_DETAIL (済みマーク)
  │
  ├── state: DOWNLOADED_LIST
  │   └── 作品選択 → WORK_DETAIL
  │
  └── state: ERROR
      └── エラーメッセージ → Back → 前の状態
```

**単一Activity方式の理由**:
- Activity間遷移のheap allocate/deleteによるRAM負荷を回避
- WiFi接続の維持が容易
- 状態間の「戻る」操作がシンプル（stateスタック）

### WiFi接続フロー

```
AozoraActivity::onEnter()
  → WiFi.mode(WIFI_STA)
  → WifiSelectionActivity をsubactivityとして起動
  → 接続成功 → onWifiSelectionComplete()
  → フォントキャッシュ解放（TLSバッファ用メモリ確保）
  → state: TOP_MENU
```

### メモリ管理

- API応答JSONは `HttpDownloader::fetchUrl()` で取得（数KB）
- パース後のリストは `std::vector` に格納、状態遷移時に `clear()` + 新データ
- EPUBダウンロードは `HttpDownloader::downloadToFile()` でSD直書き（RAM蓄積なし）

### ダウンロード管理

**保存先**: `/Aozora/` ディレクトリ

**ファイル名**: `{work_id}_{タイトル}.epub`（例: `456_羅生門.epub`）
- work_idプレフィクスで一意性保証
- FAT32制約に合わせてサニタイズ（禁止文字除去、長さ制限）

**管理インデックス**: `/Aozora/.aozora_index.json`
```json
[
  { "work_id": 456, "title": "羅生門", "author": "芥川龍之介", "filename": "456_羅生門.epub" }
]
```

**重複防止**: WORK_DETAIL表示時にwork_idがインデックスに存在すれば「ダウンロード済み」表示

**インデックス整合性**: Activity起動時（WiFi接続前）にインデックスを読み込み、各エントリのファイル存在をチェック。SD上に存在しないエントリは自動パージして書き戻す。

### デバイス側エラーハンドリング

| 状況 | 対応 |
|------|------|
| HTTP通信エラー | "サーバーに接続できません" → Back で前の状態 |
| 4xx/5xx応答 | errorフィールドのmessageを表示 → Back で前の状態 |
| ダウンロード途中エラー | 不完全ファイル削除 → エラー表示 |
| WiFi切断 | 接続チェック → 再接続試行 → 失敗なら ERROR state |

## EPUB変換ロジック

### 青空文庫注記パーサー対応範囲

**Phase 1（初回リリース）**:

| 注記 | 記法例 | EPUB変換 |
|------|--------|---------|
| ルビ | `漢字《かんじ》` | `<ruby>漢字<rt>かんじ</rt></ruby>` |
| 見出し | `［＃大見出し］`〜`［＃大見出し終わり］` | `<h1>`〜`<h3>` + 章分割ポイント |
| 字下げ | `［＃３字下げ］` | `text-indent: 3em` |
| 傍点 | `［＃「○○」に傍点］` | `<em class="sesame">` |
| 改ページ | `［＃改ページ］` | 新セクションXHTML |
| 底本注記 | 末尾の底本情報ブロック | 最終セクションとして保持 |

**Phase 2（将来）**:
- 割り注、縦中横、罫囲み等の高度な注記
- テキストパターンによるヒューリスティック章分割（「第○章」「その一」等）

### 章分割ロジック

1. 見出し注記（大見出し/中見出し/小見出し）を検出 → 章区切りポイント
2. `［＃改ページ］` を検出 → 章区切りポイント
3. 章区切りが0個の場合 → 単一セクションEPUBとして生成
4. 各章を個別XHTMLファイルに分割（`chapter_001.xhtml`, `chapter_002.xhtml`, ...）

### EPUB3出力構成

```
book.epub (ZIP)
├── mimetype                    (非圧縮、先頭固定)
├── META-INF/
│   └── container.xml
├── OEBPS/
│   ├── content.opf             (メタデータ: タイトル、作家名、言語=ja)
│   ├── nav.xhtml               (EPUB3目次: 章タイトル一覧)
│   ├── style.css               (最小限: ルビ、傍点、字下げ)
│   ├── chapter_001.xhtml
│   ├── chapter_002.xhtml
│   └── ...
└──
```

## 設定メニュー統合

`SettingsList.h` に ACTION型メニュー項目を追加:

```cpp
SettingInfo::Action(STR_AOZORA_BUNKO, SettingAction::AozoraBunko)
```

STR_CAT_SYSTEM カテゴリに配置（フォントダウンロードの隣）。

## i18n

以下の翻訳キーを `lib/I18n/translations/` のYAMLファイルに追加:

- `STR_AOZORA_BUNKO`: "青空文庫"
- `STR_SEARCH_BY_AUTHOR`: "作家から探す"
- `STR_SEARCH_BY_TITLE`: "作品名から探す"
- `STR_SEARCH_BY_GENRE`: "ジャンルから探す"
- `STR_NEWEST_WORKS`: "新着作品"
- `STR_DOWNLOADED_BOOKS`: "ダウンロード済み"
- `STR_ALREADY_DOWNLOADED`: "ダウンロード済み"
- `STR_DOWNLOAD_CONFIRM`: "ダウンロードしますか？"
- `STR_DELETE_CONFIRM`: "削除しますか？"
- `STR_DOWNLOADING_PROGRESS`: "ダウンロード中..."
- `STR_CONNECTION_FAILED`: "サーバーに接続できません"
- その他エラーメッセージ

## Done 判定基準

- [ ] Vercel APIが3エンドポイント全て動作する（authors, works, convert）
- [ ] 青空文庫注記パーサーがPhase 1の全注記を正しく変換する
- [ ] 生成EPUBがデバイスのEpubリーダーで正常に表示される
- [ ] デバイスから作家/作品名/ジャンル/新着の全検索軸で書籍を検索できる
- [ ] EPUBダウンロード→SD保存→HomeActivityからの開閲が動作する
- [ ] 重複ダウンロード防止が機能する
- [ ] インデックス整合性チェック（存在しないファイルのパージ）が動作する
- [ ] 削除機能が動作する
- [ ] WiFi切断・APIエラー時に適切なエラー表示が行われる
※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
