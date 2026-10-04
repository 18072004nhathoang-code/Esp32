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

### Live connection repair, 2026-10-05

COM10 testing reproduced `mbedtls_ssl_setup -0x7F00` (allocation failure) and
`mbedtls_ssl_handshake -0x2700`, with trust-chain flag `0x8`. The board had a
valid epoch and roughly 77 KB internal free before connecting, while PSRAM had
over 7 MB free. A PC verified the endpoint using the firmware's unchanged PEM
CA bundle. The pinned SDK sets `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`.

`src/core/tls_memory.cpp` installs the mbedTLS allocator once at boot, before
network workers start: zeroed PSRAM allocations, checked internal fallback,
overflow protection and matching `heap_caps_free`. After **only the memory
policy and diagnostic changes**, the real board completed verified WSS/hello,
recorder START/STOP ACK, 20,992 captured samples, 22 sent Opus frames with zero
uplink drops, STT, TTS, first PCM, TTS STOP and I2S owner release. This confirms
the observed connection failure was resolved in that turn without replacing
the CA or weakening verification. It does not prove all cancellation/stress
cases or acoustic quality. A preceding turn was explicitly canceled and ACKed.
The user subsequently confirmed hearing the spoken answer. Further physical
turns also reached STT/TTS without a reset, but one later PTT exposed a separate
stale `CLEANUP_TIMEOUT` from the completed turn before its START command was
consumed. `finalize_cleanup` now clears the phase/timing and recorder/cleanup
metadata **before** publishing IDLE/admitting another generation. The native
session regression replays the actual 64s cleanup / 176s next-press timing.
The second change was then tested on the real board using the separate image
`bb6c8d17f535+wtcdb42550feb5`. Five physical turns (generations 2, 3, 5, 6,
7) reached first PCM, TTS STOP, DONE and I2S release. After generation 3's DONE
at 169,380 ms, the warm socket closed at 229,611 ms and reconnected. A new PTT
at 273,921 ms completed at 290,984 ms without the stale cleanup deadline.
First-PCM latency was 449–640 ms across these five turns. No spontaneous reset,
panic or TLS allocation failure appeared in that capture through 542 seconds.
The user confirmed hearing the answer after this idle/reconnect. A sixth turn
(generation 11) completed at 570,606 ms. App close occurred afterwards, so it
does not verify cancellation during TTS. A later real turn (generation 14)
reached first PCM at 997,121 ms. Touching the mic during SPEAKING logged
`BARGE_IN` at 1,006,059 ms, `CANCEL_REQUESTED` at 1,006,069 ms and `CANCEL_ACK`
at 1,006,079 ms, followed by I2S release and DONE at 1,006,092 ms. The user
confirmed the speaker stopped immediately. The next PTT (generation 15)
received recorder START/STOP ACK, STT and first PCM at 1,044,551 ms; the user
confirmed hearing a normal reply. A second barge-in was ACKed at 1,052,962 ms,
with I2S released and DONE at 1,052,976 ms. One in-flight audio packet returned
failure during that cancellation; no terminal codec fault was logged. This
verifies two real TTS cancellations and one next-turn recovery, not the
100-cycle acceptance gate.

The first TLS-repaired image identifies itself as `bb6c8d17f535+wtff864b48d55c`;
the final cleanup fix uses `bb6c8d17f535+wtcdb42550feb5`.
Workstation evidence is under
`.pio/handoff/bb6c8d1/live-20261005/`: initial `xiaozhi-live.log`,
`tls-diagnostic.log`, `tls-flags.log`, repaired `tls-psram.log/.elf/.bin`,
and final `tls-session-fixed.log/.elf/.bin`.
The directory is ignored by Git. Native TLS allocator testing passed with
UBSan; all eleven existing native executables, simulator regression, nine
backend tests and the ES3C28P build passed. The new allocator test is in CI
with ASan/UBSan. Cloud CI itself was not run from this workstation.

The acceptance gate below **still remains**; host allocation/transport loops
must not be counted as actual board PTT cycles.

### SD control discovery repair

On the final cleanup image, the board detected a FAT32 SD with four actual MP3
files in `/music`, but live Xiaozhi requests showed only MCP `initialize`, no
`tools/list` or `self.music.play`. The transport detached to generation 0 after
hello and rejected every idle inbound frame, so discovery requests could not
reach the service. The only tool page also advertised an empty `nextCursor`.

Idle transport now admits only bounded JSON MCP discovery/initialization;
the service authenticates its session and dispatches it at idle. Pending
read-only discovery survives hello/idle/PTT transitions, while old audio, TTS
and side-effecting calls remain rejected. MCP dedup is scoped to a connection,
not cleared after each warm voice turn. The final tool page omits `nextCursor`.
The play schema explicitly documents empty arguments as SD selection, not a
YouTube query. No SD driver, GPIO, filesystem or arbitrary-file access changed.
All 13 native tests, behavioral regression, nine backend tests, pinned Opus
and ES3C28P build passed. The new image `bb6c8d17f535+wt181a52049479` was flashed
to the same COM10 board without erasing NVS. Idle notifications/initialized
and tools/list were received at 40,675/40,687 ms, with ACK at 40,705 ms.
At 44,379 ms the server invoked self.music.play; the real decoder then opened
`/music/Người Dưng.mp3`. Three subsequent real PTT turns stopped the decoder,
restored duplex for capture/TTS and restored the SD track after DONE. The user
confirmed hearing music both before and after the answers. A later quick
release during STARTING (generation 7) canceled before recording and returned
ownership; it is not evidence for an MCP Stop command. Matching evidence:
`mcp-idle-fixed.log/.elf/.bin` in the ignored handoff directory.

The same image later received a real `self.music.stop` in generation 17 at
741,239 ms, sent its async MCP ACK at 741,256 ms, received TTS stop at
745,598 ms and completed cleanup at 745,812 ms. The voice output lease was
released, with no later MUSIC acquire in that capture, including another
normal spoken turn in generation 19. The user confirmed that the speaker
was silent after Stop. This verifies one MCP Stop suppression case, not
MCP Pause or the complete acceptance gate.

The 900-second capture completed normally: nine physical Starts, eight
STT/first-PCM turns, nine DONE events and one fast-STARTING Cancel ACK.
All logged audio leases were released; no FAULT, panic, backtrace or TLS
failure was observed. There were 22 physical Starts across the cleanup and
MCP images, not 100 cycles on the final image. The matching final image is
`bb6c8d17f535+wt181a52049479`.

A separate bounded `mcp-wifi-followup.log` capture uses the same running
image with `--no-reset`. The WiFi UI has no separate Disconnect button;
Forget removes saved credentials and is not the reconnect test. Selecting
the current network, entering its password on the device and pressing
Connect invokes the worker's disconnect/connect sequence. No reconnect
or post-reconnect voice success is yet verified in this follow-up. This UI
test closes AI first, so even a successful result would not prove recovery
from WiFi loss during an active capture/upload/TTS request.

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
