#!/usr/bin/env bash
# Sync the source to akflame5 (Rocky 9.7, the target), build and test there.
# Builds in the user's home only; never touches /usr/OFX/Plugins or the live
# ComfyUI install. The one-time install symlink is done by hand with sudo:
#   sudo ln -s ~/LTX_OFX_build/build/LTXAlphaGen.ofx.bundle /usr/OFX/Plugins/
set -euo pipefail
HOST=${HOST:-akflame5}
REMOTE=${REMOTE:-LTX_OFX_build}
SRC="$(cd "$(dirname "$0")/.." && pwd)"

ssh "$HOST" "mkdir -p $REMOTE/src"
rsync -a --delete --exclude .git --exclude build --exclude __pycache__ \
  "$SRC/" "$HOST:$REMOTE/src/"
ssh "$HOST" "set -e; cd $REMOTE && cmake -S src -B build -DCMAKE_BUILD_TYPE=Release >/dev/null \
  && cmake --build build -j 8 2>&1 | grep -E 'error|warning' || true; \
  cd build && ctest --output-on-failure"
