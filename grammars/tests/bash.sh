#!/usr/bin/env bash
set -euo pipefail

# Build every project
readonly OUT_DIR="${HOME}/out"
count=0

build() {
    local name="$1"
    echo "building ${name} into $OUT_DIR"
    if [[ -d "$name" ]]; then
        (cd "$name" && make -j4) || return 1
    fi
    count=$((count + 1))
}

for p in app lib tools; do
    build "$p" 2>/dev/null
done
echo "built $count projects" | tee -a log.txt
