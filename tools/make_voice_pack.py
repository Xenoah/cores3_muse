"""StackChan 内蔵ボイスパック（data/voice.pak）を VOICEVOX で生成する。

StackChan を TTS サーバーなしで喋らせるため、実況に出てくるセリフの断片・数字・
「数字＋単位」を一度だけ VOICEVOX で合成し、本体のフラッシュ（LittleFS）へ焼き込む。
本体（BuiltinVoice）は実況文をこの辞書で最長一致に分解してつなげて再生する。

セリフ断片は src/PrintCommentator.cpp と src/PrinterState.cpp の文字列リテラルから
自動抽出するので、実況の文面を変えたらこのスクリプトを再実行すればよい。

使い方:
    1. VOICEVOX（またはエンジン run.exe）を起動しておく
    2. python tools/make_voice_pack.py
    3. pio run -t uploadfs      # data/ を LittleFS として書き込む

パック形式（リトルエンディアン）:
    "SCVP" u16:version u8:codec u8:0 u32:sample_rate u32:count u32:index_bytes
    index: count 回 [u8:key_len key u32:data_offset u32:samples i16:predictor u8:step u8:0]
    data : codec=0 IMA-ADPCM 4bit（下位ニブルが先）/ codec=1 G.711 μ-law 8bit

既定は μ-law 12kHz（約 9MB）。IMA-ADPCM 16kHz は半分の大きさだが、
VOICEVOX の声では SNR が 15dB 前後まで落ちてザラつくため選択式にしている。

キーの種類:
    断片     そのままの文字列（例: "印刷スタート！"）
    #N       数字（例: "#200" = にひゃく）
    =N単位   連濁・促音を含む数字＋単位（例: "=1時間", "=45%", "=23分"）
    カナ / カナ / カナ
             1モーラの音（低い音程 / 高い音程 / 母音の無声化）。本体はこれをつなげて
             辞書（dict/）で読みとアクセントを付けた任意の日本語・英語を喋る。
             「ア＋モーラ＋同じ母音」を平らな音程で合成し、真ん中のモーラだけを切り出す
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import math
import pathlib
import re
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ["src/PrintCommentator.cpp", "src/PrinterState.cpp"]

# ソースに無いが実況で読む語（AMS の材質、ジョブ名の代わり、テスト用のあいさつ）
EXTRA_FRAGMENTS = [
    "作品",
    "PLA", "PETG", "ABS", "ASA", "TPU", "PC", "PA", "PVA", "HIPS", "PET",
    "PLA-CF", "PETG-CF", "PA-CF", "PET-CF", "PAHT-CF", "PA6-CF", "PPA-CF", "PLA-S", "PA-S",
    "こんにちは", "ありがとう", "おはよう", "おやすみ", "よろしくね", "スタックチャンだよ",
    "テスト", "聞こえる？",
]

# 連濁・促音が起きる「数字＋単位」は丸ごと合成しておく
UNIT_RANGES = {
    "%": range(0, 101),
    "分": range(1, 60),
    "時間": range(1, 49),
    "時": range(0, 24),
    "層": range(1, 100),
    "件": range(1, 11),
}

JAPANESE = re.compile(r"[\u3040-\u30ff\u4e00-\u9fff\uff66-\uff9f]")
FORMAT_SPEC = re.compile(r"%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|h)?[diuoxXsfcp]")
TOKEN = re.compile(r'("(?:\\.|[^"\\])*")|(//[^\n]*)|(/\*.*?\*/)', re.S)
LEADING_PUNCT = "。、！？!?,"


def extract_fragments() -> list[str]:
    """ソースの文字列リテラルから日本語を含むセリフ断片を集める（コメントは除外）。"""
    found: set[str] = set()
    for rel in SOURCES:
        text = (ROOT / rel).read_text(encoding="utf-8")
        for match in TOKEN.finditer(text):
            literal = match.group(1)
            if not literal:
                continue  # コメント
            value = bytes(literal[1:-1], "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
            value = value.strip()
            if not value or not JAPANESE.search(value) or FORMAT_SPEC.search(value):
                continue
            found.add(value)
    return sorted(found)


def build_entries() -> dict[str, str]:
    """キー → VOICEVOX に渡す読み上げテキスト"""
    entries: dict[str, str] = {}
    for fragment in extract_fragments() + EXTRA_FRAGMENTS:
        speech = fragment.lstrip(LEADING_PUNCT) or fragment
        entries[fragment] = speech
    numbers = list(range(0, 100)) + [h * 100 for h in range(1, 10)] + [t * 1000 for t in range(1, 10)]
    for n in numbers:
        entries[f"#{n}"] = str(n)
    for unit, values in UNIT_RANGES.items():
        for n in values:
            entries[f"={n}{unit}"] = f"{n}{unit}"
    return entries


def http_post(url: str, data: bytes | None, content_type: str | None) -> bytes:
    request = urllib.request.Request(url, data=data or b"", method="POST")
    if content_type:
        request.add_header("Content-Type", content_type)
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def synthesize(base: str, speaker: int, text: str, speed: float, rate: int) -> list[int]:
    """VOICEVOX で合成し、16bit モノラル PCM サンプル列を返す。"""
    query_url = f"{base}/audio_query?speaker={speaker}&text={urllib.parse.quote(text)}"
    query = json.loads(http_post(query_url, None, None))
    query["speedScale"] = speed
    query["prePhonemeLength"] = 0.04
    query["postPhonemeLength"] = 0.08
    query["outputSamplingRate"] = rate
    query["outputStereo"] = False
    wav_bytes = http_post(f"{base}/synthesis?speaker={speaker}",
                          json.dumps(query).encode("utf-8"), "application/json")
    with wave.open(io.BytesIO(wav_bytes)) as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() != 1 or wav.getframerate() != rate:
            raise RuntimeError(f"unexpected wav format for {text!r}")
        raw = wav.readframes(wav.getnframes())
    return list(struct.unpack(f"<{len(raw) // 2}h", raw))


def trim(samples: list[int], rate: int, threshold: int = 250) -> list[int]:
    """前後の無音を詰める（つなぎ目の間延びを防ぐ）。前 15ms・後 40ms は残す。"""
    first = next((i for i, s in enumerate(samples) if abs(s) > threshold), 0)
    last = next((i for i in range(len(samples) - 1, -1, -1) if abs(samples[i]) > threshold), len(samples) - 1)
    start = max(0, first - rate * 15 // 1000)
    end = min(len(samples), last + rate * 40 // 1000)
    return samples[start:end] if end > start else samples


STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples: list[int]) -> bytes:
    """IMA-ADPCM（初期値 predictor=0, index=0）でエンコードする。"""
    predictor = 0
    index = 0
    out = bytearray((len(samples) + 1) // 2)
    for i, sample in enumerate(samples):
        step = STEP_TABLE[index]
        diff = sample - predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        # デコーダと同じ計算で予測値を更新する（誤差が積もらないように）
        step = STEP_TABLE[index]
        d = step >> 3
        if code & 4:
            d += step
        if code & 2:
            d += step >> 1
        if code & 1:
            d += step >> 2
        predictor = predictor - d if code & 8 else predictor + d
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + INDEX_TABLE[code]))
        if i % 2 == 0:
            out[i // 2] = code
        else:
            out[i // 2] |= code << 4
    return bytes(out)


MULAW_BIAS = 0x84
MULAW_CLIP = 32635
MULAW_EXP = [0, 0, 1, 1] + [2] * 4 + [3] * 8 + [4] * 16 + [5] * 32 + [6] * 64 + [7] * 128


def mulaw_encode(samples: list[int]) -> bytes:
    """G.711 μ-law でエンコードする（1サンプル1バイト）。"""
    out = bytearray(len(samples))
    for i, sample in enumerate(samples):
        sign = 0x80 if sample < 0 else 0
        magnitude = min(-sample if sample < 0 else sample, MULAW_CLIP) + MULAW_BIAS
        exponent = MULAW_EXP[(magnitude >> 7) & 0xFF]
        mantissa = (magnitude >> (exponent + 3)) & 0x0F
        out[i] = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return bytes(out)


# ---------------------------------------------------------------------------
# モーラ（任意の文章を読むための 1 拍ずつの音）
# ---------------------------------------------------------------------------

# 本体（src/talk/）が出すモーラ。VOICEVOX が1モーラとして読めないものは自動で外す
MORA_BASIC = list("アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン"
                  "ガギグゲゴザジズゼゾダデドバビブベボパピプペポヴ")
MORA_YOON = [c + s for c in "キギシジチニヒビピミリ" for s in "ャュョ"]
MORA_EXTRA = ["シェ", "ジェ", "チェ", "ティ", "ディ", "トゥ", "ドゥ", "テュ", "デュ", "ファ", "フィ", "フェ", "フォ",
              "フュ", "ヴァ", "ヴィ", "ヴェ", "ヴォ", "ヴュ", "ウィ", "ウェ", "ウォ", "イェ", "ツァ", "ツィ", "ツェ",
              "ツォ", "クァ", "クィ", "クェ", "クォ", "グァ", "スィ", "ズィ", "キェ", "ニェ", "ヒェ", "ミェ", "リェ",
              "ギェ", "ビェ", "ピェ"]
MORA_KEY_LOW, MORA_KEY_HIGH, MORA_KEY_DEVOICED = "\x01", "\x02", "\x03"
FRAME_RATE = 24000 / 256          # VOICEVOX の音素長はこの単位（93.75 フレーム/秒）で丸められる
MORA_VOWEL_FRAMES = 9             # 母音の長さ（約 96ms）
MORA_PAD_FRAMES = 8               # 前後につける母音・無音の長さ
MORA_PITCH = {"low": 5.84, "high": 6.08}   # ずんだもんの低い・高い音程（log F0、約4半音差）
MORA_VOWEL_RMS = 2700   # 母音の大きさをそろえる目安（実況のセリフの母音とだいたい同じ）
MORA_DEVOICED_RMS = 900 # 無声化したモーラ（ほぼ子音の息の音）の大きさ


# カナ → VOICEVOX の音素（子音, 母音）
_ROW_CONSONANT = {}
for _row, _cons in [("カキクケコ", "k"), ("ガギグゲゴ", "g"), ("サスセソ", "s"), ("シ", "sh"), ("ザズゼゾ", "z"),
                    ("ジ", "j"), ("タテト", "t"), ("チ", "ch"), ("ツ", "ts"), ("ダデド", "d"), ("ナニヌネノ", "n"),
                    ("ハヒヘホ", "h"), ("フ", "f"), ("バビブベボ", "b"), ("パピプペポ", "p"), ("マミムメモ", "m"),
                    ("ヤユヨ", "y"), ("ラリルレロ", "r"), ("ワ", "w"), ("ヴ", "v"), ("アイウエオ", "")]:
    for _ch in _row:
        _ROW_CONSONANT[_ch] = _cons
_VOWEL_OF = {}
for _row, _v in [("アカガサザタダナハバパマヤラワァャ", "a"), ("イキギシジチニヒビピミリィ", "i"),
                 ("ウクグスズツヌフブプムユルヴゥュ", "u"), ("エケゲセゼテデネヘベペメレェ", "e"),
                 ("オコゴソゾトドノホボポモヨロォョ", "o")]:
    for _ch in _row:
        _VOWEL_OF[_ch] = _v
_SPECIAL = {"ティ": ("t", "i"), "ディ": ("d", "i"), "トゥ": ("t", "u"), "ドゥ": ("d", "u"), "テュ": ("ty", "u"),
            "デュ": ("dy", "u"), "シェ": ("sh", "e"), "ジェ": ("j", "e"), "チェ": ("ch", "e"), "ウィ": ("w", "i"),
            "ウェ": ("w", "e"), "ウォ": ("w", "o"), "イェ": ("y", "e"), "スィ": ("s", "i"), "ズィ": ("z", "i"),
            "クァ": ("kw", "a"), "グァ": ("gw", "a"), "ヴュ": ("by", "u"), "フュ": ("hy", "u")}


def mora_phonemes(kana: str):
    """カナ1モーラ → (子音 or None, 母音)。分からなければ None"""
    if kana == "ン":
        return None, "N"
    if kana in _SPECIAL:
        return _SPECIAL[kana]
    base = _ROW_CONSONANT.get(kana[0])
    if base is None:
        return None
    if len(kana) == 1:
        return (base or None), _VOWEL_OF[kana[0]]
    small = kana[1]
    vowel = _VOWEL_OF.get(small)
    if vowel is None:
        return None
    if small in "ャュョ" or (small == "ェ" and _VOWEL_OF[kana[0]] == "i"):
        if base in ("sh", "j", "ch"):
            return base, vowel
        if _VOWEL_OF[kana[0]] != "i" or base in ("", "y", "w"):
            return None
        return base + "y", vowel
    # ファ・ヴァ・ツァ など（子音はそのまま、母音は小書き文字）
    if base in ("f", "v", "ts"):
        return base, vowel
    return None


def predicted_consonant_length(base: str, speaker: int, consonant: str, vowel: str, kana: str) -> float:
    """VOICEVOX の音素長モデルで「ア＋モーラ＋ア」の子音の長さを予測させる。"""
    phrase = [{"moras": [
        {"text": "ア", "consonant": None, "consonant_length": None, "vowel": "a", "vowel_length": 0.1, "pitch": 5.8},
        {"text": kana, "consonant": consonant, "consonant_length": 0.05 if consonant else None, "vowel": vowel,
         "vowel_length": 0.1, "pitch": 5.8},
        {"text": "ア", "consonant": None, "consonant_length": None, "vowel": "a", "vowel_length": 0.1, "pitch": 5.8}],
        "accent": 1, "pause_mora": None, "is_interrogative": False}]
    result = json.loads(http_post(f"{base}/mora_length?speaker={speaker}", json.dumps(phrase).encode("utf-8"),
                                  "application/json"))
    return result[0]["moras"][1]["consonant_length"] or 0.0


def synthesize_mora(base: str, speaker: int, kana: str, pitch: float, devoiced: bool, speed: float,
                    rate: int) -> list[int] | None:
    """モーラを「ア｜モーラ｜同じ母音」の中から切り出す（前後とのつながりが自然になるように）。"""
    phonemes = mora_phonemes(kana)
    if phonemes is None:
        return None
    consonant, vowel = phonemes
    try:
        consonant_len = predicted_consonant_length(base, speaker, consonant, vowel, kana) if consonant else 0.0
    except urllib.error.HTTPError:
        return None  # この話者のモデルが知らない音素
    query = json.loads(http_post(f"{base}/audio_query?speaker={speaker}&text=" + urllib.parse.quote("ア"), None, None))
    if devoiced:
        if vowel not in ("i", "u"):
            return None
        vowel = vowel.upper()  # VOICEVOX は大文字の母音を無声で読む
    frame = 1 / FRAME_RATE
    cons_frames = max(2, round(consonant_len / speed * FRAME_RATE)) if consonant else 0
    tail_vowel = {"n": "a", "cl": "a"}.get(vowel.lower(), vowel.lower())
    tail_kana = {"a": "ア", "i": "イ", "u": "ウ", "e": "エ", "o": "オ"}[tail_vowel]

    def mora(text, c, c_frames, v, v_frames, p):
        return {"text": text, "consonant": c, "consonant_length": c_frames * frame if c else None,
                "vowel": v, "vowel_length": v_frames * frame, "pitch": 0.0 if v in ("I", "U") else p}

    query["accent_phrases"] = [{
        "moras": [mora("ア", None, 0, "a", MORA_PAD_FRAMES, pitch),
                  mora(kana, consonant, cons_frames, vowel, MORA_VOWEL_FRAMES, pitch),
                  mora(tail_kana, None, 0, tail_vowel, MORA_PAD_FRAMES, pitch)],
        "accent": 1, "pause_mora": None, "is_interrogative": False}]
    query.update(speedScale=1.0, pitchScale=0.0, intonationScale=1.0, prePhonemeLength=MORA_PAD_FRAMES * frame,
                 postPhonemeLength=MORA_PAD_FRAMES * frame, outputSamplingRate=rate, outputStereo=False)
    try:
        wav_bytes = http_post(f"{base}/synthesis?speaker={speaker}", json.dumps(query).encode("utf-8"),
                              "application/json")
    except urllib.error.HTTPError:
        return None
    with wave.open(io.BytesIO(wav_bytes)) as wav:
        raw = wav.readframes(wav.getnframes())
    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    per_frame = rate / FRAME_RATE
    start = round(2 * MORA_PAD_FRAMES * per_frame)
    end = round((2 * MORA_PAD_FRAMES + cons_frames + MORA_VOWEL_FRAMES) * per_frame)
    cut = samples[start:end]
    # VOICEVOX は音程が低いほど声が小さくなるので、母音の大きさでそろえる
    vowel_part = cut[-round(MORA_VOWEL_FRAMES * per_frame):] if not devoiced else cut
    rms = math.sqrt(sum(x * x for x in vowel_part) / max(1, len(vowel_part)))
    target = MORA_DEVOICED_RMS if devoiced else MORA_VOWEL_RMS
    gain = max(0.3, min(4.0, target / rms)) if rms > 0 else 1.0
    cut = [max(-32768, min(32767, int(x * gain))) for x in cut]
    # 端のプチノイズを防ぐ短いフェード（本体はモーラ同士を 5ms 重ねてつなぐ）
    fade = max(1, rate * 15 // 10000)
    for i in range(min(fade, len(cut))):
        w = (i + 1) / (fade + 1)
        cut[i] = int(cut[i] * w)
        cut[-1 - i] = int(cut[-1 - i] * w)
    return cut


def mora_jobs() -> list[tuple[str, str, float, bool]]:
    """(キー, カナ, 音程, 無声) の一覧"""
    jobs = []
    for kana in MORA_BASIC + MORA_YOON + MORA_EXTRA:
        jobs.append((MORA_KEY_LOW + kana, kana, MORA_PITCH["low"], False))
        jobs.append((MORA_KEY_HIGH + kana, kana, MORA_PITCH["high"], False))
        if kana[0] in "カキクケコサシスセソタチツテトハヒフヘホパピプペポ":  # 無声化するのは無声子音のイ段・ウ段だけ
            jobs.append((MORA_KEY_DEVOICED + kana, kana, MORA_PITCH["low"], True))
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--voicevox", default="http://127.0.0.1:50021", help="VOICEVOX エンジンの URL")
    parser.add_argument("--speaker", type=int, default=3, help="話者 / スタイル ID（ずんだもん ノーマル = 3）")
    parser.add_argument("--speed", type=float, default=1.1, help="話速（1.0 = 標準）")
    parser.add_argument("--codec", choices=["mulaw", "adpcm"], default="mulaw", help="圧縮方式")
    parser.add_argument("--rate", type=int, default=None, help="サンプリングレート（既定: mulaw=12000, adpcm=16000）")
    parser.add_argument("--out", default=str(ROOT / "data" / "voice.pak"))
    parser.add_argument("--cache", default=str(ROOT / ".pio" / "voice_cache"))
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--list", action="store_true", help="合成せずにキー一覧だけ表示する")
    args = parser.parse_args()

    if args.rate is None:
        args.rate = 12000 if args.codec == "mulaw" else 16000
    entries = build_entries()
    if args.list:
        for key, speech in entries.items():
            print(f"{key}\t{speech}")
        print(f"{len(entries)} entries", file=sys.stderr)
        return 0

    try:
        version = urllib.request.urlopen(f"{args.voicevox}/version", timeout=5).read().decode()
    except OSError as exc:
        print(f"VOICEVOX に接続できません（{args.voicevox}）: {exc}", file=sys.stderr)
        return 1
    print(f"VOICEVOX {version} / speaker={args.speaker} / {args.codec} {args.rate}Hz / {len(entries)} entries")

    cache_dir = pathlib.Path(args.cache)
    cache_dir.mkdir(parents=True, exist_ok=True)

    def render(item: tuple[str, str]) -> tuple[str, list[int]]:
        key, speech = item
        tag = f"{args.speaker}|{args.speed}|{args.rate}|{speech}"
        cache_file = cache_dir / (hashlib.sha1(tag.encode("utf-8")).hexdigest() + ".pcm")
        if cache_file.exists():
            raw = cache_file.read_bytes()
            return key, list(struct.unpack(f"<{len(raw) // 2}h", raw))
        samples = trim(synthesize(args.voicevox, args.speaker, speech, args.speed, args.rate), args.rate)
        cache_file.write_bytes(struct.pack(f"<{len(samples)}h", *samples))
        return key, samples

    rendered: dict[str, list[int]] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for done, (key, samples) in enumerate(pool.map(render, entries.items()), start=1):
            rendered[key] = samples
            if done % 50 == 0 or done == len(entries):
                print(f"  {done}/{len(entries)}")

    # モーラ（任意の文章を読むための音）
    def render_mora(job):
        key, kana, pitch, devoiced = job
        tag = f"mora3|{args.speaker}|{args.speed}|{args.rate}|{kana}|{pitch}|{devoiced}"
        cache_file = cache_dir / (hashlib.sha1(tag.encode("utf-8")).hexdigest() + ".pcm")
        if cache_file.exists():
            raw = cache_file.read_bytes()
            return key, list(struct.unpack(f"<{len(raw) // 2}h", raw)) if raw else None
        samples = synthesize_mora(args.voicevox, args.speaker, kana, pitch, devoiced, args.speed, args.rate)
        cache_file.write_bytes(struct.pack(f"<{len(samples)}h", *samples) if samples else b"")
        return key, samples

    jobs = mora_jobs()
    skipped = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for key, samples in pool.map(render_mora, jobs):
            if samples:
                rendered[key] = samples
            elif key[0] != MORA_KEY_DEVOICED:
                skipped.append(key[1:])
    print(f"  morae: {sum(1 for k in rendered if k[0] in (MORA_KEY_LOW, MORA_KEY_HIGH)) // 2} kana"
          + (f" (VOICEVOX が読めず除外: {' '.join(sorted(set(skipped)))})" if skipped else ""))

    index = bytearray()
    data = bytearray()
    for key in sorted(rendered, key=lambda k: k.encode("utf-8")):
        samples = rendered[key]
        key_bytes = key.encode("utf-8")
        if len(key_bytes) > 255:
            raise RuntimeError(f"key too long: {key}")
        encoded = mulaw_encode(samples) if args.codec == "mulaw" else adpcm_encode(samples)
        index += struct.pack("<B", len(key_bytes)) + key_bytes
        index += struct.pack("<IIhBB", len(data), len(samples), 0, 0, 0)
        data += encoded

    codec = 1 if args.codec == "mulaw" else 0
    header = b"SCVP" + struct.pack("<HBBIII", 1, codec, 0, args.rate, len(rendered), len(index))
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + index + data)
    seconds = sum(len(s) for s in rendered.values()) / args.rate
    print(f"wrote {out} ({out.stat().st_size / 1024 / 1024:.2f} MB, {len(rendered)} clips, {seconds:.0f} s of audio)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
