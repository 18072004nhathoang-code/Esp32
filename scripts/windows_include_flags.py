"""Shorten GCC include arguments without changing include search order.

GCC's -iwithprefixbefore has the same placement as -I (not -isystem).
Only the pinned framework's repeated absolute prefix is factored out.
CPPPATH itself remains unchanged for SCons dependency scanning.
"""

import ntpath


def compact_include_flags(flags, framework_dir):
    flags = list(flags)
    if any(str(flag).startswith(('-iprefix', '-iwithprefix')) for flag in flags):
        # An existing prefix may have a different scope; never reinterpret it.
        return flags
    root = ntpath.normpath(str(framework_dir)).rstrip('\\/')
    boundary = ntpath.normcase(root + '\\')
    result = []
    changed = False
    index = 0
    while index < len(flags):
        flag = str(flags[index])
        separated = flag == '-I' and index + 1 < len(flags)
        path = str(flags[index + 1]) if separated else flag[2:] if flag.startswith('-I') else ''
        normalized = ntpath.normpath(path) if path and path != '-' else ''
        if normalized and ntpath.normcase(normalized).startswith(boundary):
            suffix = normalized[len(root) + 1:].replace('\\', '/')
            result.extend(['-iwithprefixbefore', suffix])
            changed = True
            index += 2 if separated else 1
        else:
            result.append(flags[index])
            index += 1
    if not changed:
        return flags
    # GCC applies this prefix only to subsequent -iwithprefixbefore arguments.
    return ['-iprefix', root.replace('\\', '/') + '/'] + result
