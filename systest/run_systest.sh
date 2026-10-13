#!/usr/bin/env bash
# Runs one tier of the system test on this machine (systest/README.md).
#   systest/run_systest.sh --tier fast|full --bundle <LTXAlphaGen.ofx.bundle> --host <ltxag_systest>
#                          --work <dir> [--job-python <python with numpy>] [--only a,b] [--job-timeout s]
# <dir> must be on a local disk (the resident worker's Unix socket lives under it). It gets:
#   home/       the isolated HOME (config, cache, logs, worker socket): recreated on every run
#   ui-<tier>/  the fake windows' logs;  out-<tier>/  junit-<tier>.xml, console.log, logs.tar.gz
# The real user config (~/.config/ltx-alphagen/config.conf) supplies ltx_python and model_dir,
# and its cache root the prompt embedding; nothing of the real setup is written to.
set -uo pipefail
TIER= BUNDLE= HOSTBIN= WORK= JOB_PYTHON= ONLY= TIMEOUT=
while [ $# -gt 1 ]; do
  case $1 in
    --tier) TIER=$2 ;; --bundle) BUNDLE=$2 ;; --host) HOSTBIN=$2 ;; --work) WORK=$2 ;;
    --job-python) JOB_PYTHON=$2 ;; --only) ONLY=$2 ;; --job-timeout) TIMEOUT=$2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift 2
done
[ -n "$TIER" ] && [ -d "$BUNDLE" ] && [ -x "$HOSTBIN" ] && [ -n "$WORK" ] || {
  sed -n '2,9p' "$0" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
REAL_CONFIG="$HOME/.config/ltx-alphagen/config.conf"
conf() { sed -n "s/^$1=//p" "$REAL_CONFIG" 2>/dev/null | tail -1; }
LTX_PYTHON=$(conf ltx_python)
MODEL_DIR=$(conf model_dir); MODEL_DIR=${MODEL_DIR%/}
REAL_CACHE=$(conf cache_dir); REAL_CACHE=${REAL_CACHE:-$HOME/.cache/ltx-alphagen/mattes}
if [ "$TIER" = full ] && { [ ! -x "$LTX_PYTHON" ] || [ ! -d "$MODEL_DIR" ] || [ ! -f "$REAL_CACHE/prompt-empty.safetensors" ]; }; then
  echo "full tier: needs a completed Setup for this user (ltx_python, model_dir and the prompt cache in $REAL_CONFIG)" >&2
  exit 2
fi
if [ -z "$JOB_PYTHON" ]; then
  if [ "$(uname)" = Darwin ]; then JOB_PYTHON=$LTX_PYTHON; else JOB_PYTHON=/usr/bin/python3; fi
fi
# The fast tier only needs some existing python for ltx_python (the gate checks that it exists);
# on macOS job.py runs under it, so it needs numpy there.
[ -x "$LTX_PYTHON" ] || LTX_PYTHON=$JOB_PYTHON

mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
TESTHOME="$WORK/home" UI="$WORK/ui-$TIER" OUT="$WORK/out-$TIER"
rm -rf "$TESTHOME" "$UI" "$OUT"
mkdir -p "$TESTHOME/.cache/ltx-alphagen/mattes" "$UI" "$OUT"
[ -f "$REAL_CACHE/prompt-empty.safetensors" ] &&
  ln -s "$REAL_CACHE/prompt-empty.safetensors" "$TESTHOME/.cache/ltx-alphagen/mattes/prompt-empty.safetensors"

# Fake progress/message windows (systest/fake_ui.py).
if [ "$(uname)" = Darwin ]; then
  SWIFT="$BUNDLE/Contents/Resources/worker/ltxag_worker/mac_progress.swift"
  HASH=$(shasum -a 256 "$SWIFT" | cut -c1-12)
  mkdir -p "$TESTHOME/.cache/ltx-alphagen/bin"
  install -m 0755 "$HERE/fake_ui.py" "$TESTHOME/.cache/ltx-alphagen/bin/ltxag_progress-$HASH"
else
  mkdir -p "$WORK/fakebin"
  install -m 0755 "$HERE/fake_ui.py" "$WORK/fakebin/zenity"
  export PATH="$WORK/fakebin:$PATH"
fi

ARGS=(--bundle "$BUNDLE" --tier "$TIER" --out "$OUT" --ltx-python "$LTX_PYTHON" --job-python "$JOB_PYTHON")
[ -n "$MODEL_DIR" ] && ARGS+=(--model-dir "$MODEL_DIR")
[ -n "$ONLY" ] && ARGS+=(--only "$ONLY")
[ -n "$TIMEOUT" ] && ARGS+=(--job-timeout "$TIMEOUT")
echo "run_systest: tier $TIER, bundle $BUNDLE, work $WORK" | tee "$OUT/console.log"
HOME="$TESTHOME" LTXAG_FAKE_UI_DIR="$UI" "$HOSTBIN" "${ARGS[@]}" 2>&1 | tee -a "$OUT/console.log"
RC=${PIPESTATUS[0]}
# Whatever the plugin, job.py and the worker logged, for the report.
tar -czf "$OUT/logs.tar.gz" -C "$TESTHOME/.cache/ltx-alphagen" logs mattes/jobs -C "$WORK" "ui-$TIER" 2>/dev/null
echo "run_systest: exit $RC" | tee -a "$OUT/console.log"
exit "$RC"
