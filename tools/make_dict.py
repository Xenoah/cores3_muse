"""StackChan 内蔵ボイス用の読み辞書（dict/ja.dic・dict/en.dic）を作る。

本体（src/talk/）はこの辞書で任意の日本語・英語の文章を読みとアクセントに変換し、
ボイスパックのモーラ音声（make_voice_pack.py が作る）をつなげて喋る。
辞書はファームウェアに埋め込む（platformio.ini の board_build.embed_files）。

使い方:
    python -m pip install numpy wordfreq  # 連接コストの縮約と、英単語の頻度順に使う
    python tools/make_dict.py            # 元データをダウンロードして dict/ja.dic と dict/en.dic を作る

作った辞書はリポジトリに入れてある（dict/）ので、読みの規則を変えるとき以外は実行不要。
本体と同じ処理を PC で試すには tools/talk_test/run.py を使う。

元データ（.pio/dict_cache にダウンロードする）:
    日本語: mecab-naist-jdic（Open JTalk 同梱版、BSD ライセンス）の単語・読み・アクセント・連接コスト
    英語:   CMU Pronouncing Dictionary（BSD ライセンス）。収録語は wordfreq の頻度順で選ぶ

ja.dic（リトルエンディアン）:
    header 64 bytes（下の JA_HEADER を参照）
    chars:  (u16 codepoint, u16 code) を codepoint 順に。表層形は code を 1〜2 バイトで表す
    types:  8 bytes/型 [u8 左クラスタ, u8 右クラスタ, u8 品詞, u8 品詞細分類1, u8 flags, u8 0, u16 規則]
    rules:  u32 offset[規則数] → [u8 n] n×[u8 前の品詞(0xFF=既定), u8 規則, i8 加算]
    conn:   i16 [右クラスタ][左クラスタ]（MeCab の連接コストを k-means で 256 クラスタに縮約）。
            文頭（BOS）の右クラスタと文末（EOS）の左クラスタは header の 34, 35 バイト目
    blocks: 表層形の昇順に 64 語ずつ raw deflate。各ブロックの先頭の表層形は非圧縮で持ち、二分探索する
            語: [u8 共通接頭長][u8 残り長][残り][u8 項目数] 項目×[型(varint)][u8 アクセント]
                [u8 コスト][u8 末尾コピー数][u8 明示読み長][明示読み]
            読みは カタカナ(U+30A0+n)=n、0x60=無声化（直前のモーラ）。表層形の末尾のかなは
            「末尾コピー数」ぶんだけ読みにそのまま使う（送りがな・かなだけの語を省略して小さくする）
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import struct
import sys
import urllib.request
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / ".pio" / "dict_cache"
OUT_DIR = ROOT / "dict"

NAIST_BASE = "https://raw.githubusercontent.com/r9y9/open_jtalk/master/src/mecab-naist-jdic/"
CMUDICT_URL = "https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict"
CMUDICT_LICENSE_URL = "https://raw.githubusercontent.com/cmusphinx/cmudict/master/LICENSE"

BLOCK_GROUPS = 64
CONN_CLUSTERS = 256
COST_OFFSET = 4000
COST_STEP = 96

KANJI = re.compile(r"[㐀-鿿豈-﫿々〆ヶ]")
ALNUM = re.compile(r"[A-Za-z0-9Ａ-Ｚａ-ｚ０-９]")
# 書き言葉にほとんど出ない活用形は入れない（辞書を小さくするため）
DROP_FORMS = {"仮定縮約１", "仮定縮約２", "体言接続特殊", "体言接続特殊２", "命令ｙｏ", "命令ｒｏ", "命令ｉ",
              "基本形-促音便", "未然特殊", "文語基本形", "未然ヌ接続", "現代基本形"}
KEEP_PROPER = {"地域", "一般", "姓"}  # 固有名詞は地名・一般（富士山など）・姓だけ（名・組織名は多すぎる）

POS_CODES = {"名詞": 1, "動詞": 2, "形容詞": 3, "副詞": 4, "助詞": 5, "助動詞": 6, "接続詞": 7, "連体詞": 8,
             "接頭詞": 9, "感動詞": 10, "フィラー": 11, "記号": 12}
G1_CODES = {"形容動詞語幹": 1, "副詞可能": 2, "接尾": 3, "非自立": 4, "サ変接続": 5, "接続助詞": 6, "数": 7,
            "固有名詞": 8, "代名詞": 9, "自立": 10}
RULE_CODES = {"*": 0, "F1": 1, "F2": 2, "F3": 3, "F4": 4, "F5": 5, "C1": 6, "C2": 7, "C3": 8, "C4": 9, "C5": 10,
              "P1": 11, "P2": 12, "P6": 13, "P14": 14}
RULE_RE = re.compile(r"([^/%@0-9-]+)%([A-Z][0-9]+)(?:@(-?[0-9]+))?")
PLAIN_RULE_RE = re.compile(r"^([A-Z][0-9]+)(?:@(-?[0-9]+))?$")
E_ROW = set("エケセテネヘメレゲゼデベペェ")


def download(url: str, path: pathlib.Path) -> None:
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=120) as response:
        path.write_bytes(response.read())


def hira_to_kata(s: str) -> str:
    return "".join(chr(ord(c) + 0x60) if "ぁ" <= c <= "ゖ" else c for c in s)


# ---------------------------------------------------------------------------
# 日本語
# ---------------------------------------------------------------------------

class Entry:
    __slots__ = ("surface", "pron", "acc", "pos", "g1", "renyou", "rule", "cost", "lid", "rid")


def long_vowel_e(pron: str) -> str:
    """漢語の「エ段＋イ」を長音にする（Open JTalk と同じく 経営→ケーエー、先生→センセー）。"""
    out = []
    for i, ch in enumerate(pron):
        if ch == "イ" and out and out[-1] in E_ROW and (i + 1 >= len(pron) or pron[i + 1] not in "ャュョァィゥェォ"):
            out.append("ー")
        else:
            out.append(ch)
    return "".join(out)


def load_naist(per_surface: int = 8) -> dict[str, list[Entry]]:
    raw = (CACHE / "naist-jdic.csv").read_bytes().split(b"\n")
    by: dict[str, list[Entry]] = {}
    for line in raw:
        if not line:
            continue
        r = line.decode("euc_jis_2004").split(",")
        surface, lid, rid, cost, pos, g1, g2, _g3, _ctype, cform = r[:10]
        pron, acc_s, rule = r[12], r[13], r[14]
        if pos == "記号" or ALNUM.search(surface):
            continue
        if g1 == "固有名詞" and g2 not in KEEP_PROPER and _g3 not in KEEP_PROPER:
            continue
        if cform in DROP_FORMS or pron in ("*", ""):
            continue
        e = Entry()
        e.surface = surface
        e.pron = long_vowel_e(pron) if KANJI.search(surface) else pron
        try:
            e.acc = int(acc_s.split("/")[0])
        except ValueError:
            e.acc = 0
        e.pos = POS_CODES.get(pos, 0)
        e.g1 = G1_CODES.get(g1, 0)
        e.renyou = cform.startswith("連用")
        e.rule = rule
        e.cost, e.lid, e.rid = int(cost), int(lid), int(rid)
        by.setdefault(surface, []).append(e)
    out: dict[str, list[Entry]] = {}
    for surface, lst in by.items():
        lst.sort(key=lambda e: e.cost)
        seen = set()
        keep = []
        for e in lst:
            key = (e.lid, e.rid, e.pron, e.acc, e.rule, e.pos, e.g1)
            if key in seen:
                continue
            seen.add(key)
            keep.append(e)
            if len(keep) >= per_surface:
                break
        out[surface] = keep
    return out


def infer_kanji_readings(entries: dict[str, list[Entry]]) -> dict[str, str]:
    """1字の項目が無い漢字の読みを、2字熟語の読みから推定する（予 → ヨ、修 → シュー）。

    片方の字の読みが分かっている熟語から、もう片方の読みを数える。これを数回くり返し、
    いちばん多い読みを採る。辞書に無い熟語を1字ずつでも読めるようにするための控え。
    """
    known: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for surface, lst in entries.items():
        if len(surface) == 1 and KANJI.match(surface):
            for rank, e in enumerate(lst):
                known[surface][e.pron.replace("’", "")] += 4 - min(rank, 3)
    pairs = []
    for surface, lst in entries.items():
        if len(surface) == 2 and all(KANJI.match(c) and c not in "々〆ヶ" for c in surface):
            pairs.append((surface, lst[0].pron.replace("’", "")))
    inferred: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for _ in range(3):
        for (a, b), pron in pairs:
            for k in range(1, len(pron)):
                left, right = pron[:k], pron[k:]
                if right[0] in "ャュョァィゥェォー" or len(left) > 4 or len(right) > 4:
                    continue
                if left in known[a] and a in known:
                    inferred[b][right] += 1
                if right in known[b] and b in known:
                    inferred[a][left] += 1
        for kanji, counts in inferred.items():
            if kanji not in known or not known[kanji]:
                known[kanji] = collections.Counter(dict(counts.most_common(3)))
    out = {}
    for kanji, counts in inferred.items():
        if kanji in entries:
            continue
        reading, n = counts.most_common(1)[0]
        if n >= 2 and re.fullmatch(r"[\u30a1-\u30fc]+", reading):
            out[kanji] = reading
    return out


def load_matrix() -> list[list[int]]:
    lines = (CACHE / "matrix.def").read_text().split("\n")
    n, m = map(int, lines[0].split())
    mat = [[0] * m for _ in range(n)]
    for line in lines[1:]:
        if line:
            a, b, c = line.split()
            mat[int(a)][int(b)] = int(c)
    return mat


def cluster_matrix(mat, lids, rids, k):
    """連接コスト行列を k×k に縮約する（右文脈 ID・左文脈 ID をそれぞれ k-means でまとめる）。"""
    import numpy as np

    full = np.array(mat, dtype=np.float64)
    rids = sorted(rids)
    lids = sorted(lids)
    sub = full[np.ix_(rids, lids)]
    X = np.clip(sub, -4000, 6000)

    def kmeans(data, kk, iters=40, seed=0):
        rng = np.random.default_rng(seed)
        centers = [data[rng.integers(len(data))]]
        d2 = ((data - centers[0]) ** 2).sum(1)
        for _ in range(1, kk):
            p = d2 / d2.sum() if d2.sum() > 0 else None
            centers.append(data[rng.choice(len(data), p=p)])
            d2 = np.minimum(d2, ((data - centers[-1]) ** 2).sum(1))
        c = np.array(centers)
        norms = (data ** 2).sum(1)[:, None]
        for _ in range(iters):
            dist = norms - 2 * data @ c.T + (c ** 2).sum(1)[None, :]
            lab = dist.argmin(1)
            for j in range(kk):
                members = lab == j
                if members.any():
                    c[j] = data[members].mean(0)
        return lab

    rlab = kmeans(X, min(k, len(rids)))
    llab = kmeans(X.T, min(k, len(lids)))
    table = np.zeros((k, k), dtype=np.int64)
    for a in range(k):
        rm = rlab == a
        if not rm.any():
            continue
        for b in range(k):
            lm = llab == b
            if lm.any():
                table[a, b] = int(np.median(sub[np.ix_(rm, lm)]))
    rmap = {r: int(rlab[i]) for i, r in enumerate(rids)}
    lmap = {l: int(llab[i]) for i, l in enumerate(lids)}
    return np.clip(table, -32768, 32767).astype(np.int16), rmap, lmap


def parse_rule(rule: str):
    """連接規則の文字列 → [(前の品詞コード, 規則コード, 加算)]（前の品詞 0xFF = 既定）"""
    if not rule or rule == "*":
        return []
    m = PLAIN_RULE_RE.match(rule)
    if m:
        return [(0xFF, RULE_CODES.get(m.group(1), 0), int(m.group(2) or 0))]
    out = []
    for pos, r, add in RULE_RE.findall(rule):
        if pos in POS_CODES:
            out.append((POS_CODES[pos], RULE_CODES.get(r, 0), int(add or 0)))
    return out


def encode_reading(surface: str, pron: str) -> tuple[int, bytes]:
    """読みを「明示部分＋表層形の末尾からコピーする文字数」に分ける。"""
    kata = hira_to_kata(surface)
    copy = 0
    while (copy < len(surface) and copy < len(pron) and copy < 255
           and not KANJI.match(surface[-1 - copy])
           and kata[-1 - copy] == pron[-1 - copy]
           and pron[-1 - copy] != "’"):
        copy += 1
    explicit = pron[:len(pron) - copy]
    out = bytearray()
    for ch in explicit:
        if ch == "’":
            out.append(0x60)
        elif "ァ" <= ch <= "ー":
            out.append(ord(ch) - 0x30A0)
        else:
            raise ValueError(f"unexpected reading char {ch!r} in {surface}/{pron}")
    return copy, bytes(out)


def build_ja() -> bytes:
    for name in ("naist-jdic.csv", "matrix.def", "COPYING"):
        download(NAIST_BASE + name, CACHE / ("naist-" + name if name == "COPYING" else name))
    entries = load_naist()
    # 1字の項目が無い漢字に推定した読みを足す（未知の熟語を黙って飛ばさないため）。
    # コストは MeCab の未知の漢字と同じにして、辞書の語より優先されないようにする
    fallback = infer_kanji_readings(entries)
    for kanji, reading in fallback.items():
        e = Entry()
        e.surface, e.pron, e.acc = kanji, reading, 0
        e.pos, e.g1, e.renyou, e.rule = POS_CODES["名詞"], 0, False, "C1"
        e.cost, e.lid, e.rid = 10654, 1345, 1345
        entries[kanji] = [e]
    print(f"ja: inferred readings for {len(fallback)} kanji")
    # 読みに使えない文字を含む語は除く（記号入りの顔文字など）
    bad = [s for s, lst in entries.items() if any(not re.fullmatch(r"[ァ-ー’]+", e.pron) for e in lst)]
    for s in bad:
        del entries[s]
    print(f"ja: {len(entries)} surfaces, {sum(len(v) for v in entries.values())} entries")

    mat = load_matrix()
    unk_id = 1345  # 名詞,一般（未知語）
    lids = {0, unk_id}
    rids = {0, unk_id}
    for lst in entries.values():
        for e in lst:
            lids.add(e.lid)
            rids.add(e.rid)
    conn, rmap, lmap = cluster_matrix(mat, lids, rids, CONN_CLUSTERS)

    # 文字コード（頻度順。上位 128 字は 1 バイト）
    freq = collections.Counter()
    for s in entries:
        freq.update(s)
    order = [c for c, _ in freq.most_common()]
    if len(order) >= 0x8000:
        raise RuntimeError("too many characters")
    code = {c: i for i, c in enumerate(order)}

    def enc_surface(s: str) -> bytes:
        out = bytearray()
        for ch in s:
            k = code[ch]
            out += bytes([k]) if k < 0x80 else bytes([0x80 | (k >> 8), k & 0xFF])
        return bytes(out)

    # 連接規則と型
    rule_ids: dict[tuple, int] = {}
    type_ids: dict[tuple, int] = {}
    type_freq = collections.Counter()
    unk_type_key = (lmap[unk_id], rmap[unk_id], POS_CODES["名詞"], 0, 0, tuple(parse_rule("C1")))

    def type_key(e: Entry):
        return (lmap[e.lid], rmap[e.rid], e.pos, e.g1, 1 if e.renyou else 0, tuple(parse_rule(e.rule)))

    for lst in entries.values():
        for e in lst:
            type_freq[type_key(e)] += 1
    type_freq[unk_type_key] += 0
    for t, _ in type_freq.most_common():
        type_ids[t] = len(type_ids)
        if t[5] not in rule_ids:
            rule_ids[t[5]] = len(rule_ids)

    chars_blob = b"".join(struct.pack("<HH", ord(c), code[c]) for c in sorted(order, key=ord) if ord(c) < 0x10000)
    types_blob = b"".join(struct.pack("<BBBBBBH", t[0], t[1], t[2], t[3], t[4], 0, rule_ids[t[5]])
                          for t, _ in sorted(type_ids.items(), key=lambda x: x[1]))
    rule_list = sorted(rule_ids.items(), key=lambda x: x[1])
    rule_data = bytearray()
    rule_offsets = []
    for rule, _ in rule_list:
        rule_offsets.append(len(rule_data))
        rule_data.append(len(rule))
        for pos, r, add in rule:
            rule_data += struct.pack("<BBb", pos, r, max(-128, min(127, add)))
    rules_blob = b"".join(struct.pack("<I", 4 * len(rule_offsets) + o) for o in rule_offsets) + bytes(rule_data)
    conn_blob = conn.astype("<i2").tobytes()

    items = sorted(((enc_surface(s), s, lst) for s, lst in entries.items()), key=lambda x: x[0])
    max_chars = max(len(s) for _, s, _ in items)
    block_index = bytearray()
    key_index = bytearray()
    key_area = bytearray()
    data = bytearray()
    raw_total = 0
    for b in range(0, len(items), BLOCK_GROUPS):
        blk = bytearray()
        prev = b""
        for k, s, lst in items[b:b + BLOCK_GROUPS]:
            p = 0
            while p < min(len(prev), len(k), 255) and prev[p] == k[p]:
                p += 1
            blk += bytes([p, len(k) - p]) + k[p:]
            blk.append(len(lst))
            for e in lst:
                t = type_ids[type_key(e)]
                blk += bytes([t]) if t < 0x80 else bytes([0x80 | (t >> 8), t & 0xFF])
                copy, explicit = encode_reading(s, e.pron)
                q = max(0, min(255, (e.cost + COST_OFFSET + COST_STEP // 2) // COST_STEP))
                blk += bytes([max(0, min(e.acc, 255)), q, copy, len(explicit)]) + explicit
            prev = k
        comp = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = comp.compress(bytes(blk)) + comp.flush()
        if len(blk) > 0xFFFF or len(packed) > 0xFFFF:
            raise RuntimeError("block too large")
        block_index += struct.pack("<IHH", len(data), len(packed), len(blk))
        key_index += struct.pack("<I", len(key_area))
        first = items[b][0]
        key_area += bytes([len(first)]) + first
        data += packed
        raw_total += len(blk)

    header_size = 64
    sections = [chars_blob, types_blob, rules_blob, conn_blob, bytes(block_index), bytes(key_index), bytes(key_area)]
    offsets = []
    pos = header_size
    for sec in sections:
        pos = (pos + 3) & ~3
        offsets.append(pos)
        pos += len(sec)
    pos = (pos + 3) & ~3
    data_offset = pos
    blocks = len(block_index) // 8
    header = struct.pack(
        "<4sHHIIIIIIHHIIIIII HH",
        b"SCJD", 1, 0,
        len(chars_blob) // 4, offsets[0],
        len(type_ids), offsets[1],
        len(rule_ids), offsets[2],
        CONN_CLUSTERS, rmap[0] | (lmap[0] << 8), offsets[3],
        blocks, offsets[4], offsets[5], offsets[6], data_offset,
        max_chars, type_ids[unk_type_key],
    )
    header = header.ljust(header_size, b"\0")
    out = bytearray(header)
    for off, sec in zip(offsets, sections):
        out += b"\0" * (off - len(out))
        out += sec
    out += b"\0" * (data_offset - len(out))
    out += data
    print(f"ja.dic: {len(out) / 1e6:.2f} MB ({blocks} blocks, raw {raw_total / 1e6:.2f} MB, "
          f"{len(type_ids)} types, {len(rule_ids)} rules, {len(order)} chars)")
    return bytes(out)


# ---------------------------------------------------------------------------
# 英語
# ---------------------------------------------------------------------------

VOWELS = ["AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"]
CONSONANTS = ["B", "CH", "D", "DH", "F", "G", "HH", "JH", "K", "L", "M", "N", "NG", "P", "R", "S", "SH", "T",
              "TH", "V", "W", "Y", "Z", "ZH"]
EN_WORDS = 120000         # wordfreq の上位何語から選ぶか
LTS_OFFSETS = [0, 1, -1, 2, -2]  # 決定木で見る文字の位置（注目文字からの距離、情報量の多い順）。5 段で約 370KB
LTS_LETTERS = "#abcdefghijklmnopqrstuvwxyz'"   # '#' = 語の外


def phone_code(p: str) -> int:
    """ARPAbet（強勢つき）→ 1 バイトのコード。母音 = 番号*3+強勢、子音 = 45+番号"""
    if p[-1].isdigit():
        return VOWELS.index(p[:-1]) * 3 + int(p[-1])
    return 45 + CONSONANTS.index(p)


def load_cmudict() -> dict[str, list[int]]:
    download(CMUDICT_URL, CACHE / "cmudict.dict")
    download(CMUDICT_LICENSE_URL, CACHE / "cmudict-LICENSE")
    words: dict[str, list[int]] = {}
    for line in (CACHE / "cmudict.dict").read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        word, *phones = line.split()
        if "(" in word or not re.fullmatch(r"[a-z][a-z']*", word):
            continue
        try:
            words[word] = [phone_code(p) for p in phones]
        except ValueError:
            continue
    return words


def align(words: dict[str, list[int]], iterations: int = 6):
    """文字と音素の対応を EM で学習する（1文字 → 無音 / 1音素 / 2音素）。"""
    import math

    prob: dict[tuple, float] = collections.defaultdict(lambda: 1e-3)
    alignments: dict[str, list[tuple]] = {}
    for it in range(iterations):
        counts = collections.Counter()
        letter_totals = collections.Counter()
        alignments.clear()
        for word, phones in words.items():
            n, m = len(word), len(phones)
            if m > 2 * n:
                continue
            inf = float("inf")
            best = [[inf] * (m + 1) for _ in range(n + 1)]
            back = [[None] * (m + 1) for _ in range(n + 1)]
            best[0][0] = 0.0
            for i in range(n):
                letter = word[i]
                for j in range(m + 1):
                    b = best[i][j]
                    if b == inf:
                        continue
                    for k in (0, 1, 2):
                        if j + k > m:
                            break
                        target = tuple(phones[j:j + k])
                        if it == 0:
                            c = b + (0.0 if k == 1 else 1.0 if k == 0 else 2.0)
                        else:
                            c = b - math.log(prob[(letter, target)])
                        if c < best[i + 1][j + k]:
                            best[i + 1][j + k] = c
                            back[i + 1][j + k] = (j, target)
            if best[n][m] == inf:
                continue
            path = []
            j = m
            for i in range(n, 0, -1):
                pj, target = back[i][j]
                path.append(target)
                j = pj
            path.reverse()
            alignments[word] = path
            for letter, target in zip(word, path):
                counts[(letter, target)] += 1
                letter_totals[letter] += 1
        prob = collections.defaultdict(lambda: 1e-6)
        for (letter, target), c in counts.items():
            prob[(letter, target)] = c / letter_totals[letter]
    return alignments


def lts_instances(alignments):
    rows = []
    for word, path in alignments.items():
        padded = "#" * 4 + word + "#" * 4
        for i, target in enumerate(path):
            feats = tuple(padded[4 + i + off] for off in LTS_OFFSETS)
            rows.append((feats, target))
    return rows


class TreeNode:
    __slots__ = ("default", "children")


def build_tree(rows, depth=0, max_depth=len(LTS_OFFSETS)):
    """IGTree: 決められた順に文字を見て、多数決の答えが変わるところだけ枝を残す。"""
    counts = collections.Counter(t for _, t in rows)
    node = TreeNode()
    node.default = counts.most_common(1)[0][0]
    node.children = {}
    if depth >= max_depth or len(counts) == 1:
        return node
    groups = collections.defaultdict(list)
    for feats, target in rows:
        groups[feats[depth]].append((feats, target))
    for value, sub in groups.items():
        child = build_tree(sub, depth + 1, max_depth)
        # 子の答えがすべて親の既定と同じなら枝は要らない
        if child.children or child.default != node.default:
            node.children[value] = child
    return node


def tree_predict(node, feats):
    depth = 0
    while True:
        child = node.children.get(feats[depth]) if depth < len(feats) else None
        if child is None:
            return node.default
        node = child
        depth += 1


def lts_guess(tree, word):
    padded = "#" * 4 + word + "#" * 4
    out = []
    for i in range(len(word)):
        out += list(tree_predict(tree, tuple(padded[4 + i + off] for off in LTS_OFFSETS)))
    return out


def serialize_tree(tree, class_ids):
    """ノード: [u16 既定クラス][u8 子の数] 子×[u8 文字][u24 子の位置]（位置は木の先頭から）"""
    order = []
    stack = [tree]
    while stack:
        n = stack.pop()
        order.append(n)
        for v in sorted(n.children, key=LTS_LETTERS.index, reverse=True):
            stack.append(n.children[v])
    offsets = {}
    pos = 0
    for n in order:
        offsets[id(n)] = pos
        pos += 3 + 4 * len(n.children)
    out = bytearray()
    for n in order:
        out += struct.pack("<HB", class_ids[n.default], len(n.children))
        for v in sorted(n.children, key=LTS_LETTERS.index):
            child = offsets[id(n.children[v])]
            out += bytes([LTS_LETTERS.index(v)]) + child.to_bytes(3, "little")
    return bytes(out), len(order)


# 本体の EnReader::romajiToKana と同じ規則（ローマ字として全部読める語か）
ROMAJI_SYLLABLES = sorted("""kya kyu kyo gya gyu gyo sha shu sho she shi cha chu cho che chi tsu nya nyu nyo hya hyu hyo
bya byu byo pya pyu pyo mya myu myo rya ryu ryo sya syu syo tya tyu tyo zya zyu zyo ja ju jo je ji fa fi fu fe fo
ka ki ku ke ko ga gi gu ge go sa si su se so za zi zu ze zo ta ti tu te to da di du de do na ni nu ne no ha hi hu he
ho ba bi bu be bo pa pi pu pe po ma mi mu me mo ya yu yo ra ri ru re ro la li lu le lo wa wo va vi vu a i u e o""".split(),
                          key=lambda x: -len(x))


def romaji_ok(s: str) -> bool:
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "n" and (i + 1 >= n or s[i + 1] == "'" or s[i + 1] not in "aiueoy"):
            i += 2 if i + 1 < n and s[i + 1] == "'" else 1
            continue
        if c == "m" and i + 1 < n and s[i + 1] in "bpm":
            i += 1
            continue
        if i + 1 < n and c == s[i + 1] and c not in "aiueon":
            i += 1
            continue
        if c == "t" and s[i + 1:i + 3] == "ch":
            i += 1
            continue
        for syl in ROMAJI_SYLLABLES:
            if s.startswith(syl, i):
                i += len(syl)
                break
        else:
            return False
    return True


def build_en() -> bytes:
    import wordfreq  # 収録語を選ぶためだけに使う（pip install wordfreq）

    words = load_cmudict()
    print(f"en: cmudict {len(words)} words")
    alignments = align(words)
    rows = lts_instances(alignments)
    tree = build_tree(rows)
    classes = sorted({t for _, t in rows})
    class_ids = {c: i for i, c in enumerate(classes)}
    tree_blob, node_count = serialize_tree(tree, class_ids)
    class_blob = struct.pack("<H", len(classes)) + b"".join(bytes([len(c)]) + bytes(c) for c in classes)
    lts_blob = (bytes([len(LTS_OFFSETS)]) + bytes((o + 256) % 256 for o in LTS_OFFSETS)
                + struct.pack("<I", len(class_blob)) + class_blob + tree_blob)

    # 収録語: よく使う語のうち、綴りからの推定と発音が違うもの。
    # ローマ字として読める語（time, made, i）は本体がローマ字読みしないよう必ず入れる
    top = wordfreq.top_n_list("en", EN_WORDS)
    chosen = {}
    guessed_right = 0
    for w in top:
        if w in words and w not in chosen:
            if lts_guess(tree, w) == words[w] and not romaji_ok(w):
                guessed_right += 1
                continue
            chosen[w] = words[w]
    print(f"en: {len(chosen)} words stored, {guessed_right} left to the letter-to-sound tree ({node_count} nodes)")

    items = sorted((w.encode("ascii"), p) for w, p in chosen.items())
    block_index = bytearray()
    key_index = bytearray()
    key_area = bytearray()
    data = bytearray()
    for b in range(0, len(items), BLOCK_GROUPS):
        blk = bytearray()
        prev = b""
        for k, phones in items[b:b + BLOCK_GROUPS]:
            p = 0
            while p < min(len(prev), len(k)) and prev[p] == k[p]:
                p += 1
            blk += bytes([p, len(k) - p]) + k[p:] + bytes([len(phones)]) + bytes(phones)
            prev = k
        comp = zlib.compressobj(9, zlib.DEFLATED, -15)
        packed = comp.compress(bytes(blk)) + comp.flush()
        block_index += struct.pack("<IHH", len(data), len(packed), len(blk))
        key_index += struct.pack("<I", len(key_area))
        key_area += bytes([len(items[b][0])]) + items[b][0]
        data += packed

    header_size = 32
    sections = [bytes(block_index), bytes(key_index), bytes(key_area), lts_blob]
    offsets = []
    pos = header_size
    for sec in sections:
        pos = (pos + 3) & ~3
        offsets.append(pos)
        pos += len(sec)
    pos = (pos + 3) & ~3
    data_offset = pos
    header = struct.pack("<4sHHIIIIII", b"SCED", 1, 0, len(block_index) // 8, offsets[0], offsets[1], offsets[2],
                         data_offset, offsets[3])
    out = bytearray(header.ljust(header_size, b"\0"))
    for off, sec in zip(offsets, sections):
        out += b"\0" * (off - len(out))
        out += sec
    out += b"\0" * (data_offset - len(out))
    out += data
    print(f"en.dic: {len(out) / 1e6:.2f} MB (lts {len(lts_blob) / 1e3:.0f} KB)")
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(OUT_DIR))
    parser.add_argument("--only", choices=["ja", "en"], help="片方だけ作る")
    args = parser.parse_args()
    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.only in (None, "ja"):
        (out_dir / "ja.dic").write_bytes(build_ja())
    if args.only in (None, "en"):
        (out_dir / "en.dic").write_bytes(build_en())
    return 0


if __name__ == "__main__":
    sys.exit(main())
