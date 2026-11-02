"""Save what the CoreS3 LCD is showing right now as a PNG (real pixels, not a rendering).

    python tools/capture_screen.py docs/screenshots/v0.1.0/ready.png
"""
import argparse
from pathlib import Path

from PIL import Image

from device import Device


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port")
    parser.add_argument("--scale", type=int, default=2, help="nearest-neighbour upscaling")
    args = parser.parse_args()

    device = Device(args.port)
    width, height, pixels = device.screenshot()
    device.close()
    image = Image.frombytes("RGB", (width, height), pixels)
    if args.scale != 1:
        image = image.resize((width * args.scale, height * args.scale), Image.NEAREST)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output, optimize=True)
    print(f"{args.output}: {width}x{height}")


if __name__ == "__main__":
    main()
