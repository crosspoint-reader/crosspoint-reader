# FontsPage フォントアップロードのWebSocket化

## 概要

FontsPageのフォントアップロードをHTTP POST方式からWebSocket方式に切り替える。
ESP32-C3（380KB RAM）で6-13MBの `.cpfont` ファイルをアップロードすると、
HTTP POSTではメモリ不足で接続が切断される（"Upload error: Load failed"）。

FilesPageで実績のあるWebSocketアップロードインフラを活用する。

## 背景

- 現状: FontsPageは `fetch` + `FormData` でHTTP POSTアップロード
- FilesPage: WebSocket方式（4KBチャンク + バックプレッシャー制御）で大ファイルも安定動作
- サーバー側WebSocketハンドラは任意パスへのファイル書き込みに対応済み

## 変更箇所

### 1. クライアント側（FontsPage.html）

- `fetch` + `FormData` による `/api/fonts/upload` POST を廃止
- WebSocket接続で4KBチャンク送信（FilesPageと同じプロトコル）
- 送信先パス: `/.crosspoint/fonts/<family>/`
- 進捗表示: `Uploading... XX%`
- アップロード完了後に `GET /api/fonts/uploaded?family=<name>&file=<filename>` でバリデーション＆レジストリ更新

### 2. サーバー側（CrossPointWebServer.cpp / .h）

- 新エンドポイント: `GET /api/fonts/uploaded?family=<name>&file=<filename>`
  - `.cpfont` マジックバイト検証（ファイル先頭8バイト）
  - 検証失敗: ファイル削除 + エラーJSON返却
  - 検証成功: `sdFontSystem.registry().discover()` でレジストリ更新 + 成功JSON返却
- 削除: `/api/fonts/upload` HTTP POSTエンドポイント
- 削除: `FontUploadState` 構造体（不要になる）
- 削除: `handleFontUpload()` / `handleFontUploadData()` メソッド

### 3. 既存WebSocketハンドラ（変更なし）

`onWebSocketEvent` は任意パスにファイル書き込み可能。変更不要。

## データフロー

```
FontsPage JS
  → WebSocket: START:<filename>:<size>:/.crosspoint/fonts/<family>/
  ← READY
  → [4KB chunks...]
  ← DONE
  → fetch GET /api/fonts/uploaded?family=<name>&file=<filename>
  ← {ok: true} or {error: "..."}
```

## Done 判定基準

- [ ] FontsPageから6MB以上の `.cpfont` ファイルをアップロードできる
- [ ] アップロード中に進捗表示される
- [ ] 不正なファイルアップロード時にエラーが返る
- [ ] アップロード後にフォントレジストリが更新される
- [ ] HTTP POST関連の旧コード（FontUploadState, handleFontUpload, handleFontUploadData）が削除されている
- [ ] `pio run` でビルドが通る
- ※ 必須ゲート（動作検証・既存機能・差分確認・シークレット）は常に適用
