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
    and other apps hold (NVML per process, `platform_linux_nvml.cpp`). A live resident worker's memory always
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
- **30.** Chunks of 145 frames, overlap ≥ 24, crossfade at the middle of the overlap (decisions
  61/62 made the size and the blend selectable).
- **61.** **Chunk Size dropdown: Auto, Full 145, Half 73, Quarter 33** (DECIDED, user 2026-10-12;
  `kSizeLen`/`kSizeOverlap` in `chunk_plan.h`). All 8n+1 (a quarter of 145 is 36.25, and 37 is not
  8n+1: 33 is the nearest valid size; a unit test checks it). Overlap goes with the size, 24 / 16 /
  12 frames, not a separate param. **Auto (the default)** takes the longest size whose
  `estimateVramNeed` fits free + resident worker − reserve, else the smallest, and Full without
  NVML (extends decision 34). The size is chosen in `process()` from the crop size (decision 63),
  and the preflight uses the chosen chunks. Not in the cache key (Process overwrites the whole
  clip). Trade-off: more seams and less temporal context, so the model may pick another main
  object per chunk (README warns).
- **62.** **Merge dropdown: Smooth (default) or Flow Warp** (DECIDED, user 2026-10-12). Smooth =
  the crossfade weights shaped by smoothstep (`frameContributions(..., smooth)`), computed in
  C++. Flow Warp = `worker/ltxag_worker/morph.py`, **numpy only** because job.py runs under the
  system python3 (no OpenCV dependency; none of Setup changes): Horn-Schunck coarse to fine on a
  copy reduced to ≤ 512 px, one flow each way, then
  `a_t = a(x + t·F_ba)`, `b_t = b(x + (1−t)·F_ab)`, `out = (1−t)·a_t + t·b_t` (exact at t = 0
  and 1), clamped to 5% displacement, a plain blend if the mattes differ by < 0.01. Measured on
  synthetic discs: error 8–10× lower than a dissolve (6–40 px apart), 0.36 s per HD frame, so
  about 9 s per 24-frame seam against ~4 min of inference per chunk. Applied only on the blended
  frames, in job.py's `finalize`. It does not fix a switch of the main object. Not in the cache
  key. Open: an ML morph, or conditioning the next chunk on the previous matte.
- **63.** **ROI = the picture's own alpha** (DECIDED, user 2026-10-12; `roi.*`, `plugin.cpp`
  `computeRoi`). **History:** first built as an optional second clip "ROI Matte". That needs the
  General OFX context; offering Filter too made Flame pick Filter (log `context=2 roi clip
  absent`), General-only made the plugin load but needed every node re-created (a changed input
  list invalidates saved nodes), and a second socket plus alpha plus channel choice confused the
  user. So: **no ROI input, back to the Filter context only.** Re-creating existing nodes is needed
  once more because the inputs changed again. Behaviour:
  - **No switch, auto-detected at Process:** if the source's alpha is fully opaque (nothing < 0.99
    in any frame, or an RGB image) there is no ROI and the whole frame is processed; else the
    alpha is the matte. Empty matte (nothing above 0.05 anywhere) = an error. A clip that carries
    an unrelated varying alpha would be cropped: the `ROI` info field ("Crop WxH" or "None (alpha
    opaque)") and the log (`process: ROI from source alpha …`) show it;
  - the box is the **union over all frames** of pixels above 0.05 (user: G), grown by **ROI Margin**
    (default 64, minimum 32), grown again to **at least 512 px per side** (user: F; by enlarging
    around the centre, not scaling: no resampling, more context), rounded up to a **multiple of
    32**, slid inside the image;
  - the plugin crops while staging (the model, the VRAM estimate and `/dev/shm` all see the crop);
    the manifest carries `width/height` = crop, `full_width/full_height`, `roi`; job.py pastes
    each blended matte into a black full frame before caching, so the output size is unchanged;
  - **Black Out Outside ROI** (default off, user: E): the staged source × its alpha, in the
    source's own colour space (0 stays near black after the OCIO conversion). No dilation: the
    user dilates upstream. Unverified whether the model likes it: A/B on a test clip;
  - **Multiply Result by ROI** (default off, user: D): result × the source alpha **at render
    time** (`applyRoiLimit`), so it toggles without reprocessing;
  - **cache key:** `|roi=x,y,w,h,k<mask>` for an active ROI, from the hidden `roiState` written at
    Process (active, box, mask, settings `m<margin>k<mask>`). No ROI: the old key, so earlier
    caches stay valid. If the ROI settings differ from the stored ones, or nothing was processed
    and the frame's alpha is a matte, the key is `|roi=unprocessed` (a miss: NOT PROCESSED).
    Changed alpha *content* is not detected: press Process again (README says so);
  - Flame may not show OFX param hints (every param has one), so labels say what they do. The
    alpha reaching the plugin from Flame's picture input is as observed: source is delivered as
    RGBA (UNVERIFIED: that a matte connected to the node's matte socket arrives in that alpha).
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
  Alpha Gen, Performance, Chunking, ROI and Cache plus one blank spacer column (6 columns; was
  three spacers before decisions 61/63), so Licence, Models and
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

**Platforms**
- **64.** **OS-specific code sits behind one small layer per language** (DECIDED, user
  2026-10-12, prep for the macOS/MLX port; done on `main` as a pure refactor, then the
  `macos-mlx` branch is rebased onto it). C++: `ofx/src/platform.h` (`stagingDir`, `queryVram`,
  `queryGpuProcesses`, `processName`, `platformSnapshot`, `showTextFile`), implemented in
  `platform_linux.cpp` + `platform_linux_nvml.cpp`, chosen in CMakeLists.txt (another OS stops
  configure with an error until it has its own files). Python: `worker/ltxag_worker/oslayer.py`
  (the progress window with Cancel, message windows, `process_name`), shared by job.py and
  setup_helper.py. job.py takes the staging folder from its manifest's folder, so only the plugin
  knows it. Linux behaviour is unchanged (same zenity calls, same snapshot lines; the "NVML
  unavailable" texts now read "GPU memory unavailable"/"memory query unavailable").
  - **Not in the layer, on purpose:** the inference backend (CUDA/torch/LTX-2 vs MLX) is a
    separate axis from the OS. The worker line protocol and the manifest are already its
    boundary, so an MLX worker speaks the same protocol. The backend-only parts (an MLX runtime
    in Setup, a `backend` manifest field and cache-key part, MLX constants in `vram_estimate`)
    are built on `macos-mlx`, where they can be tested. `half.h` (F16C) and `process.cpp`
    (`pipe2`, `posix_spawn_file_actions_addclosefrom_np`) get their macOS variants inside those
    files on that branch. `ltx_worker.py` (CUDA, requires input in /dev/shm) stays Linux-only.
  - **macOS (built on `macos-mlx`):** `platform_macos.cpp` + `platform_macos_metal.mm`. Staging
    in the per-user temp folder (`confstr(_CS_DARWIN_USER_TEMP_DIR)`, on disk, not RAM);
    `queryVram` = Metal `recommendedMaxWorkingSetSize` as total, free = min(total, free +
    inactive pages from `host_statistics64`), driver = the macOS version; `queryGpuProcesses` is
    empty (no per-process GPU memory API); `processName` = `proc_name`; licence in TextEdit
    (`open -e`). Inside the shared files: `half.h` NEON, `process.cpp` `pipe`+`FD_CLOEXEC` and
    `POSIX_SPAWN_CLOEXEC_DEFAULT` (also strips `DYLD_*`), `transport.cpp` `F_PREALLOCATE`
    (`F_ALLOCATEALL`) + `ftruncate`, `log.cpp` `pthread_threadid_np` (same id as Python's
    `threading.get_native_id`, checked on the Mac).
  - **macOS side of `oslayer.py`:** the progress window is `ltxag_worker/mac_progress.swift`, an
    AppKit window that reads zenity's stdin protocol, compiled once per source version with
    `xcrun swiftc` into `~/.cache/ltx-alphagen/bin` (first build ~1 min while swiftc fills its
    module cache, then seconds; cached after). Tested on macOS 15: osascript's AppleScript
    `progress` draws nothing, the system python3's Tk 8.5 aborts at `Tk()`, a JXA Cocoa window
    works but needs the untyped ObjC bridge. Messages: osascript `display dialog` (text as argv).
    `process_name`: basename of `ps -o comm=` (the full executable path when comm is the last
    column; Flame's is `.../bin/flame.app/Contents/MacOS/flame`, so "flame" as on Linux).
    Checked on the Mac over ssh (console user logged in): the helper builds (swiftc 6.2.4, 5 s with
    a warm module cache), "# text"/42/100 exits 0, end of input exits 0, its window is on screen
    (CGWindowList: 520 wide, floating layer); the `display dialog` window appears too. Not
    checked: clicking Cancel / the close button (exit 1), and how both look.
  - **macOS, other Python files:** the hook takes the bundle from `/Library/OFX/Plugins` (where the
    Mac's other OFX plugins live); `/opt/Autodesk/shared/python` exists on the Mac too
    (world-writable, empty), but that Flame loads hooks from it there is UNVERIFIED. `test_job`
    stages in the temp folder off Linux. job.py needs numpy, and macOS's `/usr/bin/python3`
    (Command Line Tools 3.9) has none, so on macOS the plugin launches job.py with the runtime's
    python (`ltx_python`, `plugin.cpp` `process()`; Process already refuses without it). All
    worker tests pass on the Mac under that python (job.py end to end with the dummy worker).
    setup_helper.py stays on `/usr/bin/python3` (stdlib only); without the Command Line Tools
    that is Apple's install-prompt stub, so Setup's own "Command Line Tools are missing" error
    can't appear: the plugin should check `xcode-select -p` before launching it.
- **65.** **macOS ships on PyTorch-MPS first; the from-scratch MLX port comes later** (DECIDED,
  user 2026-10-12). Upstream LTX-2 supports Apple Silicon (`mps-sdpa` attention on darwin/arm64,
  an MPS LoRA-fuse path, eager neighbourhood attention in the VAE without natten), and our
  `helper/ltx2-uv.lock` already resolves for macOS, so the Mac worker can drive the same
  `ICLoraPipeline` as Linux. Measured MPS vs MLX data for diffusion transformers does not exist;
  op-level benchmarks put MLX at most ~2x ahead, so the M1 Ultra itself is the limit. The MLX
  port (`mlx/PORT_MAP.md`) is worth shipping only if it clearly beats MPS on speed or memory.
  - **Measured on the M1 Ultra** (128 GB, 96 GB Metal working set, torch 2.14.1 from our lockfile's
    darwin entry, `tools/worker_smoke.py`, bf16, 2026-10-12):

    | Clip | Encode | Steps (8) | Decode | Job | Peak held | Matte error |
    |---|---|---|---|---|---|---|
    | 720p × 49 | 126 s | 440 s (55 s/step) | ~69 s | 674 s | 52.3 GB | 0.0027 |
    | HD × 33 | 188 s | 776 s (97 s/step) | ~105 s | 1112 s | 60.4 GB | 0.0033 |
    | HD × 73 | 406 s | 2120 s (265 s/step) | ~220 s | 2861 s | 79.7 GB | 0.0030 |

    About 50 s per new HD frame for Half and Quarter alike; HD × 145 would need ~115 GB (over the
    working set) and ~2.2 h. CUDA for comparison: HD × 60 in 85 s.
  - **M1 has no fast bf16:** MPS bf16 conv3d 0.40 TFLOPS vs fp32 0.92 and fp16 4.74 (fp16
    overflows to NaN in the VAE encoder, even with PixelNorm in fp32); matmul bf16 8.9, fp16 16.4,
    fp32 13.0 TFLOPS. So on MPS the worker encodes the reference in **fp32** (injection 6,
    `ReferenceEncoderInDtype`): 285 → 126 s at 720p × 49, same matte. The transformer stays bf16
    (fp16 there is a possible later speed-up, with overflow risk).
  - **bf16 only:** MPS has no fp8 dtype (`Undefined type Float8_e4m3fn`). The Mac default is bf16;
    fp8 selected on a Mac is refused at Process with a message (decision 34: no silent downgrade).
  - **Memory model** (`vram_estimate`, `mps` flag): need = 44.5 + 0.231 GB per Mpx·frame + 1,
    fitted to the three runs above (peak = lowest free working set). Free memory on the Mac =
    min(working set, available RAM) (`platform_macos_metal.mm`), so Auto picks Half or Quarter
    there. Over-committing Metal swaps instead of raising an OOM; the warning text says so. The
    resident-transformer case is not measured on MPS (UNVERIFIED).
  - **job.py runs under the runtime's Python on macOS** (`ltx_python`): the system python3 of the
    Command Line Tools has no numpy. Setup still runs under the system python3 (stdlib only).
  - **mps-sdpa's compiled bridge is not needed:** it builds only with `ninja` on PATH and then
    fails on this Mac's stale Command Line Tools C++ headers, but its fallback is as fast (255 ms
    per 12.9k-token attention call either way).

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
- `ofx/src/chunk_plan.*`: the chunk planner + crossfade weights (linear or smoothstep) + the
  Chunk Size table.
- `ofx/src/roi.*`: ROI scan, box math and matte rows (pure, tested in `test_roi`).
- `ofx/src/cache.*`: cache keys, read with mtime refresh, pending markers, purges, the age+LRU
  sweep, and the stale `/dev/shm` sweep.
- `ofx/src/transport.*`: shm frame files, written with `write()`, never mmap (no SIGBUS inside
  Flame).
- `ofx/src/process.*`: `posix_spawn` with its own process group and a cleaned env (strips
  `PYTHON*`, `LD_LIBRARY_PATH`, `LD_PRELOAD`, `HF_TOKEN`); detached launch via `sh &`.
- `ofx/src/vram_estimate.*`: the memory model (decision 34/56).
- `ofx/src/platform.h` (decision 64): `platform_linux.cpp` (staging in /dev/shm, /proc, zenity
  licence window) and `platform_linux_nvml.cpp` (NVML via dlopen; device and per-process memory).
- `ofx/src/log.*`, `env_info.*`, `marker.*` (marker + overlay), `user_config.*`, `half.h` (F16C;
  NEON on ARM64).
- `worker/job.py`: the detached job controller (resident/subprocess channels, progress Tracker,
  VRAM record, refresh marker). Manifest: `job_id node_id input width height frames chunks
  contrib frame_keys cache_root worker [worker_python worker_args worker_socket model_dir unload
  colour ocio_config seed] seconds simulate precision vram flame_pid title` plus, since decisions
  62/63, `merge` ("smooth"|"flow"), `full_width full_height` and `roi` [x,y,w,h] (with an ROI,
  `width/height` are the crop).
- `worker/ltx_worker.py`: the real worker (see "Worker"). `worker/dummy_worker.py`: a luminance
  "matte" with `--simulate oom|crash|hang`, same line protocol.
- `worker/ltxag_worker/{padding,transport,cache,log,colour,morph,oslayer}.py`.
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
- Plugin id `com.janklier.ltx.alphagen`, version from CMake `LTXAG_VERSION` (0.1.3; the OFX
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
  - MPS only: `pipeline.image_conditioner` → `ReferenceEncoderInDtype(float32)` (decision 65).
- Device: CUDA or MPS through `Accel` (memory queries, peaks sampled on MPS, the OOM type
  `torch.OutOfMemoryError` for both).
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
- **macOS (decision 65):** the manifest's `uv` has one entry per `sys.platform` (darwin:
  `uv-aarch64-apple-darwin.tar.gz`); `runtime.uv_cache_bytes_darwin` drives the bar (1.4 GB; a
  darwin sync left 1.3 GB). Same `uv sync` command (every entry of the `natten` extra in
  `helper/ltx2-uv.lock` carries `sys_platform == 'linux'`, so it adds nothing; darwin gets torch
  2.14.1 from PyPI, Linux 2.13.0+cu132). The
  smoke test asserts `torch.backends.mps.is_available()`. Instead of gcc, Setup requires the
  Command Line Tools (`xcrun --find clang++`): no Triton on macOS, but `mps-sdpa` builds its
  Objective-C++ bridge on first use (torch `cpp_extension`: clang++, Python.h, and **ninja found
  on PATH**, else it falls back to its pyobjc MPSGraph path, `backends/mpsgraph_zc.py:43-55`), and the progress window is Swift.
  The lockfile installs ninja into the venv's `bin/`, but torch looks for it on PATH only
  (`torch/utils/cpp_extension.py:2561-2568`, `subprocess.check_output(['ninja', '--version'])`)
  and nothing puts the venv's `bin/` on the worker's PATH, so as built the worker probably takes
  the fallback (UNVERIFIED in a real run; the fix would be in whatever starts the worker).
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
- **macOS build facts** (Apple Silicon, Command Line Tools: Apple clang 17, MacOSX 26.2 SDK,
  cmake 4.4; `HOST=… REMOTE=… CMAKE=… scripts/build_mac.sh`):
  - bundle binary in `Contents/MacOS/LTXAlphaGen.ofx` (arm64 Mach-O bundle, minos 15.0), as the
    installed Mac OFX plugins use; a minimal `Contents/Info.plist` is generated (most installed
    plugins carry one; whether Flame needs it is UNVERIFIED);
  - exports via `-Wl,-exported_symbols_list` with the vendored `Support/include/osxSymbols`;
    ld's default `-undefined error` replaces `--no-undefined`; links Metal + Foundation, libc++,
    libobjc, libSystem only; the linker signs it ad hoc (no real signing/notarisation yet);
  - a CLT install with a stale `usr/include/c++/v1` hides the SDK's libc++ headers (`'string'
    file not found`): pass `-stdlib++-isystem <SDK>/usr/include/c++/v1` in
    `CMAKE_CXX_FLAGS`/`CMAKE_OBJCXX_FLAGS` (via `CMAKE_ARGS`).
- **Tests (ctest, 8):** `chunk_plan` (all sizes, linear and smooth), `roi` (box math and channel
  choice), `worker_python` (padding, transport, job incl. the progress Tracker, the VRAM CSV and
  the refresh marker, the ROI paste-back and flow-merge paths, `test_morph`, the Flame hook against
  a fake `flame` module, log),
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
- **Chunk length for 48 GB cards: built as decision 61 (v0.2, not yet tested in Flame).** On a 48
  GB card (e.g. RTX 6000 Ada) HD fp8 needs ~42 GB of ~45 GB free at 145 frames; Half/Quarter
  should fit (the estimate gives the numbers; `vram_stats.csv` from that friend would confirm).
  Ada itself is supported by
  fp8-cast (the Triton upcast needs sm_89+, `ltx_core/quantization/fp8_cast.py:62-95`);
  speed and natten kernels on Ada are UNVERIFIED.
- Background Reactor check (`hostIsBackground`, cached mattes).
- Decision 56 step 2 (local estimate correction), once `vram_stats.csv` data exists.
- An offline install of the Python packages (decision 57).
- Optional: an A/B of recipe 2 (`alpha_gen.py`: dev transformer, 30 steps; its negative prompt is
  unused at cfg 1.0) on hair/smoke; carrying the previous chunk's matte as conditioning for seams.
- **macOS / MLX port** (branch `macos-mlx`): written from scratch in MLX, no community port code
  (user, 2026-10-12: learning, and one licence set, LTX's). The map of what has to be ported is
  `mlx/PORT_MAP.md` (code paths at `9ec55f9`, weights, sizes, speed estimate, risks).

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
