#!/bin/bash
# Build and test calligrus on Linux, then run the visual suite.
#   scripts/check-linux.sh [SOURCE_ROOT]       (default: the repository this script is in)
# Environment: BUILD_DIR (default <root>/build), VISUAL_ARGS (extra arguments for run.py).
# The visual suite runs under xvfb-run (invisible display) and compares with spec/tests/visual/refs/linux.
set -u
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
B="${BUILD_DIR:-$ROOT/build}"
cd "$ROOT" || exit 1
LOG="${TMPDIR:-/tmp}/calligrus-build.log"
echo "== build"; BUILD_DIR="$B" bash build.sh > "$LOG" 2>&1; echo "exit $?"; grep -E "error|warning:" "$LOG" | head -10
echo "== tests"; for t in "$B"/calligrus-*-test; do echo "$(basename "$t"): $("$t" 2>&1 | tail -1)"; done
echo "== visual"
cd "$ROOT/spec/tests/visual" || exit 1
env -u DISPLAY -u WAYLAND_DISPLAY python3 run.py --exe "$B/calligrus" ${VISUAL_ARGS:-} 2>&1 | tail -2
