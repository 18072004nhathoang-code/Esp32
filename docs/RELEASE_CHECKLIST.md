# Stable device release checklist

The automated CI gate must pass before hardware acceptance starts. The release
target is the ES3C28P N16R8 only; OTA, local DVP camera, MJPEG, RTSP and ONVIF
are not release features.

## Automated gate

- Build release, music-stress and diagnostic environments with PlatformIO 6.1.16.
- Pass native C++ contracts, backend tests and simulator regression under Node 20.
- Pass partition, warning and 120 KB RAM / 3 MB flash budget checks.
- Package the bootloader, partition table, application binary, ELF/map files,
  source revision, sizes and SHA-256 hashes with `scripts/package_release.py`.
- Packaging is fail-closed: the build provenance must match the exact current
  Git HEAD/source fingerprint and the working tree must be clean. Re-run
  `pio run -e esp32-s3-es3c28p` after committing; `--allow-dirty` is only for
  explicitly labeled developer bundles.
- Confirm CI release artifacts use the empty `secrets.example.h` configuration;
  never publish a locally provisioned firmware image containing device secrets.

## Reference-board gate

- Install serial tooling with `python3 -m pip install -r scripts/requirements-hil.txt`.
- Factory-reset one ES3C28P N16R8 board and verify 16 MB flash / 8 MB OPI PSRAM.
- Verify display color/rotation, full-screen touch, shared FT6336/ES8311 I2C,
  microphone, speaker, active-low amplifier and SDMMC.
- Run twenty cold/warm boots and reject panic, Guru Meditation, watchdog,
  stack-overflow, brownout, assertion or unexpected reset signatures.
- Run the eight-hour acceptance sequence: app open/close, sleep/wake, Wi-Fi
  loss/recovery, SD and YouTube music, twenty Xiaozhi sessions, maps, camera
  snapshots and repeated cancellation.
- Confirm internal heap low-water >= 32 KB, largest internal block >= 16 KB,
  largest PSRAM block >= 2 MB and every observed task stack margin >= 2 KB.
- Confirm post-soak internal/PSRAM drift stays within 8 KB / 32 KB.

The pinned Arduino-ESP32 SDK initializes the task watchdog at five seconds.
LVGL and the bounded audio worker are registered with that watchdog; this is
stricter than the stabilization target's eight-second upper bound. Network
workers remain deadline/heartbeat monitored because they can legitimately wait
on sockets.

This is a development-device release. Do not claim production security until
the exclusions in `docs/PRODUCTION_SECURITY.md` are resolved.
