# ESP32-audioI2S vendored source

Upstream: https://github.com/schreibfaul1/ESP32-audioI2S

Pinned upstream commit: `928c420d49fce2a09fa91f490b9fcabed6447c67`

The local patch adds checked initialization and an acknowledged decoder-task
shutdown API. This prevents `Audio` storage, buffers, and mutexes from being
destroyed while `PeriodicTask` can still access them. The upstream GPL-3.0
license is preserved in `LICENSE`.
