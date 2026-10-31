#!/bin/bash
# Accept the current Linux output as the references of the named visual fixtures only, then run the suite.
#   scripts/update-linux-refs.sh SOURCE_ROOT fixture [fixture...]
# With no fixtures, nothing is updated. Run `python3 run.py --update` in spec/tests/visual to redo them all.
ROOT="${1:?usage: update-linux-refs.sh SOURCE_ROOT fixture...}"; shift
B="${BUILD_DIR:-$ROOT/build}"
cd "$ROOT/spec/tests/visual" || exit 1
for f in "$@"; do
  env -u DISPLAY -u WAYLAND_DISPLAY python3 run.py --exe "$B/calligrus" --update --only "$f/" 2>&1 | tail -1
done
env -u DISPLAY -u WAYLAND_DISPLAY python3 run.py --exe "$B/calligrus" 2>&1 | tail -2
