Import("env")

import hashlib
import os
import subprocess

try:
    revision = subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"],
        cwd=env["PROJECT_DIR"],
        text=True,
    ).strip()
    diff = subprocess.check_output(
        ["git", "diff", "--no-ext-diff", "--binary", "HEAD"],
        cwd=env["PROJECT_DIR"],
    )
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=env["PROJECT_DIR"],
    ).split(b"\0")
    if diff or any(untracked):
        source_hash = hashlib.sha256(diff)
        for relative_bytes in sorted(path for path in untracked if path):
            relative = os.fsdecode(relative_bytes)
            source_hash.update(relative_bytes)
            with open(os.path.join(env["PROJECT_DIR"], relative), "rb") as source_file:
                source_hash.update(source_file.read())
        revision += "+wt" + source_hash.hexdigest()[:12]
except (OSError, subprocess.CalledProcessError):
    revision = "unknown"

env.Append(CPPDEFINES=[("FW_GIT_SHA", env.StringifyMacro(revision))])
