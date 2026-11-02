"""Collect the flashable files of the last build into .release/<version>/.

The voice pack is not included: it is generated with VOICEVOX and stays on the device.

    python tools/make_release.py v0.1.0
"""
import argparse
import hashlib
import re
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio/build/m5stack-cores3"
BOOT_APP0 = (Path.home() / ".platformio/packages/framework-arduinoespressif32"
             / "tools/partitions/boot_app0.bin")

INSTRUCTIONS = """StackChan x Muse {version}

CoreS3 + StackChan body. Flash with esptool (change the COM port to match yours):

python -m esptool --chip esp32s3 --port COM4 --baud 921600 write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin

Do not erase the whole flash. NVS (Wi-Fi settings, servo calibration) and the LittleFS
voice pack stay in place. If the port is not found, hold the RST button for about 3
seconds to enter download mode.

The voice pack is not included. See README.md for how to create it.
firmware-elf.zip is for crash analysis only; you do not need it to flash.
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    args = parser.parse_args()

    version_header = (ROOT / "src/Version.h").read_text(encoding="utf-8")
    built = re.search(r'kAppVersion\[\] = "([^"]+)"', version_header).group(1)
    if built != args.version:
        raise SystemExit(f"src/Version.h says {built}, not {args.version}")

    out = ROOT / ".release" / args.version
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        shutil.copy2(BUILD / name, out / name)
    shutil.copy2(BOOT_APP0, out / "boot_app0.bin")
    with zipfile.ZipFile(out / "firmware-elf.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(BUILD / "firmware.elf", "firmware.elf")
    (out / "FLASH-INSTRUCTIONS.txt").write_text(
        INSTRUCTIONS.format(version=args.version), encoding="utf-8")

    sums = []
    for path in sorted(out.iterdir()):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        sums.append(f"{digest}  {path.name}")
    (out / "SHA256SUMS.txt").write_text("\n".join(sums) + "\n", encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
