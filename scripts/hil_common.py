"""Shared serial soak monitor for operator-driven hardware-in-loop tests."""

from __future__ import annotations

import re
import time
from pathlib import Path

FATAL = re.compile(
    r"Guru Meditation|Core \d panic|Task watchdog|TASK_WDT|INT_WDT|"
    r"watchdog.*triggered|stack overflow|Brownout detector|assert failed|"
    r"panic'ed|abort\(\)|Backtrace:|unexpected reset",
    re.IGNORECASE,
)
HEALTH = re.compile(r"\[HEALTH\]\s+(.*)")


def capture(port: str, baud: int, duration: int, output: Path) -> str:
    try:
        import serial
    except ImportError as error:
        raise SystemExit("pyserial is required: python3 -m pip install pyserial") from error
    deadline = time.monotonic() + duration
    chunks: list[bytes] = []
    with serial.Serial(port, baud, timeout=0.25) as connection:
        while time.monotonic() < deadline:
            data = connection.read(4096)
            if data:
                chunks.append(data)
                print(data.decode("utf-8", errors="replace"), end="", flush=True)
    text = b"".join(chunks).decode("utf-8", errors="replace")
    output.write_text(text, encoding="utf-8")
    return text


def parse_health(text: str) -> list[dict[str, int]]:
    samples: list[dict[str, int]] = []
    for match in HEALTH.finditer(text):
        fields = {}
        for key, value in re.findall(r"([a-z_]+)=(\d+)", match.group(1)):
            fields[key] = int(value)
        for key, first, second in re.findall(r"(queues|drops)=(\d+)/(\d+)", match.group(1)):
            fields[f"{key}_uplink"] = int(first)
            fields[f"{key}_inbound"] = int(second)
        if fields:
            samples.append(fields)
    return samples


def validate(text: str, duration: int, required_tasks_mask: int = 0) -> None:
    fatal = FATAL.search(text)
    if fatal:
        raise SystemExit(f"fatal serial signature: {fatal.group(0)}")
    samples = parse_health(text)
    minimum_samples = max(1, duration // 60)
    if len(samples) < minimum_samples:
        raise SystemExit(f"only {len(samples)} health samples; expected at least {minimum_samples}")
    required = {
        "int_min": 32768,
        "int_largest": 16384,
        "psram_largest": 2 * 1024 * 1024,
        "stack_min": 2048,
    }
    for key, floor in required.items():
        observed = min(sample.get(key, 0) for sample in samples)
        if observed < floor:
            raise SystemExit(f"health threshold failed: {key}={observed}, required >= {floor}")
    heartbeat_ceiling_ms = 15000
    oldest_heartbeat = max(sample.get("heartbeat_max", heartbeat_ceiling_ms + 1)
                           for sample in samples)
    if oldest_heartbeat > heartbeat_ceiling_ms:
        raise SystemExit(
            f"task heartbeat stalled: age={oldest_heartbeat} ms, "
            f"required <= {heartbeat_ceiling_ms} ms"
        )
    if required_tasks_mask:
        seen_mask = 0
        for sample in samples:
            seen_mask |= sample.get("tasks_seen", 0)
        missing = required_tasks_mask & ~seen_mask
        if missing:
            raise SystemExit(
                f"required tasks were not exercised: missing mask=0x{missing:x}, "
                f"seen=0x{seen_mask:x}"
            )
    first, last = samples[0], samples[-1]
    if first.get("int_free", 0) - last.get("int_free", 0) > 8192:
        raise SystemExit("internal RAM drift exceeds 8 KB")
    if first.get("psram_free", 0) - last.get("psram_free", 0) > 32768:
        raise SystemExit("PSRAM drift exceeds 32 KB")
