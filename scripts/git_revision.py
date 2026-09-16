Import("env")

import subprocess

try:
    revision = subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"],
        cwd=env["PROJECT_DIR"],
        text=True,
    ).strip()
except (OSError, subprocess.CalledProcessError):
    revision = "unknown"

env.Append(CPPDEFINES=[("FW_GIT_SHA", env.StringifyMacro(revision))])
