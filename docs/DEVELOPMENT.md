# Development notes

## Layout

| Path | |
| --- | --- |
| `src/main.cpp`, `src/pg_app.h` | The host application: paths, launcher flow, menus, default renderer settings. |
| `src/pg_ui.*`, `src/pg_folder_picker.cpp` | Launcher, Insert menu, FPS counter, settings file, game folder check. |
| `src/pg_frame_pacing.*` | Frame rate limit and the fixes that keep the game's clocks right when uncapped. |
| `src/pg_buttons.cpp`, `tools/gen_ps_buttons.py` | PlayStation button icons: our art, patched into the game's button font when it is decompressed. |
| `src/pg_input_script.*` | Controller input recording and replay for repeatable test routes. |
| `src/pg_sampler.*` | In-process sampling CPU profiler (Windows). |
| `src/pg_timeline_hooks.cpp` | Timed wrappers around game functions for `--timeline_trace`. |
| `pg_config.toml` | Code generator manifest: the entry point and function hints. |
| `patches/rexglue-sdk.patch` | Changes to ReXGlue v0.10.0 (Vulkan performance, vblank control, counters). |
| `tools/dev/` | Measurement scripts. |

Recompiled functions are weak symbols that call `__imp__sub_XXXXXXXX`, the generated body.
Defining `sub_XXXXXXXX` in the host replaces a game function; calling `__imp__sub_XXXXXXXX`
from it runs the original. All game hooks in `src/` work this way.

## Frame pacing

On the console the game is paced by vblanks (60 Hz) in three places:

1. The main loop (`sub_8247E318`) waits a presentation interval of vblanks, read from a
   script integer through `sub_823608C0`.
2. The swap (`sub_824853A8`) waits for the GPU to flip the previous frame (a flip counter
   incremented by the swap-done callback `sub_8234BD48`), then for the vblank count to reach
   the previous target plus the presentation interval. Some scenes set that interval to 2
   (30 FPS) from inside the swap.
3. The engine's vblank callback (`sub_8234BD00`) advances a counter that is also a clock: the
   cutscene cameras (`sub_82432538`) measure time as vblanks / 60.

Without vsync the SDK delivers a vblank every millisecond, which the game's Direct3D needs to
retire swaps quickly; at a real 60 Hz the game drops to 30 FPS every few seconds. But at
1000 Hz the cutscenes run many times too fast. `src/pg_frame_pacing.cpp` reconciles both:

- the main loop interval and the swap interval are forced to 0 when uncapped;
- the engine's vblank counter advances at a real 60 Hz, in step with the main loop's frames
  (see "Cutscene camera clock" below);
- inside the swap, the vblank-count getter `sub_8234BE28` returns a private count that
  advances on every read, so the interval-2 waits end at once, while the cutscene cameras
  still read the real count;
- the Havok step: each frame `sub_82418560` hands the physics thread (`sub_8268D868`) a step
  of game speed x 1/30 s through the setters `sub_8268D3B0` and `sub_8268D3A8`. That is real
  time at the console's 30 FPS, but above it ragdolls fell too fast (twice the speed at 60
  FPS). The hooks scale the step by the engine's frame time (the float at `0x82A1D0B4`).

`frame_rate_limit` then holds the game at a chosen rate with a high-resolution timer after
each swap. 30 restores the original vsync pacing. `--uncapped_real_time=false` disables the
fixes.

Note that the `vsync` cvar (set by `ApplyFrameRateLimit`: on only at 30) is the SDK's guest
vblank timer, 60 Hz or 1 kHz. It has nothing to do with how the host presents.

### Presenting in step with the display (`present_sync`)

The presenters used to present without vsync only (Vulkan IMMEDIATE, D3D12
`Present(0, ALLOW_TEARING)`), so even limits that divide the refresh rate tore and beat against
the scan-out (a 60 FPS timer on a 59.94 Hz panel slips a frame every 17 s). `present_sync`
(`auto` by default, `on`, `off`; launcher and Insert menu: Vsync) chooses per frame:

- `auto`: synced when `frame_rate_limit` equals or divides the display's refresh rate within 1%,
  at most 4 vblanks per frame (30/40/60/120 at 120 Hz, 30/60 at 60 Hz); otherwise, and with the
  limit at 0, unsynced as before, for VRR. `on`: always synced (a limit that doesn't divide the
  refresh rate gets plain vsync and keeps the timer). `off`: never.
- The refresh rate comes from the monitor the window is on: `EnumDisplaySettingsW` refined by
  the closest DXGI mode on Windows (59.94, not 59), the SDL display mode elsewhere. It is
  re-read when the window changes monitor and on SDL display mode events, and logged
  (`Presenter: display refresh rate ...`).
- D3D12: the swap chain is created with `FRAME_LATENCY_WAITABLE_OBJECT` (and tearing where
  supported) and is never recreated for this. Synced, `Present(refresh / limit, 0)` with a
  maximum frame latency of 1, waiting on the waitable object before painting; unsynced as
  before, with a latency of 3 (the old default).
- Vulkan: the present mode is fixed at swapchain creation, so a change (the setting, the limit
  or the display) makes `PaintAndPresentImpl` report the connection as outdated, and the UI
  thread recreates the swapchain on the existing reconnect path. Synced uses FIFO, painted from
  the GPU thread (not the UI thread, as FIFO normally is) so the present's wait holds the GPU
  thread and, through the swap, the game. FIFO can't show a frame for several vblanks, so with
  `VK_KHR_present_wait` the presenter waits for the previous frame to reach the screen (one frame
  queued at most), takes that as the vblank, and queues the next frame just after the vblank
  before the one it's due on. Without present wait, FIFO paces 1 vblank per frame by itself, and
  for more the game's timer runs at whole refreshes of the measured rate.
- FSR 3 frame generation keeps choosing its own present mode (FIFO with
  `fsr3_frame_generation_vsync`); `present_sync` doesn't apply while it is on.
- The presenter reports how it paces (`rex::ui::present_sync::GetPacing`) after each present
  from the GPU thread. When it holds frames on the vblanks, `LimitFrameRate` steps aside instead
  of waiting a second time; the report lapses after 100 ms without presents (minimized), or
  while the UI thread paints (Insert menu), and the timer takes over.

### Cutscene camera clock

The cutscene cameras only see whole vblanks, so their poses change at most 60 times a second.
Uncapped, `sub_8234BD00` runs the original callback only when `EngineVblankClock` says a step is
due. The steps are counted when the main loop hands over a frame (`sub_824852F8`): the 1/60 s
elapsed since the last frame, rounded to whole steps. The part not given is carried to the next
frame, and that carried part is pulled slowly toward zero, so the steps stay in phase with the
frames even after the limiter restarts its schedule. The vblank interrupt still delivers the
steps, within a millisecond, so the callback keeps its thread and arguments. While the main
loop hands over no frames for 100 ms (loading screens), the interrupt advances the clock at the
real 1/60 s boundaries itself.

- At 60 FPS every frame sees exactly one step. Before, the steps fell at real 1/60 s boundaries
  with no fixed phase to the frames, and frames near a boundary saw 0 or 2 steps (judder).
- Above 60 FPS the steps are as evenly spaced as whole steps allow: at 120 FPS one every other
  frame, and at 144 FPS gaps of 2 and 3 frames. Camera pans are still only 60 poses a second.
- Cutscene speed stays within a few hundredths of a percent of real time (the pull trades that
  much for a steady phase at some frame rates).
- `--cutscene_clock_follows_frames=false` (hot reload) restores the free-running real-time
  steps, to compare.
- Capped (30 FPS, vsync) the hooks are not involved: every vblank runs the original callback.

Fully smooth cameras above 60 FPS need fractional time in the camera itself. That needs the
generated code (and the xex), which the fix above was made without:

1. Find the counter. In `generated/`, open the body of `sub_8234BD00` and note the guest address
   of the word it increments (a `lis`/`addi` pair or a constant in the `PPC_STORE_U32`). Check
   that `sub_8234BE28` returns the same word.
2. Find how `sub_82432538` reads it: a `bl sub_8234BE28`, or a load of that address. Follow the
   value to the instruction that turns it into time: an integer-to-float conversion (`fcfid`,
   often after a store and reload through the stack) followed by a multiply by 1/60 or a divide
   by 60. Search the other callers of `sub_8234BE28` and the other loads of the address as well,
   to know which other systems use the counter. Leave those on whole vblanks.
3. Give the camera fractional vblanks there with a `[[midasm_hook]]` in `pg_config.toml`, at the
   address of the conversion with `after_instruction = true` and `registers = ["fN"]` for the
   register that holds the converted count. The generated code calls the named function with the
   registers by reference (`PPCRegister&`), so declare it in a header listed in
   `[entrypoint] includes` and define it in `src/`. While `Uncapped()`, replace the value with the
   integer count plus the fraction of a step elapsed since the last step, from
   `EngineVblankClock` (it knows when the last step was given and how much time is pending). Keep
   the fraction below 1, so the camera never reads ahead of the next whole count. Regenerate
   (`build.ps1` runs codegen when the config changes).
4. If the camera does integer math on the count (segment lookups, `divw`, comparisons) before
   any conversion, hook `sub_82432538` itself instead: run `__imp__sub_82432538` twice, with the
   counter word set to the current count and to the next one, and interpolate the camera pose
   it writes (find the output in the generated body: the object passed in `r3`, or a global
   camera) by the elapsed fraction, then restore the counter word. Do this only when the second
   run has no side effects (segment "ready" logging, triggers), otherwise interpolate between the
   pose of the last step and that of the current one, one step behind.
5. Test: a slow pan in a cutscene at 144 FPS should look as smooth as gameplay, a cutscene's
   length (stopwatch, or the audio staying in sync) should not change, and at 30 FPS nothing
   should change at all.

## Settings and flags

Every setting is a cvar: it can go in `pg.toml` next to the executable or on the command line
(`--name=value`). The launcher and the Insert menu write `game_data_root`,
`renderer`, `render_resolution`, `upscaler`, `d3d12_fsr_version`, `sharpness`, `fullscreen`, `frame_rate_limit`,
`present_sync`, `swap_post_effect`, `button_prompts`, `show_fps` and `show_launcher`, and keep
any other lines in the file.

| Flag | |
| --- | --- |
| `--launcher` | Show the launcher even if it is turned off. |
| `--developer` | Keep the SDK overlays: F3 debug, F4 settings, ` console, F7 achievements. |
| `--game_data_root=<dir>` | Start this game folder directly, skipping the launcher. |
| `--user_data_root`, `--cache_root` | Save and cache folders. |
| `--perf_log_csv=<file>` | Per-frame counters (RelWithDebInfo builds). |
| `--tracy` | Start the Tracy client (RelWithDebInfo builds; viewer 0.13.1). |
| `--sample_profile=<file>` | Sampling CPU profile of all threads (Windows). |
| `--timeline_trace=<file>` | CSV of game-thread and GPU-thread wait events. |
| `--input_record=<file>`, `--input_replay=<file>` | Record or replay controller input. |

## Measuring performance

Build `RelWithDebInfo` for the counters. Compare runs on the same route, resolution and
settings, with warm shader caches.

### Routes

`tools/dev/route.ps1` records the controller input the game sees and replays it, so each
run follows the same path from boot:

```powershell
tools\dev\route.ps1 record  park   # snapshot your save, then play; F9 marks the window
tools\dev\route.ps1 replay  park   # replay from the snapshot, then you take over
tools\dev\route.ps1 measure park   # replay, close at measure_stop, summarize the FPS
```

Routes live in `.local/routes/<name>/` (`route.txt` and the `start-save` snapshot). The file
is text, one line per change of state:

```
# pg-input v1: guest ms since first poll; pad buttons(hex) lt rt lx ly rx ry
0 pad 0000 0 0 0 0 0 0
9915 pad 1000 0 0 0 0 0 -900
51230 measure_start
98410 measure_stop
```

Times are guest milliseconds since the game first polled input, so host startup time does
not shift the route. A route only replays correctly with the same frame pacing it was
recorded with: menus and loading take different times at 30 FPS and uncapped.

### Unattended profiling

`tools/dev/profile_route.ps1 -Route <name>` replays a route in a small window without taking
focus, closes the game at `measure_stop` and writes `out/traces/<route>-<time>/` with
`frames.csv`, `game.log` and `report.txt`, and adds a line to `out/traces/runs.tsv`.

- `-NoVsync` runs uncapped (for routes recorded uncapped).
- `-NoTracy` measures with counters only, for A/B runs. Without it the run is captured with
  Tracy 0.13.1's `tracy-capture.exe` from `tools/windows-0.13.1` (not included) and
  analyzed by `analyze_trace.py`.
- `-Sample` and `-Timeline` add the sampling profile and the wait timeline.
- `-GameArgs '--flag=value'` passes options to the game.

`summarize_fps.py <frames.csv>` reports effective FPS, frame time percentiles, the load of
the slowest frames and the GPU thread's work by draw count. Run-to-run noise on heavy frames is
about ±0.4 ms; for smaller changes use an in-run A/B: `--perf_ab_cvar=<bool cvar>
--perf_ab_frames=30` flips the cvar during the run and records `ab_state` in `frames.csv`.

### What limits the frame rate

At 1440p the heaviest scenes (1500+ draws) take about 9 ms per frame, split between the GPU
(main scene 3.4 ms, full-screen passes 1.3 ms, bloom 0.6 ms, EDRAM transfers about 1 ms) and
the GPU command thread that translates Xenos commands to Vulkan. `--vulkan_gpu_timing` logs
the GPU time per frame every five seconds. Changes already in the SDK patch:

- `execute_unclipped_draw_vs_on_cpu` (on by default here): render target copies cover only
  what was drawn; about 2.7 ms of GPU time at 1440p.
- `vulkan_async_submission`: records and submits command buffers on another thread.
- `vblank_after_swap`: the next vblank comes as soon as a swap is processed.
- Cached texture lookups, fewer redundant shared memory comparisons and descriptor reuse in
  the Vulkan command processor.
