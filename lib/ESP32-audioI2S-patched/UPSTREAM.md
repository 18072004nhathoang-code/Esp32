# ESP32-audioI2S vendored source

Upstream: https://github.com/schreibfaul1/ESP32-audioI2S

Pinned upstream commit: `928c420d49fce2a09fa91f490b9fcabed6447c67`

The local patch adds checked initialization and an acknowledged decoder-task
shutdown API. This prevents `Audio` storage, buffers, and mutexes from being
destroyed while `PeriodicTask` can still access them.

For Mini OS, the decoder `PeriodicTask` is also pinned to Core 0. Core 1 is
reserved for the LVGL render task, so starting AAC/M4A network playback cannot
schedule a same-priority decoder task onto the UI core. The 7 ms decoder sleep
uses `pdMS_TO_TICKS()` with a one-tick minimum to prevent a zero-delay hot loop
on configurations with a lower FreeRTOS tick rate.

The decoder worker stack is 5 KiB. Hardware MP3 stress showed that the former
3.3 KiB allocation left only 1.35 KiB free, below the project's 2 KiB release
margin.

The upstream GPL-3.0 license is preserved in `LICENSE`.
