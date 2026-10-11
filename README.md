# Flame_LTXAlphaGen_OFX

OFX plugin for Autodesk Flame to run LTX 2.5 Alpha Gen models

Generate an alpha matte from an RGB clip, directly in Flame, using Lightricks'
**LTX-2.5 Alpha Gen** IC-LoRA. It runs locally on your own NVIDIA GPU. There is no cloud service
and no ComfyUI.

> **Status: v0.1 (first release).** The plugin works in Flame and has produced good mattes on
> real footage. Setup is guided, and the viewer refreshes by itself when a job finishes. See
> [Status and roadmap](#status-and-roadmap) for what is not built yet.

## Origins

Originally written by Jan Klier and contributed to the Logik community of Autodesk Flame

Inspired by recent conversations in the community (including IBC 2026 in Amsterdam), to build
tools collectively and with best practices of software engineering and version management.

Everyone in the community is encouraged to enhance this plugin further or fix issues, 
following the guidelines at https://github.com/flamelogik

## Requirements

- Autodesk Flame on **Linux** (developed and tested on Rocky Linux 9.7 with Flame 2027.1). MacOS is not supported at this time, but may be coming.
- An **NVIDIA GPU with a lot of VRAM**. Measured peak usage with the official LTX pipeline:

  | Size | Frames | bf16 | fp8 |
  |---|---|---|---|
  | 1920×1088 | 49 | 44.9 GB | 27.5 GB |
  | 1920×1088 | 97 | 51.7 GB | 34.1 GB |
  | 1920×1088 | 145 | 58.8 GB | 41.3 GB |
  | 3840×2176 | 49 | 67.8 GB | 54.1 GB |

  fp8 saves about 17 GB with no speed cost. Expect roughly 1.6 s per HD frame. Keeping the model
  loaded between jobs (Unload GPU off) needs more: about 57 GB for HD at 145 frames in fp8.
  6K is currently not supported. A 24 GB card is too small; on a 48 GB card use fp8.
- About **80 GB of disk** in the Model Folder (73 GB of model files plus the ~7 GB LTX runtime),
  plus space for the matte cache (about 4 MB per HD frame).
- Internet access during Setup (Hugging Face, GitHub, PyPI). After Setup, processing runs offline.
- Flame's own NVIDIA driver is sufficient (verified with driver 580.159.04, torch 2.13, CUDA 13.2).
- A free [Hugging Face](https://huggingface.co) account, if you need to download the model files.

## How it works

Flame never waits on the model. The plugin is split into two parts:

1. A light **C++ OFX plugin** inside Flame. It shows the controls, looks up mattes in a disk
   cache, and displays them.
2. A **Python background process** on the same machine that loads the model and does the
   inference.

Processing never starts by itself. Frames without a matte show a faint marker (for example
`NOT PROCESSED`). Press **Process** and the plugin stages the clip and starts a separate job with
its own progress window and a Cancel button. When the job finishes, the viewer refreshes by
itself and shows the matte. Mattes are cached on disk per frame, so revisiting, re-rendering and
Batch Render are instant.

- The matte is delivered in the **alpha / matte output** of the node. The **image output** shows
  the source with the matte overlaid in magenta (30%), so you can judge the matte against the
  picture at a glance.
- Clips are processed in chunks of 145 frames with a crossfade across the overlap. Padding to the
  model's size and frame rules is automatic, and the output always matches the source size and
  frame count.
- The model sees display-referred images. Camera log and ACEScg footage is converted for the
  model with OCIO (see **Input Colour**); your source and output are not changed.
- Mattes are cached per source frame and settings. Changing Input Colour or Precision gives a
  new matte (the marker shows again until you process), and switching back finds the earlier one.

## Installation

Installation copies the plugin into Flame's OFX folder and adds a small Flame Python hook that
refreshes the viewer when a job finishes. The model, the LTX code and the Python runtime are
installed later, per user, from inside Flame (see [Setup](#setup)).

### From a release (recommended)

1. Download `LTXAlphaGen-<version>-rocky9.tar.gz` from the GitHub releases page and unpack it.
2. Quit Flame.
3. Run the installer with sudo (it writes to `/usr/OFX/Plugins`):

   ```
   tar xzf LTXAlphaGen-0.1.0-rocky9.tar.gz
   cd LTXAlphaGen-0.1.0
   sudo ./install.sh
   ```

   It installs:
   - `/usr/OFX/Plugins/LTXAlphaGen.ofx.bundle` (the plugin; an earlier version is replaced);
   - `/opt/Autodesk/shared/python/ltx_alphagen_hook.py` (the refresh hook). It will not replace
     a different file of that name.
4. Start Flame. In Batch, the node is under **LTX > LTX Alpha Gen**.

To uninstall, run `sudo ./install.sh --uninstall`, or delete the two items above. Your models,
settings and cache stay where Setup put them (see [Files and folders](#files-and-folders)).

### Building from source

You need Rocky Linux 9 (the plugin must link against glibc 2.34 or older and Flame's GCC 11
`libstdc++`), CMake and the system GCC 11:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLTXAG_DEV_BUILD=OFF
cmake --build build -j 8
cd build && ctest
```

The bundle is `build/LTXAlphaGen.ofx.bundle`. Copy or link it into `/usr/OFX/Plugins`, and copy
`flame_hook/ltx_alphagen_hook.py` into `/opt/Autodesk/shared/python`. A build with
`-DLTXAG_DEV_BUILD=ON` (the default) adds a Prototype page with test controls and lets you use
the node before Setup is complete.

## Setup

Setup runs once per user, from the node's **Setup** tab. Until it has finished, the node shows
`SETUP REQUIRED` and Process is disabled.

### 1. Accept the licence

In the **Licence** column:

1. Press **View Licence** and read the LTX-2.x Community License.
2. Tick **I Accept**. This is stored per user. If a later version of the plugin ships a different
   licence text, you will be asked again.

### 2. Get access to the models on Hugging Face

The weights are in two gated Hugging Face repos. With your Hugging Face account, open both pages
and accept the terms:

- https://huggingface.co/Lightricks/LTX-2.5
- https://huggingface.co/Lightricks/LTX-2.5-22b-IC-LoRA-Alpha-Gen

Then create an access token (Settings > Access Tokens, type "Read"). You can skip this step if
all six model files are already on your system (see Existing Models below).

### 3. Choose the folders

In the **Models** column:

- **Model Folder**: where the models and the LTX runtime are installed (about 80 GB). Pick a fast
  local disk with enough space. Default: `~/ltx-alphagen-models`; on many Flame systems a data
  volume is a better choice than the home folder.
- **Existing Models** (optional): a folder that may already hold the model files, for example a
  ComfyUI models folder. Setup searches it by exact file name, checks each file's size and
  SHA-256 checksum against the list shipped with the plugin, and **copies** the matching files
  into the Model Folder instead of downloading them. Files that don't match are ignored.

### 4. Download

Paste your token into **HF Token** and press **Download**. The token is passed to the download
once and never saved (the caption then reads "Token used HH:MM, not stored"). If everything is
found under Existing Models, you can leave the token empty.

A progress window shows each step, with Cancel:

1. **Runtime** (in `<Model Folder>/runtime`): the `uv` package manager, the Lightricks LTX-2
   code at a pinned commit, and its Python packages (about 6.6 GB, shown as "x of ~6.6 GB"),
   plus OpenColorIO. The package versions are pinned by a lock file shipped with the plugin, so
   every install is the same environment. Takes a few minutes.
2. **Models**: for each of the six files, either keep it (already in the Model Folder and
   checked before), copy it from Existing Models, or download it from Hugging Face. Every file is
   checked with its SHA-256 checksum while it is copied or downloaded. Downloads show "x of y GB"
   and speed.
3. **Prompt cache**: a one-time step that runs the Gemma text encoder once, so it never has to be
   loaded during processing.
4. **Settings**: the Model Folder and runtime are recorded, and the node becomes ready.

Running Download again is safe and quick: files already checked are kept. Network steps retry by
themselves. If Hugging Face refuses the download (HTTP 401/403), the message tells you which
repo's terms you still need to accept.

## Using it

1. Add **LTX Alpha Gen** after your source in Batch.
2. Set **Input Colour** to match the source (see below).
3. Press **Process**.
4. Watch the progress window, for example
   `Chunk 2/3 · frames 121–265 of 300 · step 5/8 · 3:12 elapsed · about 2:40 left`.
   The elapsed time ticks every second; the time left appears after the first two steps.
   **Cancel** stops the job and frees the GPU.
5. When the job finishes, the viewer steps one frame and back to refresh, and shows the matte.

While a job runs, the frames show `PROCESSING`. You can keep working in Flame, but avoid other
GPU-heavy work while processing.

**Before a job starts**, the plugin estimates the GPU memory the clip needs from its size and
frame count. If that is more than is free (minus the VRAM Reserve), it asks whether to continue,
and lists what Flame and other applications hold on the GPU. The estimate errs on the safe side.
If you continue and the GPU really runs out, the job stops with an out-of-memory error; Flame is
not affected.

## Controls

The node has two tabs: the first holds **Alpha Gen**, **Performance** and **Cache**; the
**Setup** tab holds **Licence**, **Models** and **Support**. Settings marked *per user* are
shared by all your nodes and projects; the others are saved with each node.

### Alpha Gen

- **Input Colour**: the colour space of the source. The model needs display-referred images, so
  the source is converted to Rec.1886 through Flame's ACES 2.0 config, view
  "ACES 2.0 - SDR 100 nits (Rec.709)", before inference. Only the model's input is converted.
  - `Bypass`: the source is already display-referred (Rec.709 / Rec.1886). No conversion.
  - `ACEScg`, `LogC4` (ARRI LogC4), `Log3G10` (RED Log3G10 / REDWideGamutRGB),
    `LogC3` (ARRI LogC3, EI 800), `BMD Film Gen5` (Blackmagic Film Gen5 / Wide Gamut Gen5).

  A wrong setting gives a noticeably worse matte. Each setting has its own cached mattes.
- **Process**: generate mattes for the whole clip in the background. It always processes the
  whole clip connected to the node.
- **Status**: the state of this node's last job (processing, done, failed, cancelled).
- **Show Marker**: on frames without a matte, the matte output shows faint diagonal stripes with
  a caption: `NOT PROCESSED`, `PROCESSING`, `FAILED`, `CANCELLED`, `SETUP REQUIRED`,
  `NOT CONNECTED` or `NOT SUPPORTED`. Off: such frames are plain black. Frames with a matte are
  not affected, and the image output never shows the marker.

### Performance

- **Precision**: `bf16` (reference quality) or `fp8` (about 17 GB less GPU memory, same speed).
  Each precision has its own cached mattes. The built-in default is bf16 if an HD 145-frame
  chunk fits in the free VRAM, otherwise fp8. Nothing is ever downgraded silently.
- **Unload GPU**: on (default): the model is unloaded when this node's job ends, and all GPU
  memory goes back to Flame. Off: the model stays loaded, so the next job skips loading it
  (measured: a small job took 11 s instead of 22 s) and each chunk saves ~6 s of setup. It costs
  more GPU memory and holds it between jobs. A loaded model unloads itself after 5 minutes without jobs, and when Flame
  quits. With several Alpha Gen nodes, turn Unload GPU off on all but the last node you process.
- **VRAM Reserve**: GPU memory (GB) kept free for Flame while processing. The default is 5% of
  the VRAM that was free when the plugin loaded.
- **Save Default** (*per user*): makes this node's Precision, Unload GPU and VRAM Reserve the
  defaults for every new node. Order of precedence: built-in default, then your saved default,
  then the node's own setting.

### Cache

- **Cache Folder** (*per user*): where mattes are stored. Default `~/.cache/ltx-alphagen/mattes`.
- **Cache Size**: how much the cache holds.
- **Purge Cache**: delete the cached mattes of this node's clip for its current Input Colour
  and Precision (mattes made with other settings stay).
- **Purge All**: delete every cached matte of every node and project.
- Old mattes are also swept automatically when Flame starts: unused for 30 days, or least
  recently used first when the cache passes 50 GB.

### Licence (Setup tab)

- **View Licence**, **I Accept** (*per user*), **HF Token** and **Download**: see
  [Setup](#setup).

### Models (Setup tab)

- **Model Folder** (*per user*), **Existing Models** (*per user*): see [Setup](#setup).
- **Setup**: whether the setup is complete.

### Support (Setup tab)

- **Debug Logging** (*per user*): more detail in the log, including every render. Turn it on when
  you are chasing a problem.
- **Collect Logs**: writes `~/ltx-alphagen-support-<date>.tar.gz` with the logs, a system
  summary (OS, GPU, driver, memory, plugin version) and the job records. Review it before you
  send it: it contains file paths, but no images, no clip or project names, and no token.

## Files and folders

| What | Where |
|---|---|
| Plugin | `/usr/OFX/Plugins/LTXAlphaGen.ofx.bundle` |
| Refresh hook | `/opt/Autodesk/shared/python/ltx_alphagen_hook.py` |
| Models and LTX runtime | your Model Folder (`runtime/` holds the code and Python packages) |
| Settings (per user) | `~/.config/ltx-alphagen/config.conf` |
| Matte cache | your Cache Folder (default `~/.cache/ltx-alphagen/mattes`) |
| Logs | `~/.cache/ltx-alphagen/logs/` (`ltx-alphagen.log`, `vram_stats.csv`) |
| Frames during a job | `/dev/shm` (removed when the job ends) |

## Licensing

- **This plugin** is licensed under the **Mozilla Public License 2.0** (see [LICENSE](LICENSE)).
  The vendored OpenFX headers in `third_party/openfx` keep their own BSD licence.
- **LTX code and weights are not part of this repository** and are never bundled. Setup
  downloads them from Lightricks' own repositories. They are under the **LTX-2.x Community
  License**, which you accept on the Setup tab, and you are responsible for your own compliance
  with it.
- In particular, **entities with USD 10M or more in annual revenue need a paid licence from
  Lightricks.**
- Output is AI-generated content. Check the LTX licence for disclosure requirements.
- The Python packages Setup installs (PyTorch, NVIDIA CUDA libraries, OpenColorIO and others)
  come from their public sources under their own licences.

This is not legal advice, and the licensing notes will be reviewed before a public release.

## Status and roadmap

**Works today:** the plugin in Flame with the real Alpha Gen worker; guided Setup (runtime
install, Hugging Face download, reuse of existing model files, checksum verification); input
colour conversion; bf16 and fp8; a resident model with Unload GPU; a clip-size-aware memory check
with a warning; a progress window with time left; automatic viewer refresh; the disk matte cache;
chunking for long clips; logging and Collect Logs.

**Not yet built:**

- 6K and larger frames (the decoder refuses them).
- Disk staging for very long clips. The whole clip is staged in `/dev/shm` (about 12.5 MB per HD
  frame), which limits very long or very large clips.
- Processing only the frames that are not cached yet. Process always processes the whole clip.
- Background Reactor rendering is untested (Batch Render works).
- An offline install of the Python packages for workstations without internet.

## Support and logs

Everything is logged to `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`. The HF token, image data
and clip or project names are never logged. Use **Collect Logs** to create a bundle for a bug
report.

Each job also adds a line to `~/.cache/ltx-alphagen/logs/vram_stats.csv` with the estimated and
the actual GPU memory use. Sending it with a bug report helps improve the estimate for GPUs we
haven't measured.

## Contributing

Issues and pull requests are welcome. Please follow the community guidelines at
https://github.com/flamelogik.

