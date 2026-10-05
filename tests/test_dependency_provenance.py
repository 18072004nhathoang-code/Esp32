import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from verify_dependency_provenance import (  # noqa: E402
    ROOT, TRANSPORT_SHA256, canonical_source_sha256,
)


class DependencyProvenanceTests(unittest.TestCase):
    def test_same_audited_source_across_git_newline_encodings(self):
        source = (ROOT / "src/ai/transport_ws.c").read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(canonical_source_sha256(source), TRANSPORT_SHA256)
        self.assertEqual(canonical_source_sha256(source.replace(b"\n", b"\r\n")), TRANSPORT_SHA256)

    def test_code_or_whitespace_change_is_not_accepted(self):
        source = (ROOT / "src/ai/transport_ws.c").read_bytes()
        self.assertNotEqual(canonical_source_sha256(source + b" "), TRANSPORT_SHA256)
        self.assertNotEqual(canonical_source_sha256(source.replace(b"return NULL;", b"return 0;", 1)), TRANSPORT_SHA256)
        self.assertNotEqual(canonical_source_sha256(b"a\rb"), canonical_source_sha256(b"a\nb"))
