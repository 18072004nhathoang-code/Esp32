# Stable device release checklist

The automated CI gate must pass before hardware acceptance starts. The release
target is the ES3C28P N16R8 only; OTA, local DVP camera, MJPEG, RTSP and ONVIF
are not release features.

## Automated gate

- Build release, music-stress and diagnostic environments with PlatformIO 6.1.16.
- Pass native C++ contracts, backend tests and simulator regression under Node 20.
- Pass partition, warning and 120 KB RAM / 3 MB flash budget checks.
- Pass `scripts/verify_dependency_provenance.py`; audited shim hashes, vendored
  audio metadata and every direct PlatformIO pin must agree with notices.
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
  Automate the warm-reset portion with
  `python3 scripts/hil_boot_cycles.py --port "$ESP32_PORT" --revision "$(git rev-parse --short=12 HEAD)"`;
  power-cycle the board separately to cover cold boot.
- Flash the `esp32-s3-es3c28p-music-stress` environment and run
  `python3 scripts/hil_music_stress.py --port "$ESP32_PORT"`; require the
  complete 30-minute SD playback sequence to pass before restoring release firmware.
- Run the eight-hour acceptance sequence: app open/close, sleep/wake, Wi-Fi
  loss/recovery, SD and YouTube music, twenty Xiaozhi sessions, maps, camera
  snapshots and repeated cancellation.
- Run `python3 scripts/hil_network_stress.py --port "$ESP32_PORT"` and complete
  twenty AP loss/recovery cycles, one hundred successful map refresh cycles and
  one hundred camera open/close cycles with a published snapshot. The monitor
  resets the board first and rejects a run whose lifetime event counters miss
  any threshold.
- `scripts/hil_acceptance.py` also resets the board first and requires twenty
  Xiaozhi sessions to reach listening, receive STT/TTS and complete. Faults
  deliberately injected to prove retryable cleanup remain visible in the
  lifetime counters but do not invalidate otherwise successful recovery.
- Exercise every release task at least once before ending the soak. In
  particular, change and save one Settings value (ID 10), record audio and use
  the UI action that exports the WAV to SD (ID 11), and run the speaker
  self-test to completion (ID 12). Opening/using Music, Map, Camera and
  Xiaozhi covers IDs 3, 5, 6–9; Main/LVGL/Audio/Wi-Fi cover IDs 0, 1, 2 and 4
  during normal operation. Restore the changed Settings value afterward if
  needed.
- Confirm internal heap low-water >= 32 KB, largest internal block >= 16 KB,
  largest PSRAM block >= 2 MB and every observed task stack margin >= 2 KB.
- Confirm post-soak internal/PSRAM drift stays within 8 KB / 32 KB.
- Confirm the final `tasks_seen` mask includes release task IDs 0 through 12;
  completed short-lived workers retain their historical minimum stack margin.

The pinned Arduino-ESP32 SDK initializes the task watchdog at five seconds.
LVGL and the bounded audio worker are registered with that watchdog; this is
stricter than the stabilization target's eight-second upper bound. Network
workers remain deadline/heartbeat monitored because they can legitimately wait
on sockets.

This is a development-device release. Do not claim production security until
the exclusions in `docs/PRODUCTION_SECURITY.md` are resolved.
