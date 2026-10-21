# SPDX-License-Identifier: GPL-3.0-or-later
import resource
import signal
import subprocess
import sys

# Applied before spawning; inherited by both children, no core files.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
for mode in ("cooperative", "running"):
    result = subprocess.run([sys.argv[1], mode], timeout=3, check=False)
    expected = 0 if mode == "cooperative" else -signal.SIGILL
    if result.returncode != expected:
        raise SystemExit(f"{mode}: expected {expected}, got {result.returncode}")
    print(f"{mode}: returncode={result.returncode}")
