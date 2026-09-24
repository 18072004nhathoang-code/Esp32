# Third-party notices

This project is MIT-licensed, but the firmware and build tools include components
under their own licenses. This inventory records the direct dependencies pinned
by `platformio.ini`; it is attribution information, not legal advice.

| Component | Version / source | License | Upstream |
|---|---|---|---|
| LVGL | 8.3.11 | MIT | https://github.com/lvgl/lvgl |
| LovyanGFX | 1.1.16 | FreeBSD/BSD-2-Clause, with retained upstream notices | https://github.com/lovyan03/LovyanGFX |
| ESP32-audioI2S | vendored from `928c420d49fce2a09fa91f490b9fcabed6447c67`; local shutdown patch 2.0.0-mini-os.1 | GPL-3.0 | https://github.com/schreibfaul1/ESP32-audioI2S |
| TJpg_Decoder | 1.1.0 | FreeBSD/BSD-2-Clause; bundled Tiny JPEG Decompressor notice by ChaN | https://github.com/Bodmer/TJpg_Decoder |
| ArduinoJson | 6.21.5 | MIT | https://github.com/bblanchon/ArduinoJson |
| esp32_opus | commit `a3816682932b8792f90072ee05c33fe25c055628` (package 1.0.3) | GPL-3.0 as distributed by the package repository | https://github.com/sh123/esp32_opus_arduino |
| Arduino-ESP32 | 2.0.17 (via PlatformIO Espressif32 6.8.1) | LGPL-2.1-or-later for Arduino core plus Apache-2.0 and component-specific licenses | https://github.com/espressif/arduino-esp32 |
| Be Vietnam Pro SemiBold | SHA-256 `bd8e27eb02720b9d91e59e4f10a90878643219f25ce6a8d9a4f06a8a88d3bb71` | SIL Open Font License 1.1 | https://github.com/bettergui/Be-Vietnam-Pro |
| lv_font_conv | 1.5.3 | MIT | https://github.com/lvgl/lv_font_conv |

Dependency license texts are retained in each resolved PlatformIO package; the
vendored ESP32-audioI2S license and provenance are in
`lib/ESP32-audioI2S-patched/`. The Be Vietnam Pro OFL text is distributed in
`LICENSES/OFL-1.1.txt`.

## Binary release obligations

- Retain this notice, the project MIT license, font OFL notice, and notices
shipped by LovyanGFX and TJpg_Decoder.
- A firmware binary linked with ESP32-audioI2S and esp32_opus is a GPL-3.0 distribution. A
  release must provide the complete Corresponding Source for the firmware and
  the exact dependency source, build scripts, configuration and installation
  information required by GPL-3.0. Tag the released source and archive the exact
  commit listed above; do not rely on a mutable tag.
- Preserve source offers and license texts for the Arduino-ESP32/ESP-IDF
  components included in the produced image. Review the resolved PlatformIO
  package notices for every release because transitive components may change.

## Local compatibility source provenance

- `src/ai/transport_ws.c` is an Apache-2.0 Espressif WebSocket transport source
  retained locally for the Arduino-ESP32 2.0.17 transport API. The audited file
  SHA-256 is `76372a79ec6b870b72c84dc3da21b8bf6b9afd278193066cd59d8c817b922416`;
  its copyright and license header are preserved in the file.
- `scripts/framework_wifi_patch.py` generates build-directory-only adaptations
  from Arduino-ESP32 package `3.20017.241212+sha.dcc1105b`. It verifies exact
  SHA-256 values for `WiFiScan.cpp`, `WiFiScan.h` and `WiFiGeneric.cpp` before
  applying the documented scan/recovery fixes and never modifies the installed
  framework package.
