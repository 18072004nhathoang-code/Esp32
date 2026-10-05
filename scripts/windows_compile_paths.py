"""PlatformIO pre-hook: avoid Windows GCC child-command overflow."""

Import("env")

import sys
from pathlib import Path

if sys.platform == 'win32':
    sys.path.insert(0, str(Path(env.subst('$PROJECT_DIR')) / 'scripts'))
    from windows_include_flags import compact_include_flags

    framework_dir = env.PioPlatform().get_package_dir('framework-arduinoespressif32')
    if not framework_dir:
        raise RuntimeError('Cannot resolve Arduino framework for Windows include arguments')
    original_include_flags = env['_CPPINCFLAGS']

    def windows_include_flags(target, source, env, for_signature):
        flags = env.subst_list(original_include_flags, target=target, source=source)[0]
        return compact_include_flags(flags, framework_dir)

    env.Replace(_CPPINCFLAGS=windows_include_flags)
    print('Windows GCC include prefix compaction enabled (CPPPATH unchanged)')
