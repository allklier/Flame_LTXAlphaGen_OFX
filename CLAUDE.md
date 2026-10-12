# CLAUDE.md — LTX Alpha Gen OFX for Flame

Design knowledge for anyone changing this plugin, human or AI assistant: what was decided and
why, how Flame actually behaves as an OFX host, the measurements behind the memory model, and the
approaches that were tried and failed. Numbered decisions are referenced from code comments
("decision 34"); keep the numbers stable.

Machine-specific notes (build hosts, paths, accounts, status) belong in a git-ignored
`CLAUDE.local.md`, which Claude Code loads after this file. Never put them here.

Tags: **DECIDED** (maintainer approved), **PROPOSED** (recommended, not yet approved), **OPEN**
(needs a decision), **UNVERIFIED** (not yet confirmed against a real install or source).

## Goal

An OFX plugin for Autodesk Flame on **Rocky Linux 9** (NVIDIA only, no macOS) that runs
Lightricks' **LTX-2.5 Alpha Gen** IC-LoRA and produces an alpha matte from an RGB clip. It can be
shared publicly: **users download their own weights and are responsible for their own LTX
licence.** v0.1 (2026-10-11) is the first release; see README.md for the user-facing view.

## Decisions (numbers are referenced from code comments)

**Architecture and runtime**
- **1.** A light C++ OFX shim plus a Python background process on the same machine. **No ComfyUI.**
- **5.** Progress feedback, a Cancel option, graceful OOM; Flame must never be killed by the plugin.
- **6.** All GPU memory goes back to Flame when processing ends (Unload GPU on, the default).
- **11/19.** A per-node "Unload GPU" param; the pattern is earlier nodes resident, last node
  unloads. Built as a resident worker server (see "Resident worker").
- **12/32.** Per-node VRAM reserve for Flame in GB. Built-in default = 5% of the VRAM *free* when
  the plugin loads.
- **15/16.** Recipe 1 only: the model-card recipe, distilled transformer, IC-LoRA, stage 1 at
  native resolution, empty prompt.
- **17.** Only the bf16 transformer is downloaded. Runtime precision is per node: **bf16 or
  fp8-cast**. nvfp4 and int8 are dropped (see "Dead ends").
- **22.** Flame's shipped NVIDIA driver runs the stack (verified: driver 580.159.04, torch 2.13 +
  CUDA 13.2).
- **33.** Idle timeout of 5 min for a resident worker (`$LTXAG_IDLE_TIMEOUT` overrides for
  tests). The server also exits when the Flame process that started the job dies.
- **34.** No silent downgrade. (a) The built-in default precision is bf16 only if a full HD
  145-frame chunk fits in free VRAM minus the reserve, else fp8. (c) Each run warns if the node's
  precision may not fit (decision 56). Both use the size-aware estimate (`ofx/src/vram_estimate.*`).
- **35.** Processing never starts automatically. Render serves only from the cache; **Process**
  stages the frames and launches a detached job with its own progress window and Cancel.
- **36.** Disk matte cache (content-addressed per frame) with purge: automatic age + LRU sweep,
  Purge Cache (node), Purge All.
- **38.** Licence + model gate. Release builds enforce it; dev builds bypass it unless "Enforce
  Gate" is on.
- **39.** Diagnostic logging is a product requirement (see "Logging").
- **55.** **Resident transformer only when Unload GPU is off.** Keeping the built transformer
  saves ~6 s per chunk, but it stays on the GPU through the next chunk's reference encode and the
  video decode, which costs a full transformer of extra memory (see the resident refit under
  "Measurements"). So with Unload on (the default) the worker rebuilds per chunk as upstream does.
  Audio decoding is always skipped (video-only output).
- **56.** **VRAM: warn instead of refusing, record every run, learn locally later.** The
  built-in estimate comes from measurements on one card (96 GB), so unseen GPUs may be over- or
  under-estimated.
  - **Warning with Yes/No (built):** the preflight asks `sendMessage(eMessageQuestion)`; Yes
    processes, No cancels; if the host doesn't ask (reply neither Yes nor No) it proceeds with a
    footer warning. The message lists the estimate, free + LTX worker − reserve, and what Flame
    and other apps hold (NVML per process, `nvml.cpp`). A live resident worker's memory always
    counts as available whatever its precision, because a mismatch restarts it (counting only a
    matching precision caused false refusals).
  - **Step 1, record every run (built):** the manifest carries the preflight numbers (estimate,
    free, total, resident, reserve, model size, GPU name, driver, warned); the worker samples the
    lowest free memory (`cudaMemGetInfo` every 0.25 s) and reports `vram <lowest free> <torch peak
    allocated> <peak reserved> <held at start> <torch version>`; job.py logs one line and appends a
    row to `logs/vram_stats.csv` (in Collect Logs; no telemetry; same review-first rule as 46).
    used = preflight free + resident − lowest free. Runs without preflight numbers (smoke tools)
    are logged but not recorded.
  - **Step 2, local correction (PROPOSED, after real data shows the spread):** one multiplicative
    factor per precision = median(observed / predicted) over successful runs; inactive below ~10
    runs; clamped to ~0.8–1.3; falls slowly, rises fast after an OOM or near-OOM; stored in the
    user config, logged, with a "Reset estimate" action.
  - **Caveat:** a successful run only gives an upper bound on the need and an OOM only a lower
    bound; compare allocated / lowest-free numbers (not nvidia-smi) and treat both outcomes.

**Setup, models, licence**
- **2/13/20.** Setup handles the downloads. The HF token is used once (passed in the
  environment, never on argv) and never saved. Models are downloaded once per user, to a
  user-chosen folder.
- **7.** `ltx-core`/`ltx-pipelines` are installed by the user's Setup, never bundled.
- **9.** The Gemma prompt embedding is cached locally, so Gemma isn't loaded per job. The *final*
  embedding is cached (see "Worker").
- **18/21.** Settings are per user (`~/.config/ltx-alphagen/config.conf`). Three-level defaults
  for precision, unload and reserve: built-in → user default ("Save Default") → per node.
- **51.** **Reuse existing downloads:** an optional "Existing Models" folder is searched by exact
  file name. Files are verified by **size + SHA-256** against a **pinned manifest shipped with the
  plugin**, then **copied** into the Model Folder; only missing files are downloaded. HF hides
  SHA-256 for these gated repos without auth, so the hashes are captured once per release.
- **53.** **Project licence: Mozilla Public License 2.0.** `LICENSE` is the canonical text from
  `mozilla.org/media/MPL/2.0/index.txt`. MPL is file-level copyleft: our files stay MPL; the
  vendored OpenFX files keep their BSD licence (in their headers); the LTX code and weights are
  never in the repo (LTX-2.x Community License, installed by the user).
- **60.** **The runtime uses uv's own Python, pinned (3.14.7), never a system Python.** LTX-2
  only asks for ≥ 3.10, so uv used whatever qualified: on Rocky's 3.9 it downloaded 3.14.7, but a
  user's system Python 3.12 got picked up, and without `python3.12-devel` Triton's C build failed
  at the prompt cache step (`Python.h: No such file`). Setup now runs `uv sync --managed-python
  --python <manifest runtime.python>`: the same Python everywhere, with its headers; an existing
  venv on another Python is recreated. Triton also needs a **C compiler** ($CC, else gcc, else
  clang; `triton/runtime/build.py:26-39`): Setup checks for one before any download, and the
  runtime smoke test checks `Python.h`. (0.1.2, from a user report.)
- **57.** **No bundled Python packages.** Licences would allow bundling torch etc. (only the LTX
  code must not be bundled; NVIDIA's CUDA wheels have their own redistribution terms). But the
  venv is 6.4 GB (NVIDIA CUDA libs 3.1 GB, torch 1.1, triton 0.7; the LTX packages a few MB):
  bundling only moves those bytes from PyPI into our release, which then exceeds GitHub's 2 GB
  per-file limit and ties the plugin to one torch/CUDA/Python. Packages are ~7 GB next to 73 GB
  of models. Later option: an offline install from a package folder made on a connected machine.

**Images, colour, chunks**
- **8/23/27/28/54.** Input colour: a static dropdown `Bypass`, `ACEScg`, `LogC4`, `Log3G10`,
  `LogC3`, `BMD Film Gen5` (OCIO names in `worker/ltxag_worker/colour.py`; Flame's config has
  LogC3 only at EI800). **New entries go at the end: setups store the index.** The view is fixed
  to `ACES 2.0 - SDR 100 nits (Rec.709)` on display `Rec.1886 Rec.709 - Display`, as a
  `DisplayViewTransform` (a plain `getProcessor(src, display)` skips the tone mapping and gives
  out-of-range values). **Decision 54 (supersedes 14):** the conversion runs in the **Python
  worker** with PyOpenColorIO 2.6.0, not in the C++ shim: the fixed dropdown removed the reason for
  C++ OCIO, and this avoids building OCIO and any clash with Flame's own OCIO. Frames travel as
  float16 in the source colour space. `applyRGB` releases the GIL, so the worker converts one
  frame per thread (16): 820 → 83 ms per 2K frame, identical output.
- **24.** Padding to /32 (pixels) and 8n+1 (frames) is automatic and invisible; the output always
  matches the source size and frame count.
- **26.** No resolution cap and no scaling inside the OFX. VRAM is the only limit.
- **30.** Chunks of 145 frames, overlap ≥ 24, linear crossfade at the middle of the overlap.
- **48.** Flame forces an RGBA output; alpha (Flame's matte output) carries the matte or marker.
- **59.** **SETUP REQUIRED shows on both outputs.** A node viewed on its image output (often
  Flame's default) must not look like a working pass-through before Setup is complete, so the
  image output shows the marker too (grey on black). In every other state the image is the
  source with the overlay (58), plain where there is no matte. Added in 0.1.1, from the v0.1
  acceptance test.
- **58.** **The image output is a matte overlay:** source × (1 − 0.3·m) + magenta (1, 0, 1) ×
  0.3·m per RGB channel, in the source's own colour space (`overlayPixel`, `marker.h`, tested in
  `marker_log`; 50% was too opaque). Frames without a matte show the plain source (except SETUP
  REQUIRED, decision 59). Render-only: the cache key is unchanged.

**UI**
- **37/52.** Layout follows Flame's observed rules (see "Flame host behaviour"). Tab 1 holds
  Alpha Gen, Performance and Cache plus three blank spacer columns, so Licence, Models and
  Support overflow to tab 2, which Flame names "Setup" after the 2nd OFX page.
- **41.** Plain string fields get a caption label param above them. After a download the token
  caption reads "Token used HH:MM, not stored".
- **44 (from 40).** Frames without a matte show a faint "no media"-style marker in the matte
  output: stripes at 0.08 and a caption at 0.30 (NOT PROCESSED / PROCESSING / FAILED / CANCELLED /
  SETUP REQUIRED / NOT CONNECTED / NOT SUPPORTED). "Show Marker" off gives plain black.
- **46.** Paths are logged as-is; Collect Logs asks users to review the bundle before sending it.
- **49.** **Auto-refresh when a job finishes, via a Flame Python hook** (verified in Flame).
  `flame_hook/ltx_alphagen_hook.py` (in the bundle; `install.sh` copies it to
  `/opt/Autodesk/shared/python`). job.py writes `~/.cache/ltx-alphagen/run/refresh/<job>.json`
  (only for jobs whose `flame_pid` is a live process named `flame`); the hook polls that folder
  every second through `flame.schedule_idle_event` and, on Flame's UI thread, steps
  `flame.batch.current_frame` to a neighbour (back from the last frame, forward from frame 1) and
  back one second later. Flame re-renders on each frame change, and the return renders the
  original frame from the fresh cache. If scheduling the return fails, it steps back at once.
  Several jobs ending together give one refresh. History in "Dead ends".
  - The PROCESSING marker: the plugin writes the frames' pending markers itself (Flame's pid) just
    before launching; job.py rewrites them with its pid. Otherwise Flame's render right after the
    button press can beat job.py's markers (~50 ms) and show NOT PROCESSED for the whole job. The
    plugin's watcher clears them if no job status appears within 60 s.
- **50.** No more prototype rounds; the plugin is developed and tested as the real thing.

## Architecture (as built)

```
Flame ──OFX──> LTXAlphaGen.ofx (C++ shim)
                 render():  hash source frame t → disk cache lookup → matte | marker (+ overlay)
                 Process:   preflight (memory) → fetch all clip frames (main thread, ~11 ms/HD frame)
                            → /dev/shm/ltxag_job_<id>_in.bin + manifest .json, pending markers
                            → launchDetached(python3 -I -B job.py manifest)
job.py (system python3): zenity progress + Cancel, status file, refresh marker
   └─> resident worker server (LTX venv python -I -B ltx_worker.py --serve)  | dummy_worker.py
         reads chunks from shm, converts colour, pads, infers, crops → out_<i>.bin; stage/step/vram lines
   job.py blends overlaps → writes one .matte per frame into the cache → jobs/<id>.json
Flame Python hook: polls refresh markers → steps the current frame and back
```

**Source files**
- `ofx/src/plugin.cpp`: the effect, params, layout, gate, preflight, Process, render, user
  defaults, Collect Logs, the job watcher.
- `ofx/src/chunk_plan.*`: the chunk planner + crossfade weights.
- `ofx/src/cache.*`: cache keys, read with mtime refresh, pending markers, purges, the age+LRU
  sweep, and the stale `/dev/shm` sweep.
- `ofx/src/transport.*`: shm frame files, written with `write()`, never mmap (no SIGBUS inside
  Flame).
- `ofx/src/process.*`: `posix_spawn` with its own process group and a cleaned env (strips
  `PYTHON*`, `LD_LIBRARY_PATH`, `LD_PRELOAD`, `HF_TOKEN`); detached launch via `sh &`.
- `ofx/src/vram_estimate.*`: the memory model (decision 34/56).
- `ofx/src/log.*`, `env_info.*`, `marker.*` (marker + overlay), `nvml.*` (dlopen; device and
  per-process memory), `user_config.*`, `half.h` (F16C).
- `worker/job.py`: the detached job controller (resident/subprocess channels, progress Tracker,
  VRAM record, refresh marker). Manifest: `job_id node_id input width height frames chunks
  contrib frame_keys cache_root worker [worker_python worker_args worker_socket model_dir unload
  colour ocio_config seed] seconds simulate precision vram flame_pid title`.
- `worker/ltx_worker.py`: the real worker (see "Worker"). `worker/dummy_worker.py`: a luminance
  "matte" with `--simulate oom|crash|hang`, same line protocol.
- `worker/ltxag_worker/{padding,transport,cache,log,colour}.py`.
- `helper/setup_helper.py` (Setup), `helper/model_manifest.json`, `helper/ltx2-uv.lock`,
  `helper/licence.txt` (full LTX-2.x licence + header).
- `flame_hook/ltx_alphagen_hook.py`: the Flame Python hook (decision 49), copied into the
  bundle's `Resources/flame_hook/`.
- `packaging/install.sh` (in the release tarball); `scripts/build_remote.sh` (sync, build, ctest
  on a Rocky 9 machine) and `scripts/package_release.sh` (the tarball into `dist/`, git-ignored);
  both take `HOST=<ssh host>`.
- `tools/`: `make_test_clip.py`, `measure_run.sh` (official-CLI VRAM/time runs), `worker_smoke.py`,
  `job_real_smoke.py`, `resident_smoke.py`, `setup_smoke.sh`, `nvml_probe.cpp` (NVML vs
  nvidia-smi).
- `third_party/openfx`: OpenFX `OFX_Release_1_4_TAG`, `include/` + `Support/` only (BSD,
  `VENDORED.txt`).
- Plugin id `com.janklier.ltx.alphagen`, version from CMake `LTXAG_VERSION` (0.1.2; the OFX
  version is its major.minor), grouping "LTX". `LTXAG_DEV_BUILD=ON` (the CMake default, for local
  tests) adds a Prototype page: Sim Seconds, Simulate, Sim VRAM GB, Worker Python, Enforce Gate,
  Auto Refresh, Use Dummy. Release builds (`OFF`) hide those params but still define them, so
  saved setups load.

**Cache:** `<root>/<k[0:2]>/<key>.matte` (an LTXM header + fp16, ~4 MB per HD frame), where
key = hash(source frame hash, `v1|cs=…|prec=…|model=ltx2.5-alphagen0.9-recipe1` or `dummy-0.3`).
- Also in the root: `<root>/pending/<key>` (job pid), `<root>/jobs/<id>.json` (state running/done/
  failed/cancelled, chunks_done, frames_done) and `<root>/prompt-empty.safetensors`.
- The default root is `~/.cache/ltx-alphagen/mattes`. Sweep defaults: 30 days, 50 GB cap. It runs
  in `describe` (once per Flame start).
- Known approximation: an identical source frame in another clip context reuses the first
  context's matte.

**Known limitations:**
- Process stages the **whole clip** in `/dev/shm` (~12.5 MB per HD frame); long 6K clips can
  exceed `/dev/shm`. Disk staging is needed.
- Process always reprocesses the whole clip.

## Flame host behaviour (verified on Flame 2027.1 and 2027.2, Rocky Linux 9.7)

- **Every OFX action, render included, runs on Flame's main UI thread** (log tid == pid). Long
  work in render freezes Flame, so the model must never run inside render.
- Flame **renders the current frame as soon as a Batch loads**, and re-calls render on every
  revisit, every frame change and every param change.
- Fetching a chunk's frames: ~11 ms per HD float frame, both in render and during a button press
  (`changedParam`). Flame provides **32-bit float RGBA**; OFX time starts at 0 (Batch frame 1).
- **Every `setValue` the plugin makes is reported straight back as `changedParam`** (reason =
  plugin edit). Reacting to it recursed 10,678 times and crashed Flame. **Rule:** handle only
  `eChangeUserEdit`, use a re-entrancy guard, write labels only when they change (`setIfChanged`).
- Param changes from a **non-action thread** are accepted (no crash) but applied **lazily, at
  the next UI interaction**. So no auto-refresh that way, and Status updates appear late.
- **Layout:**
  - params flow into **columns of 5 rows, up to 6 columns per tab**, in definition order;
  - **each group starts a column** headed by its label, and long groups repeat the header;
  - **page membership is ignored** (three attempts): a new tab appears only on overflow, named
    after the next OFX page;
  - `kOfxPluginPropParamPageOrder` is **unknown to Flame**, and `setPageParamOrder` throws (it's
    wrapped);
  - rows per control: choice 2, boolean 1, double 1, button 1, file/dir string 2 (value + labelled
    button), **plain string 1 with no label**, label string 1;
  - text is cut at ~30 characters (paths from the left); disabled params are drawn greyed out.
- `getClipPreferences` → Alpha-only output is **ignored**: the output stays RGBA and Flame shows
  image + matte outputs.
- The node label (`kOfxPropLabel`) can't be changed: the set succeeds, but reading it back returns
  the old label.
- All renders report `interactive=0, sequential=1`, so there's **no final-render signal**.
  `hostIsBackground` (Background Reactor) is still unobserved.
- Persistent messages appear in Flame's footer; `sendMessage` shows popups, and
  `eMessageQuestion` shows Yes/No and returns the answer. Hidden params set in `createInstance`
  (`initialized`, `nodeId`) **are saved with the setup**. `describe` runs once per Flame start;
  `describeInContext` runs lazily on the first instance.
- Batch Render output contains the cached mattes. Killing Flame mid-job: the worker exits (stdin
  EOF / Flame pid gone). Stale shm files are swept at the next plugin load.
- **Python hooks:** Flame loads hook modules from `/opt/Autodesk/shared/python` (world-writable;
  other tools install there too) and `$DL_PYTHON_HOOK_PATH`; entry points as in
  `<flame>/python/hook.py` (`app_initialized(project_name)`, …). Flame's Python is 3.13.
  - `flame.schedule_idle_event(fn, delay=0)` runs `fn` on the UI thread when idle
    (`<flame>/python_utilities/examples/watch_folder.py`). **The C++ signature is
    `(function, int delay=0)`: a float delay raises ArgumentError.**
  - `PyOFXNode` methods: add_elbow, attributes, bake_expression, cache_range, change_plugin,
    clear_expression, colour_space, create_marker, delete, duplicate, get_expression,
    get_metadata, input_sockets, load_node_setup, markers, output_sockets, parent,
    save_node_setup, set_context, set_expression, sockets. `node.attributes` = pos_x, pos_y, name,
    collapsed, note, selected, type ("OpenFX"), resolution, schematic_colour, bypass, cache,
    plugin_name, preserve_input_colour_space, shot_name. **OFX params are not reachable from
    Python**, so the hook can't tell which node a job belonged to.
  - `flame` has execute_shortcut, execute_command, go_to, schedule_idle_event, …; `flame.batch`
    has current_frame (a PyAttribute, 1-based), nodes, render, go_to, …

## Model and runtime facts

- **Alpha Gen** = IC-LoRA `ltx-2.5-22b-ic-lora-alpha-gen-0.9.safetensors` on LTX-2.5-22B
  (gated HF repos `Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen`, `Lightricks/LTX-2.5`). Licence:
  LTX-2.x Community License.
- Limits: dimensions /32, ≤145 frames of the form 8n+1 (longer clips bleed RGB into the matte).
  The card says "≤1920×1088", but larger sizes run (decision 26). Empty prompt; recipe 1 = 8
  distilled steps.
- **Required files** (~73 GB): `diffusion_models/ltx-2.5-22b-distilled-transformer-bf16`
  (42.0 GB), `text_encoders/gemma4-12b-with-proj-ltx-2.5-bf16` (26.3 GB),
  `vae/ltx-2.5-video-vae-bf16` (1.47 GB), `vae/ltx-2.5-audio-vae-bf16` (0.36 GB),
  `latent_upscale_models/ltx-2.5-latent-spatial-upscaler-x2-bf16-1.0` (1.0 GB),
  `loras/ltx-2.5-22b-ic-lora-alpha-gen-0.9` (1.31 GB). `ICLoraPipeline` demands the audio VAE and
  the upscaler even with `skip_stage_2`.
- **Stack:** LTX-2 repo pinned at commit **`9ec55f9`**, `uv sync --frozen --no-dev --extra natten`
  with our lockfile → Python 3.14.7, torch 2.13.0+cu132. The code is under the same LTX licence
  ("inference-enabling code" §1.9), so it's never bundled. LTX-2 citations in this file and the
  code refer to that commit.
- **Licensing:**
  - entities with ≥ USD 10M revenue need a paid Lightricks licence (§2.1); stated on the Setup
    tab and in the README;
  - the full `LICENSE-2_x` ships with the plugin (`helper/licence.txt`); our own code is MPL 2.0;
  - the README mentions AI-content disclosure;
  - **get a legal read before a public release.**

**Measurements** (official CLI on the reference machine, RTX PRO 6000 Blackwell 96 GB; fresh
process per run incl. model load, Gemma loaded and freed in process):

| Size | Frames | bf16 peak | fp8 peak | Time |
|---|---|---|---|---|
| 1920×1088 | 49 | 44.9 GB | 27.5 GB | ~73 s |
| 1920×1088 | 97 | 51.7 GB | 34.1 GB | ~144 s |
| 1920×1088 | 145 | **58.8 GB** | **41.3 GB** | ~239 s |
| 3840×2176 | 49 | 67.8 GB | 54.1 GB | ~425 s |
| 6144×3264 | 49 / 25 | — | fails | — |

- fp8 saves ~17 GB at no speed cost. HD needs ~0.145 GB and ~1.6 s per frame.
- On a 48 GB card, fp8 HD at 145 frames fits (not resident); bf16 doesn't.
- **Size-aware estimate** (`ofx/src/vram_estimate.*`, test `vram_estimate`): need = 19.5 GB (fp8)
  or 36.8 GB (bf16) + 0.0718 GB per Mpx·frame of the padded largest chunk + 0.70 GB per Mpx above
  HD + 1 GB. It covers every measurement, within 0.4 GB at HD.
- **Resident refit:** with the transformer kept, every chunk after the first (and every chunk of
  a reused worker) runs the reference encode on top of the held transformer. A/B, HD 180 frames,
  2 × 145, fp8: chunk 1 peak 40.5 GB, chunk 2 52.2 GB allocated (18.2 held + 34.0). So resident
  need = max(non-resident, T + 1.56 + 0.1088 GB per Mpx·frame + 0.7 context + 1), T = held
  reserved (fp8 21.7, bf16 36.8), fitted on reserved memory (what free memory loses). Covers all
  12 test cases within 0.4–2.8 GB; in Flame: 720p bf16 45.9 estimated / 44.7 used, HD 180 fp8
  resident 57.9 / 57.0.
- **Frames streamed to the GPU:** `MemoryFrameSource` keeps the chunk in host memory and moves
  one frame per `take()` step, as upstream's decoder does. A/B: −3.4 GB in both chunks at HD 145
  (55.6 → 52.2 GB peak), identical mattes, same time. `--frames-on-gpu` keeps the old behaviour for
  A/B only.
- **6K:** the DiffVAE decode planner refuses (`diffusion_tiling.py:402`; minimum tile 80 frames,
  `chunked_eager`). It isn't a real OOM. Leads: the conventional VAE `ltx-2.5-video-vae-conv-bf16`
  (1.45 GB), `--diffvae-optimization` modes, the pipeline's `--tile`.

## Worker (`worker/ltx_worker.py`)

- An `Engine` (loaded once per process) drives the official `ICLoraPipeline` through
  **`stream_chunks`**, not `__call__`: `__call__`'s `pipeline_output_from_chunks` insists on
  audio. Settings: `skip_stage_2`, 2× width/height so stage 1 is native, seed 1234, LoRA 1.0 with
  `LTXV_LORA_COMFY_RENAMING_MAP`, fp8 via `QuantizationKind("fp8-cast").to_policy(transformer)`.
  Injections:
  - `ic_lora.SequentialVideoFrameSource` → `MemoryFrameSource` (frames from shm as floats 0–255,
    one frame to the GPU per step);
  - `pipeline.prompt_encoder` → `CachedPromptEncoder`;
  - `pipeline.audio_decoder = None` (video-only; `chunks/operators.py:285`);
  - with Unload off (decision 55): **class-level** `DiffusionStage._transformer_ctx` → build once,
    keyed by checkpoint + LoRA set. The single-GPU `Builder.build` ignores the per-call
    `video_tools` kwarg (ARG002);
  - `ltx_pipelines.utils.samplers.tqdm` → a stepping wrapper that reports each denoising step (the
    loop has no callback; `samplers.py:72`).
- Modes: one-shot (`--in/--chunks/--out-prefix`, used by the smoke tools), `--serve SOCKET`
  (resident server) and `--build-embeddings` (child process).
- Input colour via `colour.Converter` before padding (decision 54).
- **Per-chunk time** (HD 65 frames fp8, `--profile`): reference encode 1.9 s, transformer build
  6.6 s (only when rebuilt), denoise 60.2 s (8 steps), video decode 10.5 s; audio is skipped.
- **Prompt cache:** stores the *final* `EmbeddingsProcessorOutput` (`video_encoding` (1,1024,4096)
  bf16 + `attention_mask`), keyed by the transformer + Gemma file names and sizes.
  - Why final rather than post-projection: there's one transformer file, and fp8-cast doesn't
    touch the embeddings processor.
  - **Built in a child process** (`--build-embeddings`) before the pipeline exists: in-process,
    Gemma's memory wasn't freed (out of memory at 94 GB vs 45.5 GB).
- `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True` is set before torch loads. The decoded output
  is `(F,H,W,3)` bf16 in [0,1]; the matte = mean of the channels.
- Errors: `OutOfMemoryError` → `error oom`; anything else → `error internal` + a traceback in the
  log. Peak memory stats are reset per chunk; each chunk logs allocated-at-start and its peaks.
- **Line protocol** (worker → job.py; the dummy speaks it too): `info <text>`, `progress <f>
  <text>`, `stage <chunk> converting|preparing|denoising|decoding`, `step <chunk> <k> <n>`,
  `chunk_done <i>`, `vram …` (decision 56), `done`, `error <kind> <text>`. job.py's `Tracker`
  turns them into "Chunk i/n · frames a–b of N · step k/8 · m:ss elapsed · about m:ss left",
  redrawn every second; the time left starts after step 2 (step 1 includes the transformer build;
  decode ≈ 1.4 steps until a chunk has finished). Verified accurate in Flame. A frame counter is
  impossible: the model denoises all frames of a chunk together.
- **Verified:** `worker_smoke` HD 60 frames bf16: error 0.0029 vs truth; `job_real_smoke`
  (job.py + worker as Process runs them, 200 frames, 2 chunks): max error 0.0026; good mattes on
  real footage in Flame.

## Resident worker (decisions 11/19/33/55)

- **Server:** `ltx_worker.py --serve ~/.cache/ltx-alphagen/run/worker.sock`, plus `worker.sock.json`
  `{pid, precision, model_dir, state loading|ready|busy|failed, vram_gb}`; its stdout/stderr go to
  `run/worker.out`. It handles one job per connection, sequentially.
- **A request with another precision or model_dir** gets `restart`; the server exits and the client
  starts a matching one.
- **The server exits after a job with `unload: true`,** after 5 min idle, or when `flame_pid` dies.
- **`job.py` uses `ResidentChannel` for the real worker:** it starts the server (detached, its own
  session) under an `flock`, connects, and streams the same lines as before; Cancel =
  `killpg(server)`; a server it started itself is checked through its Popen (a killed child is a
  zombie that still answers `kill(pid, 0)`). The dummy worker keeps `SubprocessChannel`.
- **`tools/resident_smoke.py`** (15 checks): start-and-stay; reuse (22 → 11 s); a precision change
  replaces the server; unload leaves no worker on the GPU; idle timeout; Flame death; a mid-job
  kill is reported as "ended unexpectedly (signal 9)" with no shm leftovers; ACEScg input made by
  inverting the view gives a correct matte. The socket closes before a killed server has finished
  dying, so job.py waits up to 5 s before classifying.

## Setup (decisions 2/7/13/20/51)

- **`helper/setup_helper.py`** (stdlib only, run by the system python3), launched by Download with
  `HF_TOKEN` in the env (optional) and `--existing`. In order:
  1. **Runtime** in `<Model Folder>/runtime/`: uv (sha256), LTX-2 tarball at the pinned commit,
     `uv sync --frozen --no-dev --extra natten`, `opencolorio==2.6.0`, then a torch/CUDA/OCIO
     smoke test.
  2. **Models:** keep if already verified (`*.verified` marker: sha + size + mtime); copy from
     Existing Models while hashing; or download from HF while hashing. 401/403 → the
     "accept the model terms at huggingface.co/<repo>" message.
  3. **Prompt cache** (worker `--build-embeddings`).
  4. **Config** `model_dir`, `ltx_python`, `runtime_commit`, `models_ready`, written only on success.
- **`helper/ltx2-uv.lock`:** Lightricks' repo ships **no `uv.lock`**. Without one every install
  would resolve the newest versions on that day, so we ship the lockfile resolved at the pinned
  commit (torch 2.13.0+cu132, ltx-core 1.4.2). Setup copies it in, then runs `uv sync --frozen`.
- **`helper/model_manifest.json`:** uv 0.12.18 (size + sha256), the LTX-2 commit, and per file:
  repo, revision, `path` in the Model Folder, `hf_path` in the repo (the LoRA sits at the
  Alpha-Gen repo root), size, sha256; plus `runtime.uv_cache_bytes` for the progress bar. **The
  SHA-256 values were computed from local copies whose sizes match the HF revisions; they are
  UNVERIFIED against HF until the first authenticated download.**
- **Progress for every download:** models and uv show x of y; the LTX-2 source shows MB done
  (GitHub archives have no size); `uv sync` prints nothing off a terminal, so a watcher thread
  shows the uv cache's growth against `uv_cache_bytes` (6.6 GB, measured). The watcher also
  catches Cancel while uv is silent. Network steps retry twice (PyPI and GitHub release assets
  have timed out). Errors show uv's `error:` + last `cause:` line, not its trailing `hint:`.
- Trailing `/` in folder params is stripped (".../models/" would otherwise look like another
  folder and restart the worker).
- **Plugin Setup tab:** Licence column; Models column = Model Folder, Existing Models, status;
  Support column = Debug Logging, Collect Logs. Ready = `models_ready` + `ltx_python` exists.
- **`tools/setup_smoke.sh`** (isolated HOME, 11 checks): fresh install with copies from Existing
  Models (~10 min); a re-run keeps all files (~30 s); missing file without a token → clear error;
  fake token → the accept-terms hint, and the token isn't in the log; a same-size wrong-content
  file is rejected.

## Logging (decision 39)

- `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`, shared by all components (plugin, job, worker,
  setup, hook) and rotated at plugin load (10 MB × 5). Line format: `YYYY-MM-DD HH:MM:SS.mmm LEVEL
  comp[pid/tid] node=<id> job=<id> | msg`. C++ (`log.cpp`) and Python (`ltxag_worker/log.py`)
  must stay identical; `test_log.py` checks it.
- At plugin load: the environment snapshot (plugin version + git, host, OS, glibc, CPU, RAM,
  `/dev/shm`, GPU/driver/VRAM, config without secrets) plus sweep results.
- At INFO: every non-render action. At DEBUG: renders and cache hits.
- Per job: params, chunk plan, timings, worker output (tracebacks at WARN), the exit
  classification, and the VRAM record (decision 56; also `logs/vram_stats.csv`).
- Never logged: the HF token (length only), image data, clip or project names.
- **Collect Logs** → `~/ltx-alphagen-support-<ts>.tar.gz` (the `logs/` folder + `environment.txt`
  + `jobs/`). Debug logging is per user (Setup tab).

## Building and testing

- **Build on Rocky 9** (or an equivalent glibc ≤ 2.34 system with GCC 11): binaries built on newer
  distributions won't load in Flame. `HOST=<ssh host> scripts/build_remote.sh` syncs, builds a
  release build and runs ctest there; `scripts/package_release.sh` makes the tarball.
- Local test builds on any Linux: `cmake -S . -B <build> -DWORKER_TEST_PYTHON=<python with numpy>`
  (it needs numpy importable under `python -I`); ctest runs host-free.
- **Build facts:**
  - **C++14**: OFX 1.4 Support uses dynamic exception specs and `auto_ptr`;
  - **libstdc++ is linked dynamically**: Flame and other OFX plugins use the system GCC 11
    libstdc++, and `libstdc++-static` isn't installed on Rocky;
  - the `.ofx` needs glibc ≤ 2.34 and exports only the two OFX entry points;
  - `setEvaluateOnChange` lives on `ValueParamDescriptor`;
  - `posix_spawn` needs absolute paths;
  - workers run as `python -I -B`, so dependencies must be in the interpreter's own site-packages;
  - `-DLTXAG_GIT=<hash>` sets the build hash when building from a copy without `.git`.
- **Tests (ctest, 7):** `chunk_plan`, `worker_python` (padding, transport, job incl. the progress
  Tracker, the VRAM CSV and the refresh marker, the Flame hook against a fake `flame` module, log),
  `worker_roundtrip`, `cache`, `marker_log` (marker, overlay, log format), `vram_estimate` (every
  measurement covered), `load_bundle`. Against the real model on a GPU machine:
  `tools/worker_smoke.py`, `job_real_smoke.py`, `resident_smoke.py`, `setup_smoke.sh`. All are
  host-free: re-entrancy bugs like the setValue recursion would need a fake OFX host (not built),
  so they are found by testing in Flame.
- **Installer:** `packaging/install.sh` installs the bundle into `/usr/OFX/Plugins` and the hook
  into `/opt/Autodesk/shared/python`, refuses to replace a hook file that isn't ours, and has
  `--uninstall`. It replaces a symlinked dev install with a copy.

## Roadmap (after v0.1)

- **6K:** try the conv VAE / DiffVAE modes / `--tile`.
- Large jobs: disk staging when `/dev/shm` is too small; skip already-cached chunks.
- **Chunk length for 48 GB cards (PROPOSED, 2026-10-12):** chunks are fixed at 145 frames, so on
  a 48 GB card (e.g. RTX 6000 Ada) HD fp8 needs ~42 GB of ~45 GB free and only just fits next to
  Flame; Unload off (~58 GB) can't. Offer a Chunk Length (or pick the longest that fits): ~35 GB
  at 97 frames, ~28 GB at 49, at the cost of more crossfade seams. Ada itself is supported by
  fp8-cast (the Triton upcast needs sm_89+, `ltx_core/quantization/fp8_cast.py:62-95`);
  speed and natten kernels on Ada are UNVERIFIED.
- Background Reactor check (`hostIsBackground`, cached mattes).
- Decision 56 step 2 (local estimate correction), once `vram_stats.csv` data exists.
- An offline install of the Python packages (decision 57).
- Optional: an A/B of recipe 2 (`alpha_gen.py`: dev transformer, 30 steps; its negative prompt is
  unused at cfg 1.0) on hair/smoke; carrying the previous chunk's matte as conditioning for seams.
- A macOS / MLX port is being considered (`ofx/TODO.md`).

## Dead ends: don't retry

- Running the model inside `render()`: it freezes Flame's UI thread.
- A RAM matte cache: lost when the Batch reloads; Flame renders on load.
- `setPageParamOrder`/`kOfxPluginPropParamPageOrder`: unknown to Flame, and it threw in
  `describeInContext` (the plugin wouldn't attach).
- OFX pages to make tabs: ignored in three variants (params on pages; groups+params on pages;
  only groups on pages). `kOfxParamPropGroupIsTab` doesn't exist in OFX 1.4.
- Renaming the node (`kOfxPropLabel`) for progress: ignored by Flame.
- `kOfxImageEffectPropInteractiveRenderStatus` to detect final renders: always 0 in Flame.
- `getClipPreferences` Alpha-only output: ignored; the output stays RGBA.
- Reacting to plugin-edit `changedParam` events: infinite recursion → Flame crash.
- **Auto-refresh attempts that failed:** setting params from a background thread (applied only
  at the next UI event); in the hook: idling on the UI thread, setting OFX params from Python (not
  exposed), `current_frame.set_value(same frame)` (a no-op); toggling Bypass (hits every LTX
  node, since the hook can't identify the finished one, and leaves them black if the restore
  fails); `schedule_idle_event(fn, delay=0.5)` (float delay → ArgumentError; the undo never ran).
- Building the Gemma cache in the worker process: Gemma's memory stays held → OOM.
- Off-the-shelf static libstdc++: not installed on Rocky, and unnecessary.
- nvfp4 (Blackwell-only, needs a compile step) and the int8 `comfy-int8-convrot` checkpoint (no
  loader outside ComfyUI).
- Assuming Lightricks' repo has a `uv.lock`: it doesn't (setup failed with `--frozen`); ship ours.
- `ModelRegistry(cache_weights=True)` to avoid rebuilding the transformer: worse (chunk 1 200 s
  instead of 80, +16 GB). Keep the built transformer via `_transformer_ctx` instead.
- Patching `_transformer_ctx` on the **instance**: `with_loras()` `copy.copy()`s the stage, so the
  patched copy built the base model **without the IC-LoRA**. The matte reproduced the input
  picture. Patch at class level, keyed by LoRA set.
- `ICLoraPipeline.__call__` with `audio_decoder=None`: "expected decoded audio". Use
  `stream_chunks`.
- Checking a child process with `kill(pid, 0)` after killing it: zombies answer. Use `Popen.poll()`.
- C++ OCIO in the shim: superseded by decision 54 (not needed with a fixed dropdown).
- Preflight counting a resident worker's memory only when its precision matches: a mismatch
  restarts the worker and frees it, so this refused jobs that fit.
- Single-threaded `applyRGB` on a whole chunk: 0.8 s per 2K frame; use one thread per frame.
- Holding a chunk's frames on the GPU as float32: 3.4 GB wasted at HD 145; stream per frame.
- A flat "+9.4 GB" for a resident transformer: under-estimated HD 145 by 8.6 GB.
- Watching for a process with `pgrep -f '<text>'` over ssh: it matches its own command line. Use
  marker files.

## Conventions

- Verify every API, flag or config key against installed source or docs before using it, and
  cite where (file and line, or the command). Mark anything unconfirmed UNVERIFIED.
- Ask the maintainer rather than assume when a request has two readings.
- Testing has two layers: the unit tests, and a demonstration of each requirement (in Flame for
  anything host-related). Passing unit tests alone is not evidence.
- Keep this file current: record decisions with their numbers and reasons, move items between
  PROPOSED/OPEN/DECIDED, and add dead ends to the list above. Nothing machine- or person-specific:
  that goes in `CLAUDE.local.md`.
