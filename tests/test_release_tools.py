"""Regression tests for the host-side hardware acceptance gates."""

from __future__ import annotations

import sys
import subprocess
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from hil_common import parse_health, validate  # noqa: E402
from package_release import validate_build_provenance  # noqa: E402
from verify_unprovisioned_config import configured_macros  # noqa: E402


def health_line(**overrides: int) -> str:
    fields = {
        "int_free": 64 * 1024,
        "int_min": 48 * 1024,
        "int_largest": 24 * 1024,
        "psram_free": 5 * 1024 * 1024,
        "psram_min": 4 * 1024 * 1024,
        "psram_largest": 3 * 1024 * 1024,
        "stack_min": 3072,
        "heartbeat_max": 1000,
    }
    fields.update(overrides)
    scalar = " ".join(f"{key}={value}" for key, value in fields.items())
    return f"[HEALTH] {scalar} lvgl_free=65536 queues=0/0 drops=0/0 stale=0\n"


class HilMonitorTests(unittest.TestCase):
    def test_valid_health_sample_and_queue_fields(self) -> None:
        text = health_line()
        validate(text, 60)
        sample = parse_health(text)[0]
        self.assertEqual(sample["queues_uplink"], 0)
        self.assertEqual(sample["queues_inbound"], 0)
        self.assertEqual(sample["stale"], 0)

    def test_fatal_signature_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "fatal serial signature"):
            validate(health_line() + "Task watchdog got triggered\n", 60)

    def test_stale_heartbeat_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "task heartbeat stalled"):
            validate(health_line(heartbeat_max=15001), 60)

    def test_memory_floor_is_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "health threshold failed"):
            validate(health_line(int_min=32767), 60)

    def test_memory_drift_is_rejected(self) -> None:
        text = health_line(int_free=64 * 1024) + health_line(int_free=55 * 1024)
        with self.assertRaisesRegex(SystemExit, "internal RAM drift"):
            validate(text, 60)

    def test_missing_samples_are_rejected(self) -> None:
        with self.assertRaisesRegex(SystemExit, "expected at least"):
            validate("boot only\n", 120)


class ReleaseConfigurationTests(unittest.TestCase):
    def test_python_cache_files_are_repository_ignored(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ignored = subprocess.run(
            ["git", "check-ignore", "-q", "scripts/__pycache__/release_tool.pyc"],
            cwd=root,
            check=False,
        )
        self.assertEqual(ignored.returncode, 0)

    def test_empty_template_is_unprovisioned(self) -> None:
        template = (Path(__file__).resolve().parents[1] / "include" /
                    "secrets.example.h").read_text(encoding="utf-8")
        self.assertEqual(configured_macros(template), set())

    def test_credentials_are_detected_without_exposing_values(self) -> None:
        text = '#define DEFAULT_WIFI_PASS "private value"\n'
        self.assertEqual(configured_macros(text), {"DEFAULT_WIFI_PASS"})

    def test_release_provenance_accepts_exact_clean_build(self) -> None:
        metadata = {
            "head_revision": "a" * 40,
            "firmware_revision": "a" * 12,
            "source_dirty": False,
        }
        validate_build_provenance(metadata, "a" * 40, "a" * 12, False)

    def test_release_provenance_rejects_stale_or_dirty_build(self) -> None:
        metadata = {
            "head_revision": "a" * 40,
            "firmware_revision": "a" * 12,
            "source_dirty": False,
        }
        with self.assertRaisesRegex(ValueError, "different Git HEAD"):
            validate_build_provenance(metadata, "b" * 40, "b" * 12, False)
        with self.assertRaisesRegex(ValueError, "dirty working tree"):
            validate_build_provenance(metadata, "a" * 40, "a" * 12 + "+wt123", True)


if __name__ == "__main__":
    unittest.main()
