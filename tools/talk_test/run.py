"""src/talk/ を PC でビルドして読み上げの前処理を確かめる（開発用）。

    python -m pip install ziglang            # C++ コンパイラ（zig 同梱の clang）
    python tools/talk_test/run.py "今日は3時にPETGで印刷するよ"
    python tools/talk_test/run.py --file sentences.txt
    python tools/talk_test/run.py --eval corpus.json   # VOICEVOX の読み（kana）と比べる
    python tools/talk_test/run.py --wav out.wav "こんにちは、Hello!"  # data/voice.pak で音声を作って聞く

inflate は ESP32-S3 では ROM の miniz を使うので、PC では miniz（MIT）をダウンロードして使う。
"""

from __future__ import annotations

import argparse
import io
import json
import pathlib
import re
import subprocess
import sys
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE = ROOT / ".pio" / "test_cache"
EXE = CACHE / "talk_test.exe"
MINIZ_URL = "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip"


def build() -> None:
    CACHE.mkdir(parents=True, exist_ok=True)
    if not (CACHE / "miniz.c").exists():
        data = urllib.request.urlopen(MINIZ_URL, timeout=60).read()
        zipfile.ZipFile(io.BytesIO(data)).extractall(CACHE)
    sources = sorted((ROOT / "src" / "talk").glob("*.cpp"))
    headers = sorted((ROOT / "src" / "talk").glob("*.h")) + list(pathlib.Path(__file__).parent.glob("*.h"))
    newest = max(p.stat().st_mtime for p in sources + headers +
                 [pathlib.Path(__file__), pathlib.Path(__file__).with_name("talk_test.cpp")])
    if EXE.exists() and EXE.stat().st_mtime > newest:
        return
    miniz_obj = CACHE / "miniz.o"
    if not miniz_obj.exists():
        subprocess.run([sys.executable, "-m", "ziglang", "cc", "-c", "-O2", "-w", str(CACHE / "miniz.c"),
                        "-o", str(miniz_obj)], check=True)
    cmd = [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O2", "-w",
           f"-I{ROOT / 'src'}", f"-I{ROOT / 'src' / 'talk'}", f"-I{CACHE}",
           str(pathlib.Path(__file__).with_name("talk_test.cpp")), *map(str, sources),
           str(miniz_obj), "-o", str(EXE)]
    subprocess.run(cmd, check=True)


def run(lines: list[str], units: bool = False) -> list[str]:
    build()
    proc = subprocess.run([str(EXE), str(ROOT / "dict" / "ja.dic"), str(ROOT / "dict" / "en.dic")]
                          + (["--units"] if units else []),
                          input="\n".join(lines).encode("utf-8"), capture_output=True, check=True)
    sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
    return proc.stdout.decode("utf-8").splitlines()


# --- VOICEVOX との比較 -------------------------------------------------------

VOWEL_ROWS = {"a": "アカサタナハマヤラワガザダバパァャヮ", "i": "イキシチニヒミリギジヂビピィ",
              "u": "ウクスツヌフムユルグズヅブプゥュヴ", "e": "エケセテネヘメレゲゼデベペェ",
              "o": "オコソトノホモヨロヲゴゾドボポォョ"}
VOWEL_KANA = {"a": "ア", "i": "イ", "u": "ウ", "e": "エ", "o": "オ"}
SMALL = set("ァィゥェォャュョヮ")
NORM = {"ヲ": "オ", "ヂ": "ジ", "ヅ": "ズ"}


def split_moras(kana: str) -> list[str]:
    out: list[str] = []
    for ch in kana:
        if ch in SMALL and out and out[-1] not in "ッンー":
            out[-1] += ch
        else:
            out.append(ch)
    return out


def moras_pitch(notation: str):
    moras, pitch = [], []
    for phrase in re.split(r"[/、]", notation.replace("_", "").replace("？", "")):
        if not phrase:
            continue
        acc, chars = 0, []
        for ch in phrase:
            if ch == "'":
                acc = len(split_moras("".join(chars)))
            else:
                chars.append(ch)
        pm = []
        for m in split_moras("".join(chars)):
            if m == "ー" and pm:
                v = next((k for k, row in VOWEL_ROWS.items() if pm[-1][-1] in row), None)
                m = VOWEL_KANA.get(v, m)
            pm.append(NORM.get(m, m))
        for i in range(len(pm)):
            high = i == 0 if acc == 1 else (i > 0 if acc == 0 else 0 < i < acc)
            pitch.append("H" if high else "L")
        moras += pm
    return moras, pitch


def edit_distance(a, b) -> int:
    prev = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        cur = [i]
        for j, y in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (x != y)))
        prev = cur
    return prev[-1]


def evaluate(corpus: dict, show: int) -> None:
    texts = list(corpus)
    outputs = run(texts)
    total = err = ptotal = perr = exact = shown = 0
    for text, mine in zip(texts, outputs):
        truth = corpus[text]["kana"]
        tm, tp = moras_pitch(truth)
        mm, mp = moras_pitch(mine)
        d = edit_distance(mm, tm)
        total += len(tm)
        err += d
        if d == 0:
            exact += 1
            ptotal += len(tp)
            perr += sum(a != b for a, b in zip(mp, tp))
        elif shown < show:
            shown += 1
            print(f"{text}\n  truth: {truth}\n  mine : {mine}")
    print(f"mora error {err / total:.3%}  exact {exact}/{len(texts)}  "
          f"pitch error (exact sentences) {perr / max(ptotal, 1):.3%}")


# --- 音声のプレビュー（本体の BuiltinVoice と同じつなぎ方） --------------------------

def load_pack(path: pathlib.Path):
    import struct
    data = path.read_bytes()
    codec = data[6]
    rate, count, index_bytes = struct.unpack_from("<III", data, 8)
    if codec != 1:
        raise SystemExit("μ-law のボイスパックだけ対応しています")
    clips, pos = {}, 20
    for _ in range(count):
        kl = data[pos]
        key = data[pos + 1:pos + 1 + kl].decode("utf-8")
        pos += 1 + kl
        off, n = struct.unpack_from("<II", data, pos)
        pos += 12
        clips[key] = (20 + index_bytes + off, n)
    return data, rate, clips


def mulaw(b: int) -> int:
    b = ~b & 0xFF
    sign, exponent, mantissa = b & 0x80, (b >> 4) & 7, b & 0x0F
    s = (((mantissa << 3) + 0x84) << exponent) - 0x84
    return -s if sign else s


def render_wav(text: str, out: pathlib.Path, legacy: bool = False) -> None:
    if not legacy:
        build()
        subprocess.run([str(EXE), str(ROOT / "dict" / "ja.dic"), str(ROOT / "dict" / "en.dic"),
                        "--wav", str(ROOT / "data" / "voice.pak"), str(out)],
                       input=(text + "\n").encode("utf-8"), check=True)
        print(f"wrote {out} (firmware's shared prosody / waveform renderer)")
        return
    import struct
    import wave
    data, rate, clips = load_pack(ROOT / "data" / "voice.pak")
    units = run([text], units=True)[0].split()
    overlap = 60
    samples: list[int] = []
    joinable = False
    for u in units:
        kind, value = u.split(":", 1)
        if kind == "P":
            samples += [0] * (rate * int(value) // 1000)
            joinable = False
            continue
        key = {"H": "\x02", "L": "\x01", "D": "\x03"}[kind] + value
        if key not in clips and kind == "D":
            key = "\x01" + value
        if key not in clips:
            print(f"(no clip for {value})", file=sys.stderr)
            continue
        off, n = clips[key]
        clip = [mulaw(x) for x in data[off:off + n]]
        if joinable and len(samples) >= overlap and len(clip) > 2 * overlap:
            tail = samples[-overlap:]
            del samples[-overlap:]
            for i in range(overlap):
                w = (i + 1) / (overlap + 1)
                clip[i] = int(tail[i] * (1 - w) + clip[i] * w)
        samples += clip
        joinable = True
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"wrote {out} ({len(samples) / rate:.1f} s): {' '.join(units)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("text", nargs="*")
    parser.add_argument("--file")
    parser.add_argument("--eval", help="VOICEVOX の読みを入れた JSON（{文: {kana: ...}}）")
    parser.add_argument("--show", type=int, default=10)
    parser.add_argument("--wav", help="data/voice.pak のモーラで音声を作って書き出す（text を1つ指定）")
    parser.add_argument("--legacy", action="store_true", help="--wav で旧方式の比較用音声を出す")
    parser.add_argument("--self-test", action="store_true", help="音程・長さ・長音・無声化の波形テスト")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    if args.self_test:
        build()
        subprocess.run([str(EXE), "--self-test"], check=True)
        return 0
    if args.wav:
        render_wav(" ".join(args.text), pathlib.Path(args.wav), legacy=args.legacy)
        return 0
    if args.eval:
        evaluate(json.loads(pathlib.Path(args.eval).read_text(encoding="utf-8")), args.show)
        return 0
    lines = list(args.text)
    if args.file:
        lines += pathlib.Path(args.file).read_text(encoding="utf-8").splitlines()
    for text, out in zip(lines, run(lines)):
        print(f"{text}\n  {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
