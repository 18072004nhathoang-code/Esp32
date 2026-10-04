# Xiaozhi reset: evidence and verification

Baseline: `5536a92cd3b085caff99d88b150f1a20536913bd`, clean worktree.
Implementation branch: `codex/fix-xiaozhi-reset`; no merge into `main`.
Target: `esp32-s3-es3c28p`, ES3C28P ESP32-S3 N16R8, 240x320 portrait flipped,
rotation 2. GPIO, partitions, touch, activation and UI are unchanged.

## Pinned build inputs

| Component | Version |
| --- | --- |
| PlatformIO espressif32 | 6.8.1 |
| Arduino package | 3.20017.241212+sha.dcc1105b |
| Arduino core / ESP-IDF from board boot log | 2.0.17 / v4.4.7-dirty |
| LVGL / LovyanGFX | 8.3.11 / 1.1.16 |
| ESP32Encoder / TJpg_Decoder / ArduinoJson | 0.11.7 / 1.1.0 / 6.21.5 |
| esp32_opus | a3816682932b8792f90072ee05c33fe25c055628 |
| ESP32-audioI2S | local 2.0.0-mini-os.1 |

Opus uses `FIXED_POINT` and `USE_ALLOCA`. PCM/packet allocations in PSRAM do not
move Opus's internal `alloca` scratch off the calling task's internal stack.
ESP-IDF 4.4 reports task high-water in **bytes**, as documented in the pinned
`freertos/task.h`; multiplying by four exaggerated available stack.

## Confirmed findings and limits

No original Xiaozhi panic/backtrace was supplied or reproduced. The root cause
of the reported reset remains **unconfirmed**; neither stack overflow nor
brownout is established by the available logs.

- Original worker codec self-test left only 1,572 B of stack after correcting
  the reported units. Increasing only the Xiaozhi worker from 32,768 to 40,960 B
  and moving the shared stereo expansion off provider stacks produced 14,488 B
  of measured headroom. This is a measured risk reduction, not a decoded panic.
- Uplink and downlink batches are bounded at two packets per pass. Queue pressure
  preserves the unsent frame and uses a deadline; partial network writes close
  the stream rather than replaying a prefix. Cancel gates audio between packets
  with an atomic watermark, while the worker owns resource cleanup.
- Opus self-test failure remains terminal until service recovery; it cannot be
  overwritten by activation/preconnect readiness. Negotiated v2/v3 malformed
  packets cannot fall back to raw v1.
- On the real board, Music Pause released ownership while the decoder's I2S
  task remained alive. Duplex reinstall failed with `0x103`. Pause now retains
  the lease; STOP destroys the decoder after ACK before releasing ownership.
- Direct production transport fault injection proved `begin()` previously
  succeeded without an RX mutex and accepted SDK chunks with a missing byte
  range. Initialization now fails with rollback; SDK chunks must have contiguous
  offsets, matching length and opcode. Failed initialization can be retried.
- Preconnect and PTT now allocate generations under the same service mutex.

Diagnostics are rate-limited and include state/generation, recorder request and
status, ownership, queue depth/drop counters, codec state, stack headroom and
separate internal RAM/PSRAM free, minimum and largest blocks. Tokens,
Authorization headers, passwords and raw recorded audio are not emitted.

## Hardware evidence collected

COM10 board identified as ESP32-S3 revision v0.2, MAC `7c:e8:b1:b2:5a:30`.
No erase-all or NVS erase was performed.

At firmware `663fe351f79d`, production build/upload and board boot succeeded:

```text
[BOOT] Commit: 663fe351f79d
[SELF_TEST] Firmware C++ contracts: PASS
Xiaozhi Opus self-test=PASS encoded=170 decoded=960 stack_free=14488B minimum=4096B
```

FT6336, ES8311, SD and WiFi were detected/ready. Reset reason during the deliberate
USB serial reset was ROM `USB_UART_CHIP_RESET`; the IDF API reported UNKNOWN (0).
This is not a captured spontaneous reset.

The guarded `esp32-s3-es3c28p-music-stress` environment ran 600 seconds on this
board, using real SD files and repeated Play/Next/Pause/Resume/Prev/Stop. It
completed ten Music ownership sessions without panic/reset or duplex errors:

```text
[HW_STRESS] BEGIN heap=72376 tasks=18
[HW_STRESS] COMPLETE heap=72232 delta=-144 tasks=18 delta=0
```

Production firmware was restored afterwards. This Music-only stress result
does **not** stand in for live Xiaozhi conversation or Music-to-Xiaozhi handoff.
Later transport changes require their own build and board boot verification.

## Repeatable host verification

Behavioral and backend commands:

```text
node simulator/regression_test.js
npm test --prefix backend
```

All native commands are in `.github/workflows/build.yml`. The additional test
links the production transport source, not a duplicate transport model:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Itests/mocks/xiaozhi -Iinclude -Isrc/ai tests/xiaozhi_transport_test.cpp src/ai/xiaozhi_transport.cpp -o /tmp/xiaozhi_transport_test
/tmp/xiaozhi_transport_test
pio run -e esp32-s3-es3c28p
python scripts/run_pinned_opus_test.py --env esp32-s3-es3c28p
```

On this Windows workstation MinGW has a separate `cc1plus.exe 0xc0000022`
problem. Zig 0.13.0 compiled/ran native tests with UBSan. Windows ASan linking is
unavailable in that toolchain; the CI command retains ASan on Linux. For the
pinned Opus test with Zig, use `--cc <absolute-path-to-zig.exe> --zig-driver`.
These tests exercise queue pressure, 100 host reconnects, cancellation during a
send, stale generations, fragmentation, allocation/init failure and teardown.
Host reconnect counts must not be reported as board Start/Stop/Cancel counts.

## Flash and capture the matching ELF

Run only after confirming the board's current port and MAC:

```text
pio device list
pio run -e esp32-s3-es3c28p
pio run -e esp32-s3-es3c28p -t upload --upload-port COM10
python scripts/serial_capture.py --port COM10 --seconds 30 --output .pio/boot.log
python scripts/serial_capture.py --port COM10 --no-reset --seconds 1800 --output .pio/xiaozhi-stress.log
```

Use PlatformIO's Python if the system Python lacks pyserial. Preserve
`.pio/build/esp32-s3-es3c28p/firmware.elf`, `firmware.bin`, the complete log and the
boot SHA together before another build. `--no-reset` observes the current
session without deliberately toggling reset; serial behavior still depends on
the board's USB/bridge implementation. A monitor already holding the port must
be closed first.

Decode panic addresses using **that same ELF**:

```text
pio device monitor --port COM10 --baud 115200 --filter esp32_exception_decoder
xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/esp32-s3-es3c28p/firmware.elf <PC/backtrace addresses>
```

The Xtensa executable resides in PlatformIO's pinned
`packages/toolchain-xtensa-esp32s3/bin/` directory. Do not decode an old panic
using an ELF built from a different SHA.

## Remaining board acceptance gate — NOT_TESTED

Record at least 100 actual PTT/Stop/Cancel cycles, including these cases:

| Trigger | Required observation |
| --- | --- |
| Release during STARTING/handshake | Cancel acknowledged; no late recording start |
| Cancel while capture/upload/TTS is active | No next audio after cancellation; recorder stops and ownership clears |
| Slow/lost WiFi and reconnect | Bounded error/cleanup; next PTT succeeds |
| Music playing or paused before PTT | STOP/decoder ACK before voice takes I2S; restore bookmark only when appropriate |
| MCP Pause/Stop during voice | Music remains paused/stopped after cleanup |
| Close/reopen AI app | Old generation cannot send audio or change new state |
| Open Camera and scan WiFi between turns | Shared resources remain usable |

Then run 15–30 minutes of real conversation with WiFi interruption and
Music-to-Xiaozhi-to-Music transitions. Record heap free/largest/minimum, PSRAM,
stack, task count, generation, recorder status, ownership and queue drops at
stable idle points and during load. After final ACK/cleanup, task/owner counts
must return to baseline and idle heap must not decrease progressively. A panic,
reset, accumulating allocation loss or stuck RECOVERY_REQUIRED is a failure.
Decode any backtrace before attributing it to a task. A logged brownout requires
measuring the board supply, cable and amplifier current; software cannot prove
that hardware power issue fixed.
