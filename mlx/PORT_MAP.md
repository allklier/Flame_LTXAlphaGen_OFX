# LTX-2.5 Alpha Gen on MLX: port map (B1)

**The MLX port is dropped** (user, 2026-10-14; decision 65): on an M5 Ultra PyTorch-MPS matches
MLX in fp16/bf16 (`worker/CLAUDE.md`). Kept as a reference for how the model works.

What our worker actually runs, traced through the LTX-2 source at the pinned commit `9ec55f9`,
and what a from-scratch MLX port has to rebuild. Written 2026-10-12 from reading the code and
the safetensors headers; no code has been ported yet.

Citations: `core/` = `packages/ltx-core/src/ltx_core`, `pipe/` = `packages/ltx-pipelines/src/ltx_pipelines`
(on akflame5 under `/Volumes/Data2/ltx-alphagen/ltx2/`). **V** = verified in code or header,
**I** = inferred, **UNVERIFIED** = needs checking (mostly MLX facts: no MLX installed yet).

## 1. Scope: our configuration

`ICLoraPipeline.stream_chunks(prompt="", seed=1234, height=2H, width=2W, num_frames=8n+1,
frame_rate=24, images=[], video_conditioning=[(ref, 1.0)], skip_stage_2=True)` (`worker/ltx_worker.py`).
That fixes almost every option, so the port only needs one path:

- **one stage** (stage 1 at native H×W), IC-LoRA on, 8 distilled steps, **no CFG** (one forward per step);
- **one chunk** per call (our plugin does the chunking), no keyframes, no transformer tiling;
- **empty prompt**: the text side is one cached tensor set (section 7), no Gemma;
- **reference video at native size** (LoRA metadata `reference_downscale_factor=1`, no temporal factor);
- video output only (audio is denoised but never decoded, section 3).

Everything else in the 73k-line repo (stage 2, the upsampler, keyframes, tiling, multi-GPU,
trainer, fp8/nvfp4, compile, ComfyUI key maps beyond ours) is out of scope.

## 2. The pipeline with shapes (HD 1920×1088, 145 frames)

| Step | What | Shape | Code (V) |
|---|---|---|---|
| 1 | Reference frames `x/127.5 − 1` (bf16) | (1,3,145,1088,1920) | `pipe/utils/media_io/decode.py:84-109`, `range_map.py:9` |
| 2 | **VAE encode** → normalised latent | (1,128,19,34,60) | `core/model/video_vae/video_vae.py:150-338` |
| 3 | Target latent = zeros; noise | same | `pipe/utils/helpers.py:71-106`, `core/components/noisers.py:22-37` |
| 4 | Patchify (patch 1): target + reference tokens appended | 38,760 + 38,760 = **77,520 tokens × 128** | `core/components/patchifiers.py:27-39`, `core/conditioning/types/reference_video_cond.py:50-116` |
| 5 | Audio latent zeros → noise (joint denoise) | (1,8,151,16) → 151 tokens × 128 | `pipe/chunks/planner.py:111-121`, `operators.py:91-106` |
| 6 | **8 × transformer** (velocity) → x0 → Euler | | `pipe/utils/samplers.py:26-80`, `core/model/transformer/model.py:586-600` |
| 7 | Drop reference tokens, unpatchify | (1,128,19,34,60) | `core/tools.py:88-117` |
| 8 | **VAE decode** (1-step diffusion decoder) | (1,3,145,1088,1920) in [0,1] | `core/model/video_vae/diffusion_video_decoder.py:72-746, 1258-1584` |
| 9 | Our worker: mean of RGB = matte | (145,1088,1920) | `worker/ltx_worker.py` |

Latent compression: time 8 (causal: `1 + (F−1)/8`), space 32×32, 128 channels.

## 3. Denoising loop and conditioning (~250–350 lines in MLX)

- **Sigmas** (V, `pipe/utils/constants.py:17`): `[1.0, 0.99375, 0.9875, 0.98125, 0.975, 0.909375, 0.725, 0.421875, 0.0]` → 8 transformer calls.
- **Noise** (V): one `randn` in bf16 over the patchified video state (target **and** reference
  tokens), then audio; target tokens become pure noise, reference tokens are reset to their
  clean latent (`denoise_mask = 0`).
- **Per step** (V, `pipe/utils/denoisers.py:214-252`, `helpers.py:600-625`):
  `timesteps = σ · denoise_mask` **per token** (target σ, reference 0); one forward, no
  guidance, no text mask (`context_mask=None`); the model predicts velocity,
  `x0 = x − v·t` in fp32 (`core/utils.py:39-52`); clean reference put back
  (`x0·mask + clean·(1−mask)`); Euler `x' = x + v·(σ_next − σ)` with
  `v = (x − x0)/σ`, fp32 math, bf16 storage. The last step lands on x0.
- **Positions** (V, `core/tools.py:139-196`, `patchifiers.py:64-171`): per token
  [start,end) in **pixels** (latent index × 8/32/32), time causally shifted
  (`clamp(8f+1−8, 0)`) and divided by fps (seconds). Reference tokens get **the same
  positions** as the target. The first latent frame of the target is marked as a "keyframe"
  (a learned embedding is added to those tokens).
- **Audio runs** (V, `core/model/transformer/transformer.py:199-203`): a zero audio latent is
  noised and denoised jointly; audio↔video cross-attention feeds the video. Cheap (151 tokens)
  but must be ported unless an A/B on CUDA shows the matte is unchanged without it (section 9).

## 4. Transformer (the big one; ~1,500 lines upstream, I'd expect ~600–900 in MLX)

**Config** comes from the checkpoint metadata (V, `core/loader/helpers.py:44-58`): class
`AVTransformer3DModel`, 48 layers; video 32 heads × 128 = 4096; audio 32 × 64 = 2048;
text context 4096 (video) / 2048 (audio); `qk_norm rms_norm`, gated attention, `ff_bias false`,
RoPE `split`, theta 1e4, max_pos [20, 2048, 2048], float64 frequencies.

**Weights** (V, header): 42.0 GB bf16 total, of which
| Part | GB | Needed |
|---|---|---|
| video blocks (48 × attn1, attn2, ff, tables) | 25.8 | yes |
| audio + audio↔video cross | 12.5 | yes, unless the A/B says no |
| top level (adaln, proj in/out, keyframe emb.) | 0.4 | yes |
| text embeddings connectors | 3.2 | **no**: part of the prompt encoder; its output is in our cache (V, `core/text_encoders/gemma/embeddings_processor.py:68-100`) |

**Block, per step** (V, `transformer.py:189-354`, `attention.py:470-578`), with AdaLN values =
table + per-token timestep embedding:
1. video self-attention: `rms_norm(x)·(1+scale)+shift` → q/k/v (bias) → RMSNorm on q,k
   (with weight, over all heads) → RoPE → SDPA (unmasked, 77.5k tokens) → per-head gate
   `2·sigmoid(Linear(x→32))` → out → `x += out·gate`;
2. text cross-attention: query modulated from table rows 6:9, context modulated by the prompt
   AdaLN; no RoPE, no mask;
3. the same two at width 2048 for audio;
4. audio→video and video→audio cross-attention (time-only RoPE, its own AdaLN tables);
5. FFN: 4096→16384 gelu-tanh →4096 (no bias); audio 2048→8192→2048 (bias).
Then LayerNorm (no affine) → modulation → `proj_out` 4096→128.

**Timestep embedding** (V, `timestep_embedding.py`, `adaln.py`): sinusoid(t·1000, 256) →
Linear-SiLU-Linear → SiLU → Linear(4096 → 9·4096), per token. Only two distinct t values per
step (σ and 0), so an MLX port can compute two rows and gather instead of 77.5k (I).

**RoPE** (V, `rope.py:85-230`): midpoint of each position interval / max_pos → `2·frac−1`;
682 frequencies `theta^linspace(0,1,682)·π/2` (numpy float64); interleaved (t,h,w) → 2046,
front-padded to 2048 with (cos 1, sin 0); rotate-half within each 128-dim head. Must be written
by hand: `mx.fast.rope` takes 1-D positions (I, **UNVERIFIED**).

**LoRA** (V, `core/loader/fuse_loras.py:60-69`): fused at load, `W += B@A` (strength 1, no
alpha/rank scaling), rank 128, only the **video** attn1/attn2 q,k,v,out and ff in all 48 blocks
(960 tensors, 1.31 GB). Fusing once at conversion time is simplest.

**fp8-cast**: a CUDA memory trick (weights stored fp8, upcast in `forward`); not ported. On the
Mac: bf16 first, MLX's own quantisation later if memory requires it.

## 5. VAE encoder (reference video; ~300 lines in MLX, conv-only)

V, `video_vae.py:150-338`, config `CausalDiffusionVAE` from the header; 319 M parameters.
- patchify 4 (`b c f (h q) (w r) → b (c r q) f h w`: mind the channel order) → 48 channels;
- CausalConv3d 3×3×3 (first frame repeated twice in front, zero spatial padding) 48→128;
- res blocks (PixelNorm = RMS over channels eps 1e-8 → SiLU → conv, ×2, + skip) and four
  space/time-to-depth downsamplers (conv branch + averaged skip; frame 0 duplicated before a
  temporal stride) → 128/256/512/1024 channels;
- PixelNorm → SiLU → conv_out 1024→129, keep 128 means, `(x − mean)/std` per channel
  (`per_channel_statistics`).
- No attention. Untiled in our configuration.

## 6. VAE decoder (a 1-step diffusion decoder; ~600–900 lines in MLX)

V, `diffusion_video_decoder.py`, class `NADiffusionDecoder`, 417 M parameters. **No convolutions.**
- un-normalise → Linear 128→2048 → **stages 1–4: transformer blocks with 3-D neighbourhood
  attention** (kernels 3×7×7, 3×7×7, 3×5×5, 3×5×5; RMSNorm, q/k RMSNorm, absolute 3-axis
  RoPE, SwiGLU) each followed by Linear + pixel-shuffle upsampling;
- **stage 5 = the diffusion head**: pixel-space noise `x_t` (from the shared generator),
  Linear 48→256, 8 blocks of neighbourhood attention with an **11×11×11** window at 256 channels
  (4 heads), AdaLN from a fixed t = 1.0 (precomputable), context added from stage 4;
  **one step**, x0 prediction → `(x+1)/2` clamped.
- **Neighbourhood attention** is the one op with no MLX equivalent (I, **UNVERIFIED**). The
  reference to port is upstream's pure-PyTorch fallback `transformer/fallback_na/eager.py`
  (166 lines: query tiles + additive window masks + batched SDPA, windows shifted inward at the
  borders, matching NATTEN). Later: a custom Metal kernel if it is the bottleneck.
- **Tiling**: upstream's planner (`diffusion_tiling.py:261-418`) splits stages 4–5 into tiles
  from free memory (temporal overlap 40 frames, spatial 160 px, trapezoid blending). With 128 GB
  unified memory an HD 145-frame decode may fit in one tile (I); a simple fixed tiling can come later.
- "CHUNKED_EAGER" (our default) slices stage 5 into 4 slabs along W with edge-replicated halos
  (`chunked/attn.py:356-441`), a stated approximation: the MLX port should match the plain
  ("combined") result and accept tiny differences from Linux.

## 7. Prompt embedding

Our cache (V, `prompt-empty.safetensors` on akflame5): `video_encoding` (1,1024,4096) bf16,
`audio_encoding` (1,1024,2048) bf16, `attention_mask` (1,1024) i64 (not used by the DiT).
It is the connector output, so the Mac needs **no Gemma and no connector** while the prompt is
empty. **OPEN:** how a Mac user gets this file: (a) port Gemma 4 12B + the connectors (a large
second project), (b) build it once on a CUDA/CPU machine and copy it (fine for the spike), (c)
compute it in PyTorch on CPU/MPS during Setup with the upstream code (I: likely the practical
answer; needs Gemma's 26 GB download anyway).

## 8. Size and speed estimate

| Component | MLX lines (I) | Weights | Notes |
|---|---|---|---|
| Weight loading + LoRA fuse + key mapping | 150 | — | safetensors → MLX arrays |
| Denoise loop, positions, conditioning | 300 | — | |
| Transformer (video + audio + AV cross) | 700 | 38.7 GB (26.2 video only) | RoPE by hand |
| VAE encoder | 300 | 0.6 GB | needs conv3d |
| VAE decoder incl. neighbourhood attention | 800 | 0.8 GB | NA is the hard part |
| Validation harness (CUDA dumps vs MLX) | 300 | — | |
| **Total** | **~2,500** | **~40 GB** | |

**Speed (arithmetic, I):** at HD 145 one denoising step is ~6.5 PFLOP (≈4.7 attention over
77.5k tokens + 1.8 linear), ×8 ≈ 52 PFLOP per chunk. The Blackwell does the 8 steps in ~230 s
(≈ 225 TFLOPS effective). An M1 Ultra GPU peaks around 21 TFLOPS (**UNVERIFIED**); at a
realistic ~50 % that is **~1.5 h per full HD 145-frame chunk**, plus encode and decode
(each ~0.5 PFLOP, a few minutes). Attention is quadratic in tokens, so smaller work is much
cheaper: 720p × 49 frames is ~13k tokens, ~3.4 PFLOP for 8 steps, roughly **1/15 of the
work → ~6 min** at the same rate. Whether M1
computes bf16 natively is **UNVERIFIED** (if not, bf16 runs emulated or we use fp16, which risks
overflow in a model trained in bf16).

## 9. Risks and open questions

1. **Speed** (above): decides what the Mac version is good for (short/low-res clips, or
   overnight runs). Measure early.
2. **MLX op coverage** (**UNVERIFIED** until MLX is installed): `conv3d` (channels-last),
   `mx.fast.scaled_dot_product_attention` with an additive mask at 77k tokens (memory: must not
   materialise the 77k × 77k score matrix), bf16 on M1.
3. **Audio branch**: keep it (exact parity) or drop it (−12.5 GB, simpler). A/B on CUDA first:
   run the official pipeline with the audio latent removed and compare mattes.
4. **RNG**: CUDA's generator cannot be reproduced in MLX. Validation therefore feeds
   **identical noise dumped from CUDA** into both implementations; production uses MLX's own
   RNG (mattes then differ by sampling noise only, like a different seed).
5. **Neighbourhood attention** cost and memory in MLX (11×11×11 windows over ~19M pixel tokens
   at HD 145 in stage 5).
6. **Prompt embedding** for Mac users (section 7).

**Worth knowing before porting:** upstream LTX-2 already supports Apple Silicon through
**PyTorch on MPS** (V, `core/model/transformer/attention.py:39-42, 99-141`: an `MPSSdpaAttention`
backed by Apple's `MPSGraph.scaledDotProductAttention` via the `mps-sdpa` package;
`core/loader/fuse_loras.py:80-95` has an MPS LoRA-fuse path; `devices.py:113-131` an MPS memory
budget). The VAE decoder would fall back to the eager neighbourhood attention there. Running
upstream on MPS on the M1 Ultra would give a speed number and a same-machine numerical reference
without porting anything.

## 10. Suggested order (B2 onwards)

0. *(optional, recommended)* Upstream on PyTorch-MPS on the Mac, one small chunk: speed and a
   reference matte. Also the CUDA audio A/B (risk 3).
1. MLX installed on the Mac; verify the ops in risk 2.
2. Weight conversion: safetensors → MLX, LoRA fused, video (+ audio) parts only.
3. VAE encoder → compare latents with CUDA dumps.
4. One transformer block → full transformer (one step with dumped inputs) → the 8-step loop.
5. VAE decoder: deterministic stages 1–4, then neighbourhood attention, then stage 5.
6. End to end on a small clip (720p × 49) vs the Linux matte; then time and memory at HD.
