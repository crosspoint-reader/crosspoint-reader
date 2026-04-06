# フォントビルド・リリース自動化設計

## 概要

SDカードフォントのビルド・リリースを自動化し、BIZ UDGothicを含む全フォントをデバイスからダウンロード可能にする。
既存の `release-fonts.yml` ワークフローのトリガーを拡張し、FONT_MANIFEST_URLをリリースアセットに向ける。

## 変更1: release-fonts.yml のトリガー拡張

### 現状
```yaml
on:
  workflow_dispatch:
```

### 変更後
```yaml
on:
  workflow_dispatch:
  push:
    branches: [master]
    paths:
      - 'lib/EpdFont/scripts/sd-fonts.yaml'
```

- `sd-fonts.yaml` が変更されたmaster pushで自動トリガー
- 手動dispatchも引き続き利用可能
- フォント定義以外の変更ではトリガーされない（pathsフィルタ）

## 変更2: FONT_MANIFEST_URL の修正

### 現状（FontDownloadActivity.h）
```cpp
#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://raw.githubusercontent.com/zrn-ns/crosspoint-reader/personal/main/docs/fonts.json"
#endif
```

### 変更後
```cpp
#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://github.com/zrn-ns/crosspoint-reader/releases/download/sd-fonts/fonts.json"
#endif
```

- リリースアセットから取得することで、フォントファイルとマニフェストの整合性を保証
- ブランチ名に依存しなくなる
- GitHub ReleaseのアセットURLはHTTPSリダイレクト経由でダウンロード可能（デバイスのHttpDownloaderで対応済み）

## 変更3: docs/fonts.json の扱い

- リリースアセットが正となるため、リポジトリ内の `docs/fonts.json` は参考ファイルとして残す
- 実際のダウンロードには使用されなくなる

## 既存ワークフローへの影響

| ワークフロー | 影響 |
|---|---|
| `ci.yml` | なし |
| `dev-build.yml` | なし |
| `release.yml` | なし |
| `release_candidate.yml` | なし |
| `release-fonts.yml` | トリガー部分のみ変更 |

## 実装上の注意

- `release-fonts.yml` のビルド・リリース手順自体は変更しない
- `sd-fonts` 固定タグへの上書きリリース方式は既存のまま
- フォントビルドは全フォント対象のため、CJKフォント含め実行時間が長い（数十分）
- `paths` フィルタにより `sd-fonts.yaml` 以外の変更ではトリガーされない
