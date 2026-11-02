# Changelog

## v0.1.0 - 2026-11-02

土台の版です。まだ話しかけることはできません。

### 追加

- [stackchan-llm-mqtt-fft](https://github.com/Xenoah/stackchan-llm-mqtt-fft) の顔・サーボ校正・内蔵ボイスを移植
- 状態を色と文字で伝える会話 UI（状態チップ、モデル名、Wi-Fi、字幕）
- Meta Model API への接続確認（DigiCert Global Root G2 で証明書を検証）
- USB からの Wi-Fi・API キー設定（`tools/set_wifi.py`、`tools/set_key.py`）
- LCD の実画面を読み出すスクリーンショット（`tools/capture_screen.py`）
- API キーなしで試すためのモックサーバー（`tools/mock_muse.py`）

### 実機での確認

CoreS3 + スタックチャンのボディで確認しました。

| 項目 | 結果 |
| --- | --- |
| NVS の引き継ぎ（Wi-Fi、サーボ校正） | ✅ 設定し直さずに動作 |
| ボイスパックの引き継ぎ | ✅ 982 クリップを認識 |
| `api.meta.ai` への TLS 接続（証明書の検証あり） | ✅ キー未設定で `401 invalid_api_key` が返る |
| 接続できたときの流れ | ✅ **モックサーバー**で確認 |
| 読み上げ | ⚠️ 再生の完了までを確認（音そのものは未確認） |
| サーボ | ⚠️ 起動と命令の受け付けを確認（動きそのものは未確認） |
| スクリーンショットの色 | ✅ 赤・緑・青を描いて読み戻して確認 |
| 本物の API キーでの動作 | ❌ 未確認 |
