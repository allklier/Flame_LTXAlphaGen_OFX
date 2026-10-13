#!/usr/bin/env bash
# Build the macOS release tarball on an Apple Silicon Mac (Command Line Tools) and copy it back
# to dist/ (decision 65).
#   HOST=<ssh host> REMOTE=<folder on the Mac> [CMAKE=<cmake on the Mac>] [CMAKE_ARGS=...] \
#     scripts/package_release_mac.sh  ->  dist/LTXAlphaGen-<version>-macos-arm64.tar.gz
# Contents: LTXAlphaGen.ofx.bundle (release build), install.sh, README.md, LICENSE.
# CMAKE_ARGS as for build_mac.sh (e.g. -DWORKER_TEST_PYTHON=<python with numpy>, the libc++
# header workaround). Builds in $REMOTE/release; never installs anything.
set -euo pipefail
HOST=${HOST:?set HOST to the ssh name of the Mac}
REMOTE=${REMOTE:?set REMOTE to the build folder on the Mac}
CMAKE=${CMAKE:-cmake}
if [[ $CMAKE == */* ]]; then CTEST=${CTEST:-${CMAKE%/*}/ctest}; else CTEST=${CTEST:-ctest}; fi
CMAKE_ARGS=${CMAKE_ARGS:-}
SRC="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=$(sed -n 's/^set(LTXAG_VERSION "\([0-9.]*\)")/\1/p' "$SRC/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "no LTXAG_VERSION in CMakeLists.txt" >&2; exit 1; }
NAME="LTXAlphaGen-$VERSION"

ssh "$HOST" "mkdir -p '$REMOTE/src'"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude docs --exclude __pycache__ \
  --exclude CLAUDE.local.md "$SRC/" "$HOST:$REMOTE/src/"
GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo nogit)
ssh "$HOST" "set -e; cd '$REMOTE'
  '$CMAKE' -S src -B release -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=OFF -DLTXAG_GIT=$GIT $CMAKE_ARGS >/dev/null
  '$CMAKE' --build release -j 8 2>&1 | grep -E 'error|warning' || true
  (cd release && '$CTEST' --output-on-failure >/dev/null) || { echo 'ctest failed' >&2; exit 1; }
  rm -rf dist/$NAME && mkdir -p dist/$NAME
  cp -R release/LTXAlphaGen.ofx.bundle dist/$NAME/
  install -m 0755 src/packaging/install.sh dist/$NAME/install.sh
  cp src/README.md src/LICENSE dist/$NAME/
  find dist/$NAME -name __pycache__ -prune -exec rm -rf {} +
  COPYFILE_DISABLE=1 tar -C dist -czf dist/$NAME-macos-arm64.tar.gz $NAME
  echo binary: \$(file dist/$NAME/LTXAlphaGen.ofx.bundle/Contents/MacOS/LTXAlphaGen.ofx | cut -d: -f2)"
mkdir -p "$SRC/dist"
scp -q "$HOST:$REMOTE/dist/$NAME-macos-arm64.tar.gz" "$SRC/dist/"
ls -la "$SRC/dist/$NAME-macos-arm64.tar.gz"
