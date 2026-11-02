# StackChan × Muse

M5Stack CoreS3 のスタックチャンで、Meta の [Muse](https://dev.meta.ai/) と話すためのファームウェアです。

> **開発中です。** 進み具合は [CHANGELOG.md](CHANGELOG.md) を見てください。
> 実際の Muse の応答での確認は、API キーが用意できてからになります。

## できること

| 機能 | 使うもの | 状態 |
| --- | --- | --- |
| 顔・表情・字幕・状態表示 | 画面（m5stack-avatar） | ✅ |
| 首の動き | サーボ（校正値は本体に保存済みのものを使用） | ✅ |
| Muse への接続確認 | Wi-Fi、TLS（証明書を検証） | ✅ |
| 声で読み上げ | スピーカー（内蔵ボイス / VOICEVOX） | ✅ |
| 実画面のスクリーンショット | USB シリアル | ✅ |
| 声で話しかける | マイク → Muse Voice Transcribe | 次の版 |
| Muse と会話 | Muse Spark（Chat Completions） | 次の版 |
| 見せて話す | カメラ → Muse Spark（画像入力） | 次の版 |

### 声について

Muse には文字起こしのモデルはありますが、[音声合成はありません](https://dev.meta.ai/docs/speech-to-text)。
そのため返事の読み上げは本体側で行います。
VOICEVOX サーバーが設定されていて届けばそれを使い、届かなければ内蔵ボイスで喋ります。

## 必要なもの

- M5Stack CoreS3 + スタックチャンのボディ
- 2.4GHz の Wi-Fi
- [Meta Model API](https://dev.meta.ai/) の API キー（`LLM_` で始まるもの）
- 内蔵ボイスで喋るなら、LittleFS のボイスパック
  （[stackchan-llm-mqtt-fft](https://github.com/Xenoah/stackchan-llm-mqtt-fft) を入れていた本体にはすでに入っています）

## セットアップ

```powershell
pip install pyserial pillow

# 書き込み（NVS とボイスパックは消えません）
pio run -t upload

# Wi-Fi（stackchan-llm-mqtt-fft で設定済みなら不要）
python tools/set_wifi.py "ネットワーク名"

# API キー（入力は画面に出ません）
python tools/set_key.py
```

キーは [Model API ダッシュボード](https://dev.meta.ai/) の **API keys → Create API key** で作れます。
キーは本体の NVS に保存され、ログや画面には出しません。
NVS は暗号化していないので、本体を人に渡すときは引数なしの `setkey` でキーを消してください。

## 開発用の道具

| コマンド | 内容 |
| --- | --- |
| `python tools/capture_screen.py out.png` | LCD に出ている画面をそのまま PNG にする |
| `python tools/mock_muse.py` | API キーなしで試すための Model API の代役 |
| `python tools/make_release.py v0.1.0` | 書き込み用ファイルを `.release/` に集める |

USB シリアル（115200bps）で次のコマンドを受け付けます。

`status` / `shot` / `say <文>` / `setkey <キー>` / `wifi <SSID><TAB><パスワード>` / `volume <0-255>` / `model <ID>` / `check` / `nod` / `joy` / `shake` / `calibrate` / `reboot`

`baseurl <URL>` はテスト用に接続先を差し替えます。保存はしないので、再起動すると `api.meta.ai` に戻ります。
既定以外の接続先に本物のキーは送りません。

## 構成

| ファイル | 役割 |
| --- | --- |
| [src/main.cpp](src/main.cpp) | 起動、画面の状態、USB コマンド |
| [src/MuseClient.cpp](src/MuseClient.cpp) | Meta Model API のクライアント |
| [src/FaceHud.cpp](src/FaceHud.cpp) | 顔に重ねる会話 UI |
| [src/Body.cpp](src/Body.cpp) | 首の動き |
| [src/Speech.cpp](src/Speech.cpp) | 読み上げ（VOICEVOX → 内蔵ボイス） |
| [src/ScreenShot.cpp](src/ScreenShot.cpp) | 実画面の読み出し |

顔・サーボ校正・内蔵ボイスは [stackchan-llm-mqtt-fft](https://github.com/Xenoah/stackchan-llm-mqtt-fft) から移植しました。
パーティション構成も同じなので、Wi-Fi 設定・サーボ校正・ボイスパックはそのまま引き継がれます。

使っているオープンソースソフトウェアは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。
本プロジェクトは Meta とは関係ありません。
