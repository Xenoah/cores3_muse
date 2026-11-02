"""Build and run the ReplySplitter host test. Needs: pip install ziglang"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    with tempfile.TemporaryDirectory() as temp:
        exe = Path(temp) / "reply_test.exe"
        subprocess.run([sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1",
                        "-I", str(ROOT / "src"),
                        str(ROOT / "tools/reply_test/reply_test.cpp"),
                        str(ROOT / "src/ReplySplitter.cpp"),
                        "-o", str(exe)], check=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    sys.exit(main())
