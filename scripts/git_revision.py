Import("env")

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(env["PROJECT_DIR"]) / "scripts"))
from config_provenance import inspect_config

try:
    full_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"],
        cwd=env["PROJECT_DIR"],
        text=True,
    ).strip()
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
    source_dirty = bool(diff or any(untracked))
    if source_dirty:
        source_hash = hashlib.sha256(diff)
        for relative_bytes in sorted(path for path in untracked if path):
            relative = os.fsdecode(relative_bytes)
            source_hash.update(relative_bytes)
            with open(os.path.join(env["PROJECT_DIR"], relative), "rb") as source_file:
                source_hash.update(source_file.read())
        revision += "+wt" + source_hash.hexdigest()[:12]
except (OSError, subprocess.CalledProcessError):
    full_revision = "unknown"
    revision = "unknown"
    source_dirty = True

build_dir = Path(env.subst("$BUILD_DIR"))
build_dir.mkdir(parents=True, exist_ok=True)
config = inspect_config(Path(env["PROJECT_DIR"]) / "include" / "secrets.h")
(build_dir / "source_revision.json").write_text(
    json.dumps({
        "config_fields": list(config.configured_fields),
        "config_present": config.present,
        "config_provisioned": config.provisioned,
        "config_sha256": config.sha256,
        "firmware_revision": revision,
        "head_revision": full_revision,
        "source_dirty": source_dirty,
    }, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)

env.Append(CPPDEFINES=[
    ("FW_GIT_SHA", env.StringifyMacro(revision)),
    ("FW_BUILD_ENV", env.StringifyMacro(env["PIOENV"])),
])
