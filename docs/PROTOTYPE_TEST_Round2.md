# Prototype test in Flame, round 2

**Goal:** check that the redesigned flow works in Flame. Round 1 showed that rendering inline
freezes Flame's UI, so now:
- render only reads a disk cache;
- **Process** hands the work to a background job with its own progress window and Cancel button;
- the UI is split into pages;
- a licence/model gate is in place.

The worker is still a dummy: its "matte" is the source luminance, with no GPU or model.
Everything is logged to `~/.cache/ltx-alphagen/proto.log` on akflame5, so you only need to note what you **see**.

## Before you start (on akflame5)

```bash
rm -f ~/.cache/ltx-alphagen/proto.log ~/.config/ltx-alphagen/config.conf
```

The symlink from round 1 already points to the new build, so **restarting Flame** is enough. If
the node still shows round-1 controls (no pages, a "Model" page), Flame is caching the old plugin
description. Note that.

Use a Batch with an HD clip, 32-bit float if possible. **A screenshot of each page** would help a
lot with the layout.

## Tests

| # | Do | Look for |
|---|---|---|
| 1 | Add **LTX › LTX Alpha Gen**. Open each page. | Is there a page dropdown or tabs: Alpha Gen, Performance, Setup, Prototype? Any text cut off? |
| 2 | Prototype page: tick **Enforce Gate**. | Alpha Gen and Performance controls greyed out? The viewer shows black, and somewhere a "Setup required" message. **Where** does that message appear? |
| 3 | Setup: press **View Licence**. | A text window opens; Flame stays usable. |
| 4 | Setup: tick **I Accept**, type `hf_TESTTOKEN123` as the token, press **Download**. | A progress window (~15 s); Flame stays usable; the token field clears. When it's done, touch any control: does Setup say "models ready" and do the controls become active? |
| 5 | Save the Batch, then run `grep -rl TESTTOKEN ~/flame /opt/Autodesk/project 2>/dev/null`. | **Nothing found.** |
| 6 | Connect a **≤145-frame** clip and view it. | Black output and a **"Not processed"** message. Nothing starts by itself. |
| 7 | Alpha Gen: press **Process**. | A brief freeze (about 1.5 s for 145 frames), then a **progress window with Cancel**, and Flame **stays usable** meanwhile. Does the node name change to "(processing)"? When the window closes, scrub: do the mattes (grey luminance) appear? Does the node name change back? |
| 8 | A **~300-frame** clip: press Process. | A longer progress run (3 chunks). Afterwards the whole clip has mattes, with no visible seams. |
| 9 | Close and reopen the Batch, then restart Flame and open it again. | Mattes appear **immediately**, with nothing recomputed, and Batch loads are fast. |
| 10 | Prototype: **Sim Seconds = 120**. Alpha Gen: **Purge Cache**, then **Process**, and press **Cancel** in the progress window after a few seconds. | The window closes quickly; frames say "cancelled". |
| 11 | Sim Seconds back to 20. **Simulate = oom**, then Process. Repeat with **crash**. Then Simulate = none and **Sim VRAM GB = 500**, then Process. | oom and crash: an error window, plus a "Processing failed" message on the node. 500 GB: an error popup right away, with no job started. |
| 12 | **Simulate = hang**, Process, then **Cancel** in the window. Afterwards run `pgrep -af dummy_worker`. | The window closes; no worker left. |
| 13 | Simulate = none. **Purge Cache**, then Setup › **Purge All**. | Frames go back to "Not processed"; Cache Size drops to 0. |
| 14 | Performance: Precision fp8, VRAM Reserve 3, untick Unload GPU, press **Save Default**. Add a new node. Restart Flame and add another. | Both new nodes show fp8 / 3 / unticked. |
| 15 | Process a clip, then render the Batch to a file (**Batch Render**, and Background Reactor if you use it). | The output contains the mattes. (The log shows how Flame flags final renders.) |

Clean up afterwards:
`rm -f ~/.config/ltx-alphagen/config.conf; rm -rf ~/.cache/ltx-alphagen/mattes`

## What to report back

Short notes per test number plus screenshots of the pages. Claude reads the log for the rest.

- At beginning output is pure black, don't see any indication of 'not processed', not processed shows up upon scrubbing in footer status
- Gate works, popup box shows with error message
- Field for HF token is unlabled, there's no guidance that it has to be entered.
- HF token disappears afterwards (should be replaced with something like 'known'/'cached', etc.)
- Test download worked
- Processing worked, and status updated where visible.
- Had to scrub for matte to appear, would be nice if current frame could update without scrubbing. More intuitive
- Everything goes to black and 0 used upon purge
- Switched to fp8, set default, new node. All worked. OFX has both image and matte output, both of which show same matte. Can we skip picture?



