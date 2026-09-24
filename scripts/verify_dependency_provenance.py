#!/usr/bin/env python3
"""Fail when audited vendored sources or direct dependency pins drift silently."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT_SHA256 = "34cd86f0f17c771861c1aefc651b9e89c65bb22799b5f45f0b791bf201b02770"
AUDIO_UPSTREAM_COMMIT = "928c420d49fce2a09fa91f490b9fcabed6447c67"
AUDIO_PATCH_VERSION = "2.0.0-mini-os.1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
    transport = ROOT / "src/ai/transport_ws.c"
    transport_hash = hashlib.sha256(transport.read_bytes()).hexdigest()
    require(transport_hash == TRANSPORT_SHA256,
            "transport_ws.c changed; audit it and update its recorded SHA-256")

    notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    require(TRANSPORT_SHA256 in notices,
            "THIRD_PARTY_NOTICES.md does not record the audited transport hash")

    audio_dir = ROOT / "lib/ESP32-audioI2S-patched"
    audio_metadata = json.loads((audio_dir / "library.json").read_text(encoding="utf-8"))
    upstream = (audio_dir / "UPSTREAM.md").read_text(encoding="utf-8")
    require(audio_metadata.get("version") == AUDIO_PATCH_VERSION,
            "vendored ESP32-audioI2S patch version drifted")
    require(audio_metadata.get("license") == "GPL-3.0",
            "vendored ESP32-audioI2S license metadata drifted")
    require(AUDIO_UPSTREAM_COMMIT in str(audio_metadata.get("description", "")) and
            AUDIO_UPSTREAM_COMMIT in upstream and AUDIO_UPSTREAM_COMMIT in notices,
            "vendored ESP32-audioI2S upstream commit metadata is inconsistent")

    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    required_pins = (
        "espressif32 @ 6.8.1",
        "lvgl/lvgl @ 8.3.11",
        "lovyan03/LovyanGFX @ 1.1.16",
        "bodmer/TJpg_Decoder @ 1.1.0",
        "bblanchon/ArduinoJson @ 6.21.5",
        "esp32_opus_arduino.git#a3816682932b8792f90072ee05c33fe25c055628",
    )
    missing = [pin for pin in required_pins if pin not in platformio]
    require(not missing, "missing exact dependency pins: " + ", ".join(missing))
    print("Dependency provenance and audited source hashes pass.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
