# helper/ — Setup

Part of the design notes (root `CLAUDE.md` has the overview and the decision index).

## Decisions

- **2/13/20.** Setup handles the downloads. The HF token is used once (passed in the environment,
  never on argv) and never saved. Models are downloaded once per user, to a user-chosen folder.
- **7.** `ltx-core`/`ltx-pipelines` are installed by the user's Setup, never bundled.
- **51.** **Reuse existing downloads:** an optional "Existing Models" folder is searched by exact
  file name; files are verified by **size + SHA-256** against the **pinned manifest shipped with
  the plugin**, then copied; only missing files are downloaded. HF hides SHA-256 for these gated
  repos without auth, so the hashes are captured once per release.
- **66.** **Link instead of copy, asked at Download** (user 2026-10-13: save disk space). With
  Existing Models set, Download asks Yes/No: Yes passes `--link`; Setup hashes each found file **in
  place** (still verified, so it saves space, not time) and puts a **symlink** in the Model Folder
  (not a hard link: Existing Models is often another volume or a share); No, or no answer, copies.
  A popup because the Models column is full (5 rows). A dangling or mismatching destination is
  replaced; a verified link is kept on re-runs. The folder must stay mounted; a network folder
  slows every model load. Tested in `worker/tests/test_setup_models.py`.
- **57.** **No bundled Python packages.** Licences would allow torch etc. (not the LTX code;
  NVIDIA's CUDA wheels have their own terms), but the venv is 6.4 GB (CUDA libs 3.1, torch 1.1,
  triton 0.7): bundling moves those bytes into our release, past GitHub's 2 GB per-file limit, and
  ties the plugin to one torch/CUDA/Python. Later option: an offline install from a package folder.
- **60.** **uv's own Python, pinned (3.14.7), never a system Python.** LTX-2 asks only for ≥ 3.10,
  so uv picked a user's system 3.12, and without `python3.12-devel` Triton's C build failed at the
  prompt cache step (`Python.h: No such file`). Setup runs `uv sync --managed-python --python
  <manifest runtime.python>`; a venv on another Python is recreated. Triton also needs a **C
  compiler** ($CC, else gcc, else clang; `triton/runtime/build.py:26-39`): checked before any
  download; the smoke test checks `Python.h`. (0.1.2, from a user report.)

## `setup_helper.py`

Stdlib only, run by the system python3; launched by Download with `HF_TOKEN` in the env
(optional), `--existing`, `--link`. In order:
1. **Runtime** in `<Model Folder>/runtime/`: uv (sha256), LTX-2 tarball at the pinned commit,
   `uv sync --frozen --no-dev --extra natten`, `opencolorio==2.6.0`, then a torch/CUDA/OCIO smoke
   test.
2. **Models:** keep if verified (`*.verified` marker: sha + size + mtime); copy from Existing
   Models while hashing, or link after hashing (66); or download from HF while hashing. 401/403 →
   "accept the model terms at huggingface.co/<repo>".
3. **Prompt cache** (worker `--build-embeddings`).
4. **fp16 copy (macOS only, decision 68):** probe (`ltx_worker.py --fp16-probe`); at ≥ 1.2× write
   `<Model Folder>/diffusion_models/…-fp16.safetensors` (`--fp16-convert`, ~80 s on NVMe; a real
   file even when the bf16 one is linked; `.verified` tied to the bf16 sha256) and set
   `transformer_fp16=1`, else `0`. Probe or conversion failures only log and leave bf16.
5. **Config** `model_dir`, `ltx_python`, `runtime_commit`, `models_ready`, only on success.

- **Progress for every download:** models and uv show x of y; the LTX-2 source shows MB done
  (GitHub archives have no size); `uv sync` prints nothing off a terminal, so a watcher thread
  shows the uv cache's growth against `uv_cache_bytes` (6.6 GB measured) and catches Cancel.
  Network steps retry twice (PyPI and GitHub assets have timed out). Errors show uv's `error:` +
  last `cause:` line, not its trailing `hint:`.

## `ltx2-uv.lock`, `model_manifest.json`, `licence.txt`

- Lightricks' repo ships **no `uv.lock`**; without one every install would resolve the newest
  versions of that day. So we ship the lockfile resolved at the pinned commit (torch 2.13.0+cu132,
  ltx-core 1.4.2); Setup copies it in and runs `uv sync --frozen`.
- The manifest: uv 0.12.18 (size + sha256), the LTX-2 commit, and per file: repo, revision,
  `path` in the Model Folder, `hf_path` in the repo (the LoRA sits at the Alpha-Gen repo root),
  size, sha256; `runtime.uv_cache_bytes` for the bar. **The SHA-256 values come from local copies
  whose sizes match the HF revisions: UNVERIFIED against HF until the first authenticated
  download.**
- `licence.txt`: the full LTX-2.x licence + a header; shown by View Licence, its hash stored on
  acceptance.

## macOS (decision 65)

The manifest's `uv` has one entry per `sys.platform` (darwin: `uv-aarch64-apple-darwin.tar.gz`);
`runtime.uv_cache_bytes_darwin` drives the bar (1.4 GB). Same `uv sync` (every `natten` entry in
the lockfile carries `sys_platform == 'linux'`; darwin gets torch 2.14.1 from PyPI). The smoke test
asserts `torch.backends.mps.is_available()`. Instead of gcc, Setup requires the Command Line Tools
(`xcrun --find clang++`): no Triton on macOS, but `mps-sdpa` builds its Objective-C++ bridge on
first use (torch `cpp_extension`: clang++, Python.h and **ninja on PATH**, else its pyobjc MPSGraph
fallback, `backends/mpsgraph_zc.py:43-55`), and the progress window is Swift. The lockfile puts
ninja in the venv's `bin/`, but torch looks only on PATH (`torch/utils/cpp_extension.py:2561-2568`)
and nothing adds that `bin/` to the worker's PATH, so the worker probably takes the fallback
(UNVERIFIED; the fix would be in whatever starts the worker).

## Tests

`tools/setup_smoke.sh` (isolated HOME, 11 checks): fresh install with copies from Existing Models
(~10 min); a re-run keeps all files (~30 s); a missing file without a token → clear error; a fake
token → the accept-terms hint, and the token isn't in the log; a same-size wrong-content file is
rejected. `worker/tests/test_setup_models.py`: link, copy, other version, dangling link, fp16.

## Dead ends

- Assuming Lightricks' repo has a `uv.lock`: it doesn't (setup failed with `--frozen`); ship ours.
