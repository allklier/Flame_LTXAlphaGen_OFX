# ofx/ — the C++ plugin

Part of the design notes (root `CLAUDE.md` has the overview and the decision index).

## Source files

- `src/plugin.cpp`: the effect (params, layout, gate, preflight, Process, render, defaults,
  Collect Logs, job and setup watchers). `chunk_plan.*`: planner, crossfade weights, Chunk Size
  table. `roi.*`: ROI scan and box math (pure). `cache.*`: keys, reads (mtime refresh), pending
  markers, purges, age+LRU sweep, stale staging sweep.
- `src/transport.*`: staged frame files, written with `write()`, never mmap (no SIGBUS in Flame).
- `src/process.*`: `posix_spawn` in its own process group with a cleaned env (strips `PYTHON*`,
  `LD_LIBRARY_PATH`, `LD_PRELOAD`, `HF_TOKEN`; macOS also `DYLD_*`); detached launch via `sh &`.
- `src/vram_estimate.*`: the memory model (decisions 34/56). `src/log.*`, `env_info.*`,
  `marker.*` (marker + overlay), `user_config.*`, `half.h` (F16C; NEON on ARM64).
- `src/platform.h` (64): `stagingDir`, `queryVram`, `queryGpuProcesses`, `processName`,
  `platformSnapshot`, `showTextFile`; CMakeLists.txt picks the files (another OS stops configure).
  Linux: `platform_linux.cpp` (/dev/shm, /proc, zenity licence window) + `platform_linux_nvml.cpp`
  (NVML via dlopen). macOS below.

## Params, defaults, gate

- **11/19.** Per-node "Unload GPU": earlier nodes resident, last node unloads (`worker/CLAUDE.md`).
- **12/32.** Per-node VRAM reserve for Flame in GB; built-in default = 5% of the VRAM *free* at
  plugin load.
- **18/21.** Settings per user (`~/.config/ltx-alphagen/config.conf`). Three-level defaults for
  precision, unload and reserve: built-in → user default ("Save Default") → per node.
- **34.** No silent downgrade. (a) Built-in default precision is bf16 only if a full HD 145-frame
  chunk fits in free VRAM minus the reserve, else fp8. (c) Each run warns if the node's precision
  may not fit (56). Both use the size-aware estimate.
- **38.** Licence + model gate: release builds enforce it; dev builds bypass it unless "Enforce
  Gate". Ready = `models_ready` + `ltx_python` exists + accepted licence hash.
- Setup is its own process; the node learns it finished (Mac test 2026-10-13: "Models: NO" until
  the next edit) from a **setup watcher** thread (config polled every 2 s, up to 4 h;
  `refreshSetupUi` off-thread) and from **render** when the gate opens. UNVERIFIED in Flame.
- Trailing `/` in folder params is stripped (".../models/" would look like another folder and
  restart the worker).

## UI and outputs

- **37/52.** Layout follows Flame's rules (below). Tab 1: Alpha Gen, Performance, Chunking, ROI,
  Cache + one blank spacer column (6 columns), so Licence, Models, Support overflow to tab 2, which
  Flame names "Setup" after the 2nd OFX page.
- **41.** Plain string fields get a caption label above them ("Token used HH:MM, not stored").
- **44.** Frames without a matte show a faint "no media"-style marker in the matte output: stripes
  at 0.08, caption at 0.30 (PROCESSING / NOT CONNECTED / NOT SUPPORTED; "Show Marker" off gives
  plain black), and, always drawn, SETUP REQUIRED / MATTE MISSING / FAILED / CANCELLED (59, 71).
- **46.** Paths logged as-is; Collect Logs asks users to review the bundle. **48.** Flame forces an
  RGBA output; alpha (Flame's matte output) carries the matte or marker.
- **58.** **Image output = matte overlay:** source × (1 − 0.3·m) + magenta (1, 0, 1) × 0.3·m per RGB
  channel, in the source's colour space (`overlayPixel`, `marker.h`; 50% was too opaque). Frames
  without a matte show the plain source. Render-only: the cache key is unchanged.
- **59.** **SETUP REQUIRED shows on both outputs** (image = the marker, grey on black), so a node
  viewed on its image output doesn't look like a working pass-through (0.1.1).
- **71.** **Processed record** (DECIDED, user, 2026-10-13): never processed → source + white matte
  (footer "Not processed"). Processed and no matte (purged/swept, another machine, source or
  settings changed, failed/cancelled job) → MATTE MISSING etc. on both outputs, an error footer,
  then `render` throws `kOfxStatFailed`, so a buried node can't fail silently. The record is the
  hidden `roiState` (written by every launched Process since 0.1.3, saved and copied with the
  node); Purge Cache (node) clears it. A duplicate errors too: a copy shares every param, and a
  content record couldn't tell it from an upstream change (user chose the error). How Flame
  shows a failed render, and whether Batch Render stops: UNVERIFIED (test in Flame).
- **PROCESSING race (49):** the plugin writes the frames' pending markers itself (Flame's pid)
  just before launching; job.py rewrites them with its pid. Otherwise Flame's render right after
  the button press beat job.py (~50 ms) and showed NOT PROCESSED for the whole job. The plugin's
  watcher clears them if no job status appears within 60 s.

## Cache (decision 36)

- `<root>/<k[0:2]>/<key>.matte` (LTXM header + fp16, ~4 MB per HD frame); key = hash(source frame
  hash, `v1|cs=…|prec=…|model=ltx2.5-alphagen0.9-recipe1` or `dummy-0.3`, + ROI part, 63).
- Also in the root: `pending/<key>` (job pid), `jobs/<id>.json` (state, chunks_done,
  frames_done), `prompt-empty.safetensors`. Default root `~/.cache/ltx-alphagen/mattes`; sweep 30
  days / 50 GB cap in `describe` (once per Flame start). Purge Cache (node), Purge All.
- Known approximation: an identical source frame in another clip context reuses the first matte.

## Chunks, merge, ROI

- **26.** No resolution cap, no scaling in the OFX; VRAM is the only limit.
- **30/61.** Chunk Size dropdown **Auto, Full 145, Half 73, Quarter 33** (user 2026-10-12;
  `kSizeLen`/`kSizeOverlap` in `chunk_plan.h`), all 8n+1 (33 is the nearest valid quarter; a test
  checks it). Overlap goes with the size, 24 / 16 / 12. **Auto** (default) takes the longest size
  whose `estimateVramNeed` fits free + resident worker − reserve, else the smallest; Full without
  GPU memory info. Chosen in `process()` from the crop size; the preflight uses the chosen chunks.
  Not in the cache key (Process overwrites the whole clip). Trade-off: more seams, and the model
  may pick another main object per chunk (README warns).
- **62.** Merge: **Smooth** (default) = crossfade weights shaped by smoothstep
  (`frameContributions(..., smooth)`), computed here; **Flow Warp** is done by job.py
  (`worker/CLAUDE.md`). Not in the cache key.
- **63.** **ROI = the picture's own alpha** (user 2026-10-12; `roi.*`, `computeRoi`). A second
  clip "ROI Matte" needed the General context (Flame then picked Filter, or every node had to be
  re-created) and confused users, so: **no ROI input, Filter context only.** Behaviour:
  - auto-detected at Process: an alpha that is opaque in every frame (nothing < 0.99, or RGB) = no
    ROI; else the alpha is the matte. Empty matte (nothing above 0.05) = an error. An unrelated
    varying alpha would be cropped: the `ROI` field ("Crop WxH" / "None (alpha opaque)") and the
    log show it;
  - box = **union over all frames** of pixels > 0.05, grown by **ROI Margin** (default 64, min 32),
    to **≥ 512 px per side** (enlarged around the centre, no resampling), rounded up to **×32**,
    slid inside the image;
  - cropped while staging (model, estimate and staging all see the crop); manifest `width/height`
    = crop, plus `full_width/full_height`, `roi`; job.py pastes each matte into a black full frame;
  - **Black Out Outside ROI** (default off): staged source × its alpha in the source colour space;
    no dilation (dilate upstream). Whether the model likes it: UNVERIFIED (A/B pending);
  - **Multiply Result by ROI** (default off): result × source alpha **at render time**
    (`applyRoiLimit`), toggles without reprocessing;
  - cache key `|roi=x,y,w,h,k<mask>` from the hidden `roiState` written at Process (active, box,
    mask, settings `m<margin>k<mask>`); no ROI = the old key. Settings changed since, or nothing
    processed and the alpha is a matte: `|roi=unprocessed` (NOT PROCESSED). Changed alpha
    *content* isn't detected: press Process again (README);
  - Flame may not show param hints, so labels say what they do. UNVERIFIED: that a matte on the
    node's matte socket arrives in the source alpha.
- **Limits:** Process stages and reprocesses the **whole clip** (~12.5 MB per HD frame staged).

## Preflight and the memory model (decisions 34, 56)

- **56. Warn instead of refusing, record every run, learn locally later.** The estimate comes
  from one 96 GB card, so unseen GPUs may be over- or under-estimated.
  - **Yes/No warning (built):** `sendMessage(eMessageQuestion)`; Yes processes, No cancels; no
    answer → proceed with a footer warning. Lists the estimate, free + LTX worker − reserve, and
    what Flame and other apps hold (NVML per process). A live resident worker's memory always
    counts as available, whatever its precision (a mismatch restarts it).
  - **Record (built):** the manifest carries estimate, free, total, resident, reserve, model size,
    GPU, driver, warned; the worker's `vram` line and job.py's `logs/vram_stats.csv` row are in
    `worker/CLAUDE.md`.
  - **Step 2, local correction (PROPOSED):** one factor per precision = median(observed /
    predicted) over successful runs; inactive below ~10 runs; clamped ~0.8–1.3; falls slowly, rises
    fast after an OOM or near-OOM; in the user config, logged, with a "Reset estimate" action.
  - **Caveat:** a success only bounds the need from above, an OOM from below; use allocated /
    lowest-free numbers (not nvidia-smi).
- **Measurements** (official CLI, RTX PRO 6000 Blackwell 96 GB, fresh process per run incl. model
  load, Gemma loaded and freed in process):

  | Size | Frames | bf16 peak | fp8 peak | Time |
  |---|---|---|---|---|
  | 1920×1088 | 49 | 44.9 GB | 27.5 GB | ~73 s |
  | 1920×1088 | 97 | 51.7 GB | 34.1 GB | ~144 s |
  | 1920×1088 | 145 | **58.8 GB** | **41.3 GB** | ~239 s |
  | 3840×2176 | 49 | 67.8 GB | 54.1 GB | ~425 s |
  | 6144×3264 | 49 / 25 | — | fails | — |

  fp8 saves ~17 GB at no speed cost; HD needs ~0.145 GB and ~1.6 s per frame.
- **Estimate** (`test_vram_estimate`): need = 19.5 GB (fp8) or 36.8 GB (bf16) + 0.0718 GB per
  Mpx·frame of the padded largest chunk + 0.70 GB per Mpx above HD + 1 GB; within 0.4 GB at HD.
- **Resident refit:** with the transformer kept, later chunks run the reference encode on top of
  it (A/B HD 2 × 145 fp8: chunk 1 peak 40.5 GB, chunk 2 52.2 allocated = 18.2 held + 34.0). So
  resident need = max(non-resident, T + 1.56 + 0.1088 GB per Mpx·frame + 0.7 + 1), T = held
  reserved (fp8 21.7, bf16 36.8). Covers 12 cases within 0.4–2.8 GB; in Flame 720p bf16 45.9
  estimated / 44.7 used, HD 180 fp8 resident 57.9 / 57.0.
- **MPS** (`mps` flag, decision 65): need = 44.5 + 0.231 GB per Mpx·frame + 1, fitted to the M1
  runs in `worker/CLAUDE.md`. Free memory on a Mac = min(working set, available RAM), so Auto picks
  Half or Quarter there. Over-committed Metal swaps instead of raising an OOM; the warning says so.
  Resident case on MPS: UNVERIFIED. fp16 (68) uses the bf16 model (equal at 720p; HD unmeasured).
- **Mac precision:** fp8 is refused at Process (MPS has none; 34) but stays in the dropdown (user
  2026-10-13: explains the difference, keeps stored indexes). **68:** `effectivePrecision()` makes
  "bf16" "fp16" if `transformer_fp16=1` (cache key, manifest, warning; the worker restarts).

## macOS platform layer (decisions 64, 65)

`platform_macos.cpp` + `platform_macos_metal.mm`: staging in the per-user temp folder
(`confstr(_CS_DARWIN_USER_TEMP_DIR)`, on disk); `queryVram` = Metal
`recommendedMaxWorkingSetSize` as total, free = min(total, free + inactive pages from
`host_statistics64`), driver = the macOS version; `queryGpuProcesses` empty (no per-process API);
`processName` = `proc_name`; licence in TextEdit (`open -e`). In shared files: `half.h` NEON,
`process.cpp` `pipe`+`FD_CLOEXEC` and `POSIX_SPAWN_CLOEXEC_DEFAULT`, `transport.cpp`
`F_PREALLOCATE` (`F_ALLOCATEALL`) + `ftruncate`, `log.cpp` `pthread_threadid_np` (same id as
Python's `threading.get_native_id`). job.py runs under `ltx_python` (the CLT python3 has no
numpy; Process refuses without it). The plugin should check `xcode-select -p` before launching
setup_helper.py (without the CLT, `/usr/bin/python3` is an install-prompt stub).

## Flame host behaviour (verified on Flame 2027.1 and 2027.2, Rocky Linux 9.7)

- **Every OFX action, render included, runs on Flame's main UI thread** (log tid == pid): never
  run the model in render.
- Flame **renders the current frame as soon as a Batch loads**, and again on every revisit, frame
  change and param change. Fetching frames: ~11 ms per HD float frame, in render and during a
  button press. **32-bit float RGBA**; OFX time starts at 0 (Batch frame 1).
- **Every plugin `setValue` comes straight back as `changedParam`** (reason plugin edit). Reacting
  recursed 10,678 times and crashed Flame. **Rule:** handle only `eChangeUserEdit`, use a
  re-entrancy guard, write labels only when they change (`setIfChanged`).
- Param changes from a **non-action thread** are accepted but applied **lazily, at the next UI
  interaction** (no auto-refresh that way; Status updates appear late).
- **Layout:** params flow into **columns of 5 rows, up to 6 columns per tab**, in definition
  order; **each group starts a column** (long groups repeat the header); **page membership is
  ignored**: a new tab appears only on overflow, named after the next OFX page;
  `kOfxPluginPropParamPageOrder` is unknown, `setPageParamOrder` throws (wrapped); rows per
  control: choice 2, boolean 1, double 1, button 1, file/dir string 2, **plain string 1 with no
  label**, label string 1; text cut at ~30 characters (paths from the left); disabled = greyed.
- `getClipPreferences` → Alpha-only is **ignored** (output stays RGBA: image + matte outputs).
  The node label (`kOfxPropLabel`) can't be changed (the set succeeds, reads return the old one).
- All renders report `interactive=0, sequential=1`: **no final-render signal**.
  `hostIsBackground` (Background Reactor) is unobserved.
- Persistent messages show in the footer; `sendMessage` shows popups; `eMessageQuestion` shows
  Yes/No and returns the answer. Hidden params set in `createInstance` (`initialized`, `nodeId`)
  **are saved with the setup**. `describe` runs once per Flame start; `describeInContext` lazily.
- Batch Render output contains the cached mattes. Killing Flame mid-job: the worker exits (stdin
  EOF / Flame pid gone); stale staging files are swept at the next plugin load.

## Dead ends: don't retry

- A RAM matte cache (lost on Batch reload); C++ OCIO in the shim (superseded by 54).
- `setPageParamOrder`/`kOfxPluginPropParamPageOrder` (threw in `describeInContext`; the plugin
  wouldn't attach); OFX pages as tabs (ignored, three variants; no `kOfxParamPropGroupIsTab` in 1.4);
  renaming the node for progress; `InteractiveRenderStatus` for final renders; Alpha-only output.
- Reacting to plugin-edit `changedParam` (recursion → crash); background-thread param sets to
  refresh the viewer (applied only at the next UI event).
- Preflight counting a resident worker only when its precision matches (refused jobs that fit); a
  flat "+9.4 GB" for a resident transformer (under-estimated HD 145 by 8.6 GB).
