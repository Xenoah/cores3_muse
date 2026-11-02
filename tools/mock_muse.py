"""A stand-in for the Meta Model API, for testing the firmware without an API key.

It checks requests the way the real API documents them, answers with canned text, and
reports what it received. Audio and images are measured and discarded unless --save-image
is given.

    python tools/mock_muse.py --transcript "..." --reply "[happy] ..."
    # then on the device, over USB:  baseurl http://<this PC>:8765/v1
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import math
import struct
import sys
import threading
import time
import wave
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS = None
LOCK = threading.Lock()


def report(event: str, **fields) -> None:
    with LOCK:
        print(json.dumps({"event": event, **fields}, ensure_ascii=False), flush=True)


def parse_multipart(content_type: str, body: bytes) -> dict[str, tuple[str, bytes]]:
    message = BytesParser().parsebytes(
        b"Content-Type: " + content_type.encode() + b"\r\nMIME-Version: 1.0\r\n\r\n" + body)
    if not message.is_multipart():
        raise ValueError("not multipart")
    return {part.get_param("name", header="content-disposition"):
            (part.get_content_type(), part.get_payload(decode=True))
            for part in message.get_payload()}


def jpeg_size(data: bytes) -> tuple[int, int] | None:
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            return None
        marker, length = data[i + 1], struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            height, width = struct.unpack(">HH", data[i + 5:i + 9])
            return width, height
        i += 2 + length
    return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def send_error_json(self, status: int, code: str, message: str) -> None:
        self.send_json(status, {"error": {"code": code, "message": message, "param": None,
                                          "type": "invalid_request_error"}})

    def do_GET(self):
        if self.path == "/v1/models":
            report("models", authorization=self.headers.get("Authorization"))
            self.send_json(200, {"object": "list", "data": [
                {"id": "muse-spark-1.3"}, {"id": "muse-voice-transcribe-1.0"}]})
        else:
            self.send_error_json(404, "not_found", self.path)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        started = time.monotonic()
        body = self.rfile.read(length)
        upload_s = time.monotonic() - started
        if self.path == "/v1/asr/transcribe":
            self.transcribe(body, upload_s)
        elif self.path == "/v1/chat/completions":
            self.chat(body)
        else:
            self.send_error_json(404, "not_found", self.path)

    def transcribe(self, body: bytes, upload_s: float) -> None:
        try:
            parts = parse_multipart(self.headers["Content-Type"], body)
            settings = json.loads(parts["request"][1])
            if parts["request"][0] != "application/json":
                raise ValueError("request part must be application/json")
            with wave.open(io.BytesIO(parts["audio"][1])) as wav:
                channels, width, rate = wav.getnchannels(), wav.getsampwidth(), wav.getframerate()
                frames = wav.readframes(wav.getnframes())
            if channels != 1 or width != 2 or rate not in (16000, 24000):
                raise ValueError(f"unsupported WAV {channels}ch {width * 8}bit {rate}Hz")
            for field, expected in (("model", "muse-voice-transcribe-1.0"),
                                    ("audioEncoding", "WAV")):
                if settings.get(field) != expected:
                    raise ValueError(f"{field}={settings.get(field)!r}")
        except Exception as exc:  # the real API answers 400 here
            report("transcribe_rejected", error=str(exc))
            self.send_error_json(400, "invalid_request", str(exc))
            return
        samples = struct.unpack(f"<{len(frames) // 2}h", frames)
        rms = math.sqrt(sum(s * s for s in samples) / max(len(samples), 1))
        report("transcribe", settings=settings, seconds=round(len(samples) / rate, 2), rate=rate,
               rms=round(rms, 1), peak=max(map(abs, samples), default=0),
               upload_s=round(upload_s, 2), authorization=self.headers.get("Authorization"))
        if ARGS.fail_transcribe:
            self.send_error_json(ARGS.fail_transcribe, "mock_failure",
                                 "requested by --fail-transcribe")
            return
        time.sleep(ARGS.delay)
        self.send_json(200, {"sessionId": "mock", "transcript": ARGS.transcript,
                             "audioDurationMs": int(1000 * len(samples) / rate), "turns": []})

    def chat(self, body: bytes) -> None:
        try:
            request = json.loads(body)
        except ValueError as exc:
            self.send_error_json(400, "invalid_json", str(exc))
            return
        messages = request.get("messages", [])
        images = []
        for message in messages:
            content = message.get("content")
            if isinstance(content, list):
                for part in content:
                    if part.get("type") != "image_url":
                        continue
                    url = part["image_url"]["url"]
                    prefix = "data:image/jpeg;base64,"
                    if not url.startswith(prefix):
                        self.send_error_json(400, "bad_image", url[:40])
                        return
                    data = base64.b64decode(url[len(prefix):], validate=True)
                    images.append({"bytes": len(data), "size": jpeg_size(data),
                                   "role": message["role"]})
                    if ARGS.save_image:
                        with open(ARGS.save_image, "wb") as out:
                            out.write(data)
        report("chat", model=request.get("model"), reasoning=request.get("reasoning_effort"),
               stream=request.get("stream"), max_tokens=request.get("max_completion_tokens"),
               roles=[m.get("role") for m in messages], images=images,
               last_user=next((m.get("content") if isinstance(m.get("content"), str) else
                               [p.get("text") for p in m["content"] if p.get("type") == "text"]
                               for m in reversed(messages) if m.get("role") == "user"), None),
               authorization=self.headers.get("Authorization"))
        if ARGS.fail_chat:
            self.send_error_json(ARGS.fail_chat, "mock_failure", "requested by --fail-chat")
            return
        time.sleep(ARGS.delay)
        if not request.get("stream"):
            self.send_json(200, {"choices": [{"index": 0, "finish_reason": "stop", "message": {
                "role": "assistant", "content": ARGS.reply, "reasoning_content": ""}}]})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()

        def write_event(text: str) -> None:
            data = f"data: {text}\n\n".encode()
            self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
            self.wfile.flush()

        chars = list(ARGS.reply)
        for i in range(0, len(chars), 4):
            write_event(json.dumps({"object": "chat.completion.chunk", "choices": [{
                "index": 0, "delta": {"role": "assistant", "content": "".join(chars[i:i + 4]),
                                      "reasoning_content": ""}, "finish_reason": None}]},
                                   ensure_ascii=False))
            time.sleep(0.03)
        write_event(json.dumps({"object": "chat.completion.chunk", "choices": [{
            "index": 0, "delta": {}, "finish_reason": "stop"}]}))
        write_event("[DONE]")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()
        self.close_connection = True


def main() -> None:
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--transcript", default="今日の天気はどうかな")
    parser.add_argument("--reply",
                        default="[happy] こんにちは。僕はスタックチャンだよ。今日もよろしくね！")
    parser.add_argument("--delay", type=float, default=0.3)
    parser.add_argument("--fail-transcribe", type=int)
    parser.add_argument("--fail-chat", type=int)
    parser.add_argument("--save-image", help="save received JPEGs here (for local checks only)")
    ARGS = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    server = ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler)
    print(f"mock Muse on :{ARGS.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
