#!/usr/bin/env python3
"""Validate repeated USB-reset warm boots on the reference board."""

import argparse
import time
from pathlib import Path

from hil_common import FATAL, current_git_revision, validate_firmware_identity


REQUIRED_MARKERS = (
    "[SELF_TEST] Firmware C++ contracts: PASS",
    "[BOOT] Capability: chip=ESP32-S3 memory=PASS partitions=PASS coredump=READY",
    "[I2C] Probe FT6336 (0x38): DETECTED",
    "[I2C] Probe ES8311 (0x18): DETECTED",
    "[LCD] Status: Ready",
    "[TOUCH] Status: Detected & Ready",
    "[AUDIO] Status: Ready",
    "[SYSTEM] Mini OS Pro Max đã sẵn sàng hoạt động!",
)


def capture_boot(port: str, baud: int, timeout: float) -> str:
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required: python3 -m pip install pyserial") from error

    deadline = time.monotonic() + timeout
    chunks: list[bytes] = []
    with serial.Serial(port, baud, timeout=0.2) as connection:
        # USB-JTAG/serial maps RTS to chip enable and DTR to the boot strap.
        # Keep GPIO0 deasserted while pulsing EN for a normal warm boot.
        connection.dtr = False
        connection.rts = True
        time.sleep(0.1)
        connection.rts = False
        time.sleep(0.1)
        connection.reset_input_buffer()
        while time.monotonic() < deadline:
            data = connection.read(4096)
            if data:
                chunks.append(data)
                text = b"".join(chunks).decode("utf-8", errors="replace")
                if REQUIRED_MARKERS[-1] in text:
                    return text
    return b"".join(chunks).decode("utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-timeout", type=float, default=15.0)
    parser.add_argument("--revision", default="")
    parser.add_argument("--output", type=Path, default=Path("hil-boot-cycles.log"))
    args = parser.parse_args()
    if args.cycles < 1:
        raise SystemExit("--cycles must be positive")
    expected_revision = args.revision or current_git_revision()

    logs: list[str] = []
    for cycle in range(1, args.cycles + 1):
        text = capture_boot(args.port, args.baud, args.boot_timeout)
        fatal = FATAL.search(text)
        if fatal:
            raise SystemExit(f"cycle {cycle}: fatal serial signature: {fatal.group(0)}")
        missing = [marker for marker in REQUIRED_MARKERS if marker not in text]
        if missing:
            raise SystemExit(f"cycle {cycle}: boot did not reach required marker: {missing[0]}")
        validate_firmware_identity(text, expected_revision, "esp32-s3-es3c28p")
        logs.append(f"===== WARM BOOT {cycle}/{args.cycles} =====\n{text}")
        print(f"warm boot {cycle}/{args.cycles}: PASS", flush=True)
        time.sleep(0.25)

    args.output.write_text("\n".join(logs), encoding="utf-8")
    print(f"{args.cycles} warm boots passed; log: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
