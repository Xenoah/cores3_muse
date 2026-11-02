"""USB serial helper for the StackChan x Muse firmware."""
from __future__ import annotations

import base64
import json
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A


def find_port() -> str:
    ports = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not ports:
        raise SystemExit("CoreS3 not found on USB")
    return ports[0]


class Device:
    def __init__(self, port: str | None = None):
        self.serial = serial.Serial()
        self.serial.port = port or find_port()
        self.serial.baudrate = 115200
        self.serial.timeout = 0.2
        # Keep DTR/RTS low: the USB-Serial-JTAG peripheral resets the chip on some edges.
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.buffer = b""

    def close(self) -> None:
        self.serial.close()

    def send(self, line: str) -> None:
        self.serial.write(line.encode("utf-8") + b"\n")
        self.serial.flush()

    def read_line(self, timeout: float = 5.0) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line, self.buffer = self.buffer[:newline], self.buffer[newline + 1:]
                return line.rstrip(b"\r").decode("utf-8", "replace")
            self.buffer += self.serial.read(self.serial.in_waiting or 1)
        return None

    def wait_for(self, prefix: str, timeout: float = 10.0, echo: bool = False) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(deadline - time.monotonic())
            if line is None:
                break
            if echo and not line.startswith("#L "):
                print(line)
            if line.startswith(prefix):
                return line
        raise TimeoutError(f"no {prefix!r} within {timeout}s")

    def command(self, line: str, timeout: float = 10.0) -> str:
        self.serial.reset_input_buffer()
        self.buffer = b""
        self.send(line)
        return self.wait_for("#", timeout)

    def status(self, timeout: float = 5.0) -> dict:
        self.send("status")
        return json.loads(self.wait_for("#STATUS ", timeout)[len("#STATUS "):])

    def screenshot(self, retries: int = 3):
        """Read the real LCD contents. Returns (width, height, rgb888 bytes)."""
        for _ in range(retries):
            self.serial.reset_input_buffer()
            self.buffer = b""
            self.send("shot")
            width, height = map(int, self.wait_for("#SHOT ", 10).split()[1:3])
            rows: dict[int, bytes] = {}
            while True:
                line = self.read_line(10)
                if line is None or line.startswith("#END"):
                    break
                if not line.startswith("#L "):
                    continue
                parts = line.split(" ")
                try:
                    row = base64.b64decode(parts[3], validate=True)
                except (IndexError, ValueError):
                    continue
                if len(row) == width * 3 and sum(row) == int(parts[2]):
                    rows[int(parts[1])] = row
            if len(rows) == height:
                return width, height, b"".join(rows[y] for y in range(height))
            print(f"screenshot incomplete: {len(rows)}/{height} rows, retrying")
        raise RuntimeError("screenshot failed")
