#!/usr/bin/env bash
# Setup helper end-to-end on a Rocky 9 machine with the GPU (no Flame). Isolated HOME, so the user's real config and
# logs are untouched. Needs ~85 GB free under $T.
#   setup_smoke.sh <existing models dir> <test root>
# A: fresh install (runtime + 6 models copied from the existing folder, no token)
# B: re-run is idempotent and fast (verified markers)
# C: missing file + no token -> clear error
# D: fake token -> HTTP 401/403 -> "accept the model terms" message
# E: same-name, same-size but different file in "existing" -> rejected (not copied)
set -u
EXISTING=$1
T=$2
SRC="$(cd "$(dirname "$0")/.." && pwd)"
H="$SRC/helper/setup_helper.py"
export HOME=$T/home
mkdir -p "$HOME"
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }
LOG=$HOME/.cache/ltx-alphagen/logs/ltx-alphagen.log
CFG=$HOME/.config/ltx-alphagen/config.conf

t0=$(date +%s)
/usr/bin/python3 -I -B "$H" --model-dir "$T/models" --cache-root "$T/cache" --existing "$EXISTING" --no-ui
rc=$?
tA=$(($(date +%s) - t0))
[ $rc -eq 0 ] && ok "A fresh install rc=0 (${tA}s)" || bad "A fresh install rc=$rc"
grep -q "^models_ready=" "$CFG" && grep -q "^ltx_python=$T/models/runtime/ltx2/.venv/bin/python" "$CFG" \
  && ok "A config written" || bad "A config: $(cat "$CFG" 2>/dev/null)"
[ "$(grep -c 'copying with verification' "$LOG")" -eq 6 ] && ok "A all 6 files copied from existing" \
  || bad "A copies: $(grep -c 'copying with verification' "$LOG")"
[ -f "$T/cache/prompt-empty.safetensors" ] && ok "A prompt cache built" || bad "A no prompt cache"
"$T/models/runtime/ltx2/.venv/bin/python" -I -c "import PyOpenColorIO, torch; assert torch.cuda.is_available()" \
  && ok "A runtime imports torch(CUDA)+OCIO" || bad "A runtime import"

t0=$(date +%s)
/usr/bin/python3 -I -B "$H" --model-dir "$T/models" --cache-root "$T/cache" --existing "$EXISTING" --no-ui
rc=$?
tB=$(($(date +%s) - t0))
[ $rc -eq 0 ] && [ $tB -lt $((tA / 2)) ] && ok "B re-run idempotent and fast (${tB}s vs ${tA}s)" \
  || bad "B re-run rc=$rc ${tB}s vs ${tA}s"
grep "setup finished\|Setup finished" "$LOG" | tail -1 | grep -q "Kept 6" && ok "B kept all 6" \
  || bad "B summary: $(grep 'Setup finished' "$LOG" | tail -1)"

mv "$T/models/vae/ltx-2.5-audio-vae-bf16.safetensors" "$T/audio.bak"
/usr/bin/python3 -I -B "$H" --model-dir "$T/models" --cache-root "$T/cache" --no-ui
rc=$?
[ $rc -eq 2 ] && grep "setup failed" "$LOG" | tail -1 | grep -q "no Hugging Face token" \
  && ok "C missing file without token -> clear error" || bad "C rc=$rc $(grep 'setup failed' "$LOG" | tail -1)"

HF_TOKEN=hf_notarealtoken0000000000000000000 /usr/bin/python3 -I -B "$H" --model-dir "$T/models" \
  --cache-root "$T/cache" --no-ui
rc=$?
grep "setup failed" "$LOG" | tail -1 | grep -q "accept the model terms" \
  && ok "D fake token -> refused with the accept-terms hint (rc=$rc)" || bad "D $(grep 'setup failed' "$LOG" | tail -1)"
grep -q "notarealtoken" "$LOG" && bad "D token leaked into the log" || ok "D token not in the log"

mkdir -p "$T/fake/vae"
truncate -s 364866540 "$T/fake/vae/ltx-2.5-audio-vae-bf16.safetensors"
/usr/bin/python3 -I -B "$H" --model-dir "$T/models" --cache-root "$T/cache" --existing "$T/fake" --no-ui
rc=$?
grep -q "existing copy is a different version" "$LOG" && [ ! -f "$T/models/vae/ltx-2.5-audio-vae-bf16.safetensors" ] \
  && ok "E wrong-content file rejected, not copied (rc=$rc)" || bad "E rc=$rc"
mv "$T/audio.bak" "$T/models/vae/ltx-2.5-audio-vae-bf16.safetensors"

echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
