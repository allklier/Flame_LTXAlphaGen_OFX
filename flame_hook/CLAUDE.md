# flame_hook/ — the Flame Python hook

Part of the design notes (root `CLAUDE.md` has the overview and the decision index).

## Decision 49: auto-refresh when a job finishes (verified in Flame)

`ltx_alphagen_hook.py` ships in the bundle's `Resources/flame_hook/`; `install.sh` copies it to
`/opt/Autodesk/shared/python`. job.py writes `~/.cache/ltx-alphagen/run/refresh/<job>.json`, only
for jobs whose `flame_pid` is a live process named `flame` (so smoke tools and the system test
never step a Flame session). The hook polls that folder every second through
`flame.schedule_idle_event` and, on Flame's UI thread, steps `flame.batch.current_frame` to a
neighbour (back from the last frame, forward from frame 1) and back one second later. Flame
re-renders on each frame change, and the return renders the original frame from the fresh cache.
If scheduling the return fails, it steps back at once. Several jobs ending together give one
refresh. Markers older than an hour are swept (no hook installed). Tested against a fake `flame`
module in `worker/tests/test_hook.py`.

macOS: the hook takes the bundle from `/Library/OFX/Plugins`; `/opt/Autodesk/shared/python` exists
there too (world-writable) and Flame 2027.2 on the Mac loads hooks from it (plugin logs 2026-10-13:
the hook loaded and auto-refresh worked).

## Decision 70: mattes as media on the Batch Renders shelf (discussed, shelved; user, 2026-10-13)

The idea: keep each matte as Flame media (visible in the project, archived with it) instead of
the plugin's disk cache (decision 36), and have the node show that media on its output.
- OFX has no API to write into Flame's library or reels, nor to read a Flame clip. Only the hook
  could import: `flame.import_clips(path, destination)` (`<flame>/python_utilities/examples/
  watch_folder.py:88`), `flame.batch.import_clip(path, reel_name)` / `import_clips(paths,
  reel_name)` (`examples/import_file_using_custom_dialog.py:46,55`); Logik Portal creates batch
  groups with `shelf_reels=['Batch Renders']`. UNVERIFIED: importing onto a shelf reel by name.
- The node could mirror the clip only if Flame soft-imports (references our EXRs, not a copy in
  its own storage); whether that holds, and whether an archive takes unmanaged media, depends on
  project and archive settings (UNVERIFIED). The hook also can't tell which node a job came from.
- **Shelved:** too many open questions about where and how media is saved. To get mattes as media,
  pair the node with a Render or Write File node, which are built for that. No known precedent of
  an OFX node mirroring a shelf clip.

## Flame's Python (verified on Flame 2027.1/2027.2, Rocky Linux 9.7)

- Hook modules load from `/opt/Autodesk/shared/python` (world-writable; other tools install there
  too) and `$DL_PYTHON_HOOK_PATH`; entry points as in
  `<flame>/python/hook.py` (`app_initialized(project_name)`, …). Flame's Python is 3.13.
- `flame.schedule_idle_event(fn, delay=0)` runs `fn` on the UI thread when idle
  (`<flame>/python_utilities/examples/watch_folder.py`). **The C++ signature is
  `(function, int delay=0)`: a float delay raises ArgumentError.**
- `PyOFXNode` methods: add_elbow, attributes, bake_expression, cache_range, change_plugin,
  clear_expression, colour_space, create_marker, delete, duplicate, get_expression, get_metadata,
  input_sockets, load_node_setup, markers, output_sockets, parent, save_node_setup, set_context,
  set_expression, sockets. `node.attributes` = pos_x, pos_y, name, collapsed, note, selected, type
  ("OpenFX"), resolution, schematic_colour, bypass, cache, plugin_name,
  preserve_input_colour_space, shot_name. **OFX params are not reachable from Python**, so the hook
  can't tell which node a job belonged to.
- `flame` has execute_shortcut, execute_command, go_to, schedule_idle_event, …; `flame.batch` has
  current_frame (a PyAttribute, 1-based), nodes, render, go_to, …

## Dead ends: auto-refresh attempts that failed

- Setting params from a background thread in the plugin: applied only at the next UI event.
- In the hook: idling on the UI thread; setting OFX params from Python (not exposed);
  `current_frame.set_value(same frame)` (a no-op); toggling Bypass (hits every LTX node, since the
  hook can't identify the finished one, and leaves them black if the restore fails);
  `schedule_idle_event(fn, delay=0.5)` (float delay → ArgumentError; the undo never ran).
