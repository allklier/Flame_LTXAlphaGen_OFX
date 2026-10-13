#!/usr/bin/env bash
# Sync the source to a Mac (Apple Silicon, Command Line Tools), build and test there.
#   HOST=<ssh host> REMOTE=<folder on the Mac> [CMAKE=<cmake on the Mac>] scripts/build_mac.sh
# CMAKE_ARGS passes extra configure options (e.g. -DWORKER_TEST_PYTHON=<python with numpy>;
# the default /usr/bin/python3 of the Command Line Tools has no numpy, so the two Python tests fail).
# Writes only under $REMOTE (src/ and build/); never installs into /Library/OFX.
set -euo pipefail
HOST=${HOST:?set HOST to the ssh name of the Mac}
REMOTE=${REMOTE:?set REMOTE to the build folder on the Mac}
CMAKE=${CMAKE:-cmake}
if [[ $CMAKE == */* ]]; then CTEST=${CTEST:-${CMAKE%/*}/ctest}; else CTEST=${CTEST:-ctest}; fi
CMAKE_ARGS=${CMAKE_ARGS:-}
SRC="$(cd "$(dirname "$0")/.." && pwd)"

ssh "$HOST" "mkdir -p '$REMOTE/src'"
rsync -a --delete --exclude .git --exclude build --exclude __pycache__ --exclude docs \
  --exclude dist --exclude CLAUDE.local.md "$SRC/" "$HOST:$REMOTE/src/"
DEV_BUILD=${DEV_BUILD:-OFF}
ssh "$HOST" "set -e; cd '$REMOTE' && '$CMAKE' -S src -B build -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=$DEV_BUILD $CMAKE_ARGS >/dev/null \
  && '$CMAKE' --build build -j 8 2>&1 | grep -E 'error|warning' || true; \
  cd build && '$CTEST' --output-on-failure"
