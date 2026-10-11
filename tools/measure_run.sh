#!/usr/bin/env bash
# Build step 3: one measurement run of the official Alpha Gen recipe (model card) on akflame5.
#   measure_run.sh WIDTH HEIGHT FRAMES PRECISION [extra ic_lora args...]
# PRECISION: bf16 | fp8 (fp8 = --quantization fp8-cast on the bf16 file).
# Renders at native WIDTHxHEIGHT (the CLI takes 2x with --skip-stage-2). Samples the process's
# real GPU memory (nvidia-smi per-process, includes the CUDA context) every 0.25 s.
# Appends one CSV line to $ROOT/logs/measurements.csv; full output in $ROOT/logs/run-*.log.
set -uo pipefail
ROOT=/Volumes/Data2/ltx-alphagen
M=$ROOT/models
W=$1 H=$2 N=$3 PREC=$4
shift 4
TAG="${W}x${H}x${N}-${PREC}${*:+-extra}"
CLIP=$ROOT/tmp/clip-${W}x${H}x${N}.mp4
LOG=$ROOT/logs/run-$TAG-$(date +%H%M%S).log
cd $ROOT/ltx2
PY=$ROOT/ltx2/.venv/bin/python
[ -f "$CLIP" ] || $PY "$(dirname "$0")/make_test_clip.py" "$CLIP" "$W" "$H" "$N" >/dev/null
QUANT=()
[ "$PREC" = fp8 ] && QUANT=(--quantization fp8-cast)
START=$(date +%s.%N)
PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True $PY -m ltx_pipelines.ic_lora \
  --transformer-path $M/diffusion_models/ltx-2.5-22b-distilled-transformer-bf16.safetensors \
  --text-encoder-path $M/text_encoders/gemma4-12b-with-proj-ltx-2.5-bf16.safetensors \
  --video-vae-path $M/vae/ltx-2.5-video-vae-bf16.safetensors \
  --audio-vae-path $M/vae/ltx-2.5-audio-vae-bf16.safetensors \
  --spatial-upsampler-path $M/latent_upscale_models/ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0.safetensors \
  --lora $M/loras/ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors 1.0 \
  --video-conditioning "$CLIP" 1.0 \
  --prompt "" --skip-stage-2 --width $((W * 2)) --height $((H * 2)) \
  --num-frames "$N" --frame-rate 24 --seed 1234 \
  --output-path "$ROOT/tmp/out-$TAG.mp4" "${QUANT[@]}" "$@" >"$LOG" 2>&1 &
PID=$!
PEAK=0
while kill -0 $PID 2>/dev/null; do
  for p in $(pgrep -P $PID) $PID; do
    MEM=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits |
          awk -F', ' -v p="$p" '$1==p {print $2}')
    [ -n "$MEM" ] && [ "$MEM" -gt "$PEAK" ] && PEAK=$MEM
  done
  sleep 0.25
done
wait $PID
RC=$?
END=$(date +%s.%N)
SECS=$(echo "$END - $START" | bc)
OOM=$(grep -c -i "out of memory" "$LOG")
LINE="$(date +%F_%T),$W,$H,$N,$PREC,${*:-},$RC,$OOM,$PEAK,$SECS"
echo "$LINE" >> $ROOT/logs/measurements.csv
echo "$LINE  (log: $LOG)"
