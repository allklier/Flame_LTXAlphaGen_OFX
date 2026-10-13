#!/usr/bin/env bash
# Builds the dev and release bundles plus the test host on this machine and runs the system-test
# tiers (systest/README.md): fast on the dev build, full on the release build.
#   systest/run_all.sh --build-root <dir> --work <local dir> [--tiers "fast full"] [--cmake <cmake>]
#                      [--git <hash>] [--only a,b] [-- <extra cmake configure args>]
# Writes <work>/DONE when finished: one line "<tier> <exit code>" per tier. Exit 0 = all passed.
set -uo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
ROOT= WORK= TIERS="fast full" CMAKE=cmake GIT= ONLY=
while [ $# -gt 0 ]; do
  case $1 in
    --build-root) ROOT=$2; shift 2 ;; --work) WORK=$2; shift 2 ;; --tiers) TIERS=$2; shift 2 ;;
    --cmake) CMAKE=$2; shift 2 ;; --git) GIT=$2; shift 2 ;; --only) ONLY=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$ROOT" ] && [ -n "$WORK" ] || { sed -n '2,6p' "$0" >&2; exit 2; }
mkdir -p "$ROOT" "$WORK"
rm -f "$WORK/DONE"
GITARG=()
[ -n "$GIT" ] && GITARG=(-DLTXAG_GIT="$GIT")
RESULTS=""
RC=0
for TIER in $TIERS; do
  case $TIER in fast) VARIANT=dev DEV=ON ;; full) VARIANT=release DEV=OFF ;; *) echo "unknown tier $TIER" >&2; exit 2 ;; esac
  B="$ROOT/$VARIANT"
  echo "== $TIER: building the $VARIANT bundle in $B"
  if ! { "$CMAKE" -S "$SRC" -B "$B" -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=$DEV ${GITARG[@]+"${GITARG[@]}"} "$@" >/dev/null &&
         "$CMAKE" --build "$B" -j 8 --target ltxag_systest; } >"$WORK/build-$VARIANT.log" 2>&1; then
    tail -20 "$WORK/build-$VARIANT.log"
    echo "== $TIER: BUILD FAILED (see $WORK/build-$VARIANT.log)"
    RESULTS+="$TIER build-failed"$'\n'
    RC=1
    continue
  fi
  ARGS=(--tier "$TIER" --bundle "$B/LTXAlphaGen.ofx.bundle" --host "$B/ltxag_systest" --work "$WORK")
  [ -n "$ONLY" ] && ARGS+=(--only "$ONLY")
  "$SRC/systest/run_systest.sh" "${ARGS[@]}"
  T=$?
  RESULTS+="$TIER $T"$'\n'
  [ $T -eq 0 ] || RC=1
done
printf '%s' "$RESULTS" >"$WORK/DONE"
exit $RC
