"""Store the Meta Model API key on the CoreS3 over USB.

The key is typed at a hidden prompt (or read from MODEL_API_KEY), so it stays out of
shell history and logs. Get one at https://dev.meta.ai/ -> API keys -> Create API key.

    python tools/set_key.py
"""
import argparse
import getpass
import os

from device import Device


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    args = parser.parse_args()

    key = os.environ.get("MODEL_API_KEY") or getpass.getpass("Model API key: ")
    key = key.strip()
    if not key.startswith("LLM_"):
        raise SystemExit("Model API keys start with LLM_")

    device = Device(args.port)
    print(device.command(f"setkey {key}"))
    device.close()


if __name__ == "__main__":
    main()
