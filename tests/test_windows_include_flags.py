import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from windows_include_flags import compact_include_flags


class WindowsIncludeFlagsTests(unittest.TestCase):
    ROOT = r'C:\Users\Builder Name\.platformio\packages\framework-arduinoespressif32@pinned'

    def test_preserves_order_and_ordinary_include_semantics(self):
        original = ['-Iinclude', '-I' + self.ROOT + r'\tools\sdk\include',
                    '-Ilib/local', '-I', self.ROOT + r'\cores\esp32', '-DKEEP=1']
        self.assertEqual(compact_include_flags(original, self.ROOT), [
            '-iprefix', self.ROOT.replace('\\', '/') + '/', '-Iinclude',
            '-iwithprefixbefore', 'tools/sdk/include', '-Ilib/local',
            '-iwithprefixbefore', 'cores/esp32', '-DKEEP=1'])
        self.assertNotIn('-isystem', compact_include_flags(original, self.ROOT))
        self.assertEqual(original[1], '-I' + self.ROOT + r'\tools\sdk\include')

    def test_windows_case_slashes_spaces_and_directory_boundary(self):
        source = ['-I' + self.ROOT.upper().replace('\\', '/') + '/cores/esp32',
                  '-I' + self.ROOT + '-other/include', '-I' + self.ROOT + r'\..\other']
        result = compact_include_flags(source, self.ROOT + '\\')
        self.assertEqual(result[2:4], ['-iwithprefixbefore', 'cores/esp32'])
        self.assertEqual(result[4:], source[1:])

    def test_no_framework_paths_and_incomplete_flags_are_unchanged(self):
        for flags in [['-Iinclude', '-I-'], ['-I'], ['-DONLY=1'], []]:
            self.assertEqual(compact_include_flags(flags, self.ROOT), flags)

    def test_existing_prefix_is_never_reinterpreted(self):
        for prefix in ['-iprefix', '-iprefix=other', '-iwithprefixbefore', '-iwithprefix']:
            flags = [prefix, 'other/', '-I' + self.ROOT + '/include']
            self.assertEqual(compact_include_flags(flags, self.ROOT), flags)

    def test_large_repeated_prefix_has_bounded_command_size(self):
        flags = ['-I' + self.ROOT + '/tools/sdk/include/part' + str(i) for i in range(160)]
        result = compact_include_flags(flags, self.ROOT)
        self.assertLess(len(' '.join(result)), len(' '.join(flags)) // 2)
        self.assertEqual(result.count('-iwithprefixbefore'), 160)


if __name__ == '__main__':
    unittest.main()
