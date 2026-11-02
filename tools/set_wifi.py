"""Store Wi-Fi settings on the CoreS3 over USB. The device reboots afterwards.

    python tools/set_wifi.py "MyNetwork"
"""
import argparse
import getpass

from device import Device


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ssid")
    parser.add_argument("--port")
    args = parser.parse_args()

    password = getpass.getpass("Wi-Fi password: ")
    device = Device(args.port)
    print(device.command(f"wifi {args.ssid}\t{password}"))
    device.close()


if __name__ == "__main__":
    main()
