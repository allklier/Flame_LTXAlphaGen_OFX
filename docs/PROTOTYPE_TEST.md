# Prototype test in Flame, round 3

**New in this build:**
- the layout redesigned from the test-2 screenshots: one tab of five columns;
- a token caption;
- a faint "NOT PROCESSED" marker;
- a matte-only output;
- **auto-refresh** when processing finishes (an experiment);
- full logging, with **Collect Logs**.

The worker is still a dummy: its "matte" is the source luminance.

**The log has moved** to `~/.cache/ltx-alphagen/logs/ltx-alphagen.log`, a new format shared by
the plugin, job and helper. The old `proto.log` is no longer written.

## Before you start (on akflame5)

```bash
rm -rf ~/.cache/ltx-alphagen/logs ~/.config/ltx-alphagen/config.conf ~/.cache/ltx-alphagen/mattes
```

Restart Flame; the install symlink is unchanged. Batch with an HD clip, 32-bit float if possible.
**Screenshots of the node's controls**, please, both tabs.

## Tests

| # | Do | Look for |
|---|---|---|
| 1 | Add **LTX › LTX Alpha Gen**. | One tab with 5 columns: Alpha Gen, Performance, Licence, Models, Cache, plus a 2nd tab "Prototype". Any text cut off? Is there a caption "Hugging Face token:" above the token field? |
| 2 | Look at the node's outputs in the schematic. | **Only a matte output** now? (Before: image + matte.) |
| 3 | Connect a clip **without** pressing anything. | Faint diagonal stripes with **NOT PROCESSED** in the middle, **immediately**, no scrub needed. Then untick **Show Marker**: plain black. Tick it again. |
| 4 | Prototype tab: tick **Enforce Gate**. | The marker reads **SETUP REQUIRED**; Alpha Gen and Performance are greyed out. Models column: "Licence: NO \| Models: NO". |
| 5 | Licence: **I Accept**, enter `hf_TESTTOKEN123`, **Download**. | The caption changes to "Token used HH:MM, not stored"; the field is empty. After the progress window finishes, touch any control: "Licence: OK \| Models: OK", controls active. |
| 6 | Alpha Gen: **Process** (≤145-frame clip). | The progress window appears. **Without scrubbing**, does the viewer switch from the marker to the matte when it finishes? Does Status change to "Done: N frames"? **Watch for any Flame instability**: this is the experiment. |
| 7 | A **~300-frame** clip: Process, and watch the viewer and Status. | Status steps through "Processing: 1/3 chunks done" … "Done". The viewer updates by itself; no seams. |
| 8 | Close and reopen the Batch; restart Flame and open it again. | Mattes appear immediately, nothing recomputes, the load is fast. |
| 9 | Prototype: **Sim Seconds = 120**. Cache: **Purge Cache**, then Alpha Gen: **Process**, and press **Cancel** in the window after a few seconds. | The window closes; the marker reads **CANCELLED**; Status "Cancelled". |
| 10 | Sim Seconds = 20. **Simulate = oom**, Process. Then **crash**, Process. | An error window; the marker reads **FAILED**; the footer message names the reason. |
| 11 | Simulate = none, **Sim VRAM GB = 500**, Process. | An error popup right away; no job starts. Set Sim VRAM GB back to 0. |
| 12 | **Simulate = hang**, Process, then **Cancel** in the window. Then run `pgrep -af dummy_worker`. | The window closes; no worker left. |
| 13 | Simulate = none. Process, then render the Batch to a file (**Batch Render**; also **Background Reactor** if you use it). | The output file contains the mattes. |
| 14 | Prototype: untick **Auto Refresh**, Purge Cache, Process. | Same as 6, but the viewer only updates when you scrub (the fallback behaviour). |
| 15 | Models: tick **Debug Logging**, scrub a little, then press **Collect Logs**. | A message with the bundle path in your home folder. |

## What to report back

Short notes per test number, plus screenshots. **Mention any crash or freeze immediately, with
the test number.** The last lines of the log will show what was running.

Clean up afterwards:
`rm -rf ~/.config/ltx-alphagen/config.conf ~/.cache/ltx-alphagen/mattes ~/ltx-alphagen-support-*.tar.gz`
