# Third-Party Notices

このプロジェクトは、以下を含むオープンソースソフトウェアを利用しています。
各ライブラリの最新の利用条件は、リンク先の公式リポジトリも確認してください。

## M5Stack-Avatar

- Project: [stack-chan/m5stack-avatar](https://github.com/stack-chan/m5stack-avatar)
- Version: `0.10.0`
- Copyright: Copyright (c) 2018 Shinya Ishikawa
- License: MIT License

顔、表情、目、口、視線、呼吸、配色、吹き出し、変形の描画に使用しています。

```text
MIT License

Copyright (c) 2018 Shinya Ishikawa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## M5Stack libraries

- [M5Unified](https://github.com/m5stack/M5Unified)
- [M5GFX](https://github.com/m5stack/M5GFX)
- [StackChan-BSP](https://github.com/m5stack/StackChan-BSP)
- [M5CoreS3](https://github.com/m5stack/M5CoreS3)
- [M5Unit-NFC](https://github.com/m5stack/M5Unit-NFC)

## IRremoteESP8266

- [crankyoldgit/IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266)

## ArduinoJson

- Project: [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- Version: `^7.4.2`
- License: MIT License

プリンターの状態 JSON の解析と、Web API の JSON 生成に使用しています。

## mecab-naist-jdic（Open JTalk 同梱版）

- Project: [Open JTalk](http://open-jtalk.sourceforge.net/)（辞書は [r9y9/open_jtalk](https://github.com/r9y9/open_jtalk) の `src/mecab-naist-jdic`）
- License: BSD License（NAIST / HTS Working Group）

`tools/make_dict.py` がこの辞書の単語・読み・アクセント型・アクセント結合規則・連接コストから
`dict/ja.dic` を作り、ファームウェアに埋め込んでいます。アクセント句とアクセント核の決め方は
Open JTalk（njd_set_accent_phrase / njd_set_accent_type）の規則にならっています。

```text
Copyright (c) 2009, Nara Institute of Science and Technology, Japan.

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
Neither the name of the Nara Institute of Science and Technology
(NAIST) nor the names of its contributors may be used to endorse or
promote products derived from this software without specific prior
written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

```text
The Japanese TTS System "Open JTalk" developed by HTS Working Group
http://open-jtalk.sourceforge.net/

Copyright (c) 2008-2014  Nagoya Institute of Technology
                         Department of Computer Science

All rights reserved.

Redistribution and use in source and binary forms, with or
without modification, are permitted provided that the following
conditions are met:

- Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.
- Neither the name of the HTS working group nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## CMU Pronouncing Dictionary

- Project: [cmusphinx/cmudict](https://github.com/cmusphinx/cmudict)
- License: BSD License（Carnegie Mellon University）

`tools/make_dict.py` がこの辞書から英単語の発音と、綴りから発音を推定する決定木を作り、
`dict/en.dic` としてファームウェアに埋め込んでいます。

```text
Copyright (C) 1993-2015 Carnegie Mellon University. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   The contents of this file are deemed to be source code.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

This work was supported in part by funding from the Defense Advanced
Research Projects Agency, the Office of Naval Research and the National
Science Foundation of the United States of America, and by member
companies of the Carnegie Mellon Sphinx Speech Consortium. We acknowledge
the contributions of many volunteers to the expansion and improvement of
this dictionary.

THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND
ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## ビルド・テストだけに使うもの（ファームウェアには含まない）

- [wordfreq](https://github.com/rspeer/wordfreq)（コード Apache-2.0、データ CC BY-SA 4.0）:
  `dict/en.dic` に入れる英単語を頻度順に選ぶためだけに使っています（頻度の値は含めていません）。
- [miniz](https://github.com/richgel999/miniz)（MIT）: `tools/talk_test` が PC で辞書を展開するときに使います。
  本体は ESP32-S3 の ROM に入っている inflate を使います。
- [ziglang](https://pypi.org/project/ziglang/)（MIT）: `tools/talk_test` の C++ コンパイラ。

## VOICEVOX

VOICEVOXおよびキャラクター音声は本リポジトリへ同梱していません。
利用者が別途用意したVOICEVOX EngineへHTTP接続します。

- [VOICEVOX公式サイト](https://voicevox.hiroshiba.jp/)
- [VOICEVOX Engine](https://github.com/VOICEVOX/voicevox_engine)

音声利用時はVOICEVOXおよび各キャラクターの利用規約に従ってください。

## stackchan-llm-mqtt-fft

- Project: [Xenoah/stackchan-llm-mqtt-fft](https://github.com/Xenoah/stackchan-llm-mqtt-fft)

顔の制御（`AvatarFaceController`・`FaceHud`）、サーボ校正（`CalibrationController`）、
内蔵ボイスと読み辞書（`BuiltinVoice`・`src/talk/`・`dict/`・`tools/make_dict.py`・`tools/make_voice_pack.py`）、
VOICEVOX クライアント（`VoiceVoxClient`）、BSP のサーボ修正（`tools/patch_stackchan_bsp.py`）を移植しています。

## Meta Model API

Muse の各モデルは [Meta Model API](https://dev.meta.ai/) を通じて利用します。モデルはこのリポジトリに含まれません。
利用には Meta Model API の利用規約が適用されます。本プロジェクトは Meta とは関係ありません。
