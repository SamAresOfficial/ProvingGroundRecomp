# A native renderer: what it would take

Today the game's own Direct3D (linked into `default.xex` from the Xbox 360 SDK) builds Xenos
GPU command buffers, and the rexglue SDK emulates the GPU: it parses those commands, tracks
guest memory for changes, emulates EDRAM and translates the shaders to Vulkan as they appear.
On the phone that emulation is the most expensive part of a heavy frame (see PORTING.md,
"Low frame rate": the GPU command thread spends 24 to 30 ms per frame with 1500+ draws). This
note sizes the alternative: drawing with Vulkan directly. Nothing here is implemented.

## The game's Direct3D, mapped from `generated/`

Found from the kernel's graphics imports (`Vd*`: `VdSwap`, `VdInitializeRingBuffer`,
`VdSetGraphicsInterruptCallback`, ...) and from functions that build PM4 packet headers:

| | |
| --- | --- |
| Direct3D and other graphics libraries | 0x82560000 to 0x82594000: 992 functions, about 125,000 lines of generated C++ (part of it is other SDK libraries in the same range, such as file I/O) |
| Entry points the game calls | 256 functions |
| Game functions that call them | 459, mostly in 0x824xxxxx (195), 0x827xxxxx (101) and 0x825xxxxx (93): the engine's renderer |
| After it | 0x82594000 to 0x825C0000: Xam, profile and session code, not graphics |
| Before it | 0x8255xxxx: the C runtime (`memcpy`, `memset`, register save/restore) |
| Shaders | about 300 vertex and 260 pixel shaders in one session of play; about 580 pipelines |

The entry points are unnamed (`sub_XXXXXXXX`); naming them is the first job, by matching the
Xbox 360 SDK's Direct3D (calling patterns, PM4 opcodes each one emits, device structure
offsets). REA can help with that part: decompiling, call graphs, comparing with runtime traces.

## How Skate 3 Recomp did it

Skate 3 Recomp 2.0 (built on a fork of the same SDK) draws natively with Direct3D 12 and Vulkan
and reports more than twice the frame rate at a quarter of the GPU power. It does not translate
Direct3D calls: it hooks Skate 3's own engine (`RenderMesh`, `SceneRenderView` and its sort
lists), reads the scene from guest memory and draws it with hand-ported copies of the game's
material shaders (world, characters, water, shadows, post effects), about 1.3 MB of code
specific to that engine. The repository has no license, so its code can't be reused; the
approach can. Proving Ground's engine (Neversoft's) is different, so nothing carries over
directly.

## Two ways to do it here

1. **At the Direct3D level** (as Unleashed Recompiled did): replace the 256 entry points with
   a Vulkan backend and translate the shaders ahead of time. Works for every effect the game
   draws without knowing the engine. The hard parts on the Xbox 360:
   - The CPU writes vertex buffers, constants and textures straight into GPU memory, with no
     lock or upload call, so the backend still has to find out what changed (what the shared
     memory page tracking does now), unless the engine's own update points are hooked.
   - Render targets live in EDRAM and reach textures through resolves; predicated tiling, if
     the game uses it, splits a frame into tiles.
   - Tiled texture layouts, Xbox-only formats, memexport, and any command buffers the game
     records once and replays.
2. **At the engine level** (as Skate 3 did): hook the renderer that calls Direct3D and redraw
   its scene natively. The fastest result, and the most work: every material, effect and
   screen has to be ported and checked by hand.

Either way this is months of work, with the old emulated renderer kept as the fallback while
effects are ported. The gain on the phone would be most of the GPU command thread's 24 to 30
ms per heavy frame, plus the shared memory tracking; the game's own threads (main loop, render
thread, four workers) would then set the limit.

## Smaller steps first

- Name the 256 entry points and log, per frame, which ones the game calls and how often. That
  gives the real size of approach 1 before committing to it.
- Meanwhile, the emulated path's heavy frames break down as vertex buffers 8 to 10 ms (page
  compares and uploads), bindings 5 ms and textures 4 to 6 ms on the command thread; those are
  in `command_processor.cpp` and `shared_memory.cpp`, which the PC performance work is also
  changing, so that work needs to be coordinated with it.

## First trace: where the engine draws (2026-11-06)

`--d3d_trace=<file>` (`src/native/pg_d3d_trace.cpp`) wraps the 255 Direct3D entry points that
game code calls and counts, per frame, how often each is called, on which thread and from
which engine function; `tools/native/d3d_trace_report.py` turns the report into the list
below. One manual session on PC (menus, then skating; 9001 frames) called 122 of the 255.

Direct3D on the Xbox 360 is lazy: the setters only write the device's state, and the draw
call writes the GPU commands for whatever changed. So only one entry point builds commands:

| Entry point | Calls per frame (max) | What it is |
| --- | --- | --- |
| `sub_82578638` | 706 (1774) | The draw: flushes state (shader loads, ALU constants, fetch constants) and emits the draw packets |
| `sub_8257D9F0` | 2092 (5507) | Setter, the most called; 94% from `sub_824C6488` |
| `sub_82576DF8`, `sub_82576FA0` | 1348, 702 | Setters called around each draw |
| `sub_82579550`, `sub_82579468`, `sub_82579720`, `sub_825799D8` | 972, 390, 343, 343 | Setters called while preparing a material |

Almost every call comes from one thread (the game's render thread, index 0 in the report).
The engine functions that make them:

| Engine function | Direct3D calls per frame | Role (from what it calls) |
| --- | --- | --- |
| `sub_824C4128` | 2391, including 550 draws | **The main mesh draw loop**: per mesh, two setters, the draw, and a fourth setter |
| `sub_824C3BD8`, `sub_824C3898` | 286, 267, with 70 and 76 draws | Two more draw loops of the same shape (other passes or mesh types) |
| `sub_824C6488` | 1963 | Per-draw setter loop (likely textures or sampler state) |
| `sub_8250D4E8` | 1754 | Material setup: shaders and constants before the draws |
| `sub_824BEAC8`, `sub_824BEB30`, `sub_824BEC58` | 467, 138, 83 | More material and constant setup |
| `sub_824FF6E0`, `sub_824C60F8` | 221, 268 | Render-state setup (blend, depth, render targets) |

So the engine's renderer sits in 0x824BE000-0x824D1000, plus `sub_8250D4E8` and the
`sub_824FFxxx` state functions. These are where approach 2 hooks the engine: `sub_824C4128`
alone issues about 80% of the draws in gameplay.

## Names and the first engine hook (2026-11-06)

The entry points and engine functions now have names, in `tools/native/d3d_names.txt`
(the trace report prints them). They come from what each function writes into the device
(at 0x40001A00 in guest memory) and from arguments sampled by the tracer: a Direct3D
resource's first word holds its type (0x00100003 texture, 0x00100004 surface, 0x00100005
vertex declaration). Names marked `?` are guesses. The hot ones are sure:
`DrawIndexedVertices`, `SetTexture`, `SetStreamSource`, `SetIndices`,
`SetVertexDeclaration`, `SetVertexShader`, `SetPixelShader` and the two
`Set*ShaderConstantF`.

`sub_824C4128` is `Engine_DrawMesh(mesh record, effect override)`. For one mesh it sets the
vertex declaration (if it changed), the vertex buffer on streams 0 and 1, the index buffer,
then for each pass of the mesh's effect (`Engine_ApplyEffectPass`, `sub_824C68D8`, until it
returns 0) draws the indexed triangle strip. A second path draws the mesh in a flat color
with a blend mode from a 13-entry table. The record's layout is in
`src/native/pg_engine_mesh.h`.

`src/native/pg_engine_hooks.cpp` hooks it (the Skate 3 entry point). For now it only
observes:

- `--native_mesh_stats` logs what the meshes use every 10 s. In the skatepark: about 520
  meshes a frame, all indexed triangle strips with blend mode 3 and no flat-color draws;
  over 10 s about 2,300 mesh records, 2,250 vertex and index buffers, 1,400 effects and 52
  vertex declarations (strides 12 to 60 bytes).
- `--native_skip_meshes` skips it. The scene and the character disappear; the HUD, the
  character's outline and the dark background stay. On the route
  (`tools/dev/route.ps1 measure skatepark`), draws per frame fall from 905 to 272 and the GPU
  command thread's work from 4.49 to 1.38 ms: **this one function is 70% of the draws and of
  the emulated renderer's cost.**

So a native renderer that only replaces `Engine_DrawMesh` already takes most of the
emulation out. What it needs, in order:

1. Vertex data: read the vertex declaration's elements and the buffers' guest addresses
   and sizes, and upload each buffer once (the records and buffers repeat from frame to frame).
2. The effects: each pass's vertex and pixel shader (the shader objects `SetVertexShader`
   and `SetPixelShader` receive) translated ahead of time, as Skate 3 and Unleashed do with
   XenosRecomp, plus the constants `Engine_ApplyEffectPass` sets.
3. Drawing into the emulated renderer's current render target and depth buffer, so the rest
   of the frame (HUD, outline, post effects) still works. This is the decision that needs
   the SDK side: the Vulkan backend has to expose its current EDRAM render target as a
   Vulkan image for a native pass.

## Step 1: the vertex data, decoded (2026-11-06)

`--native_dump=<file> --native_dump_frame=<n>` writes every mesh `Engine_DrawMesh` draws in
frame n, with its record, vertex declaration, buffers, effect and the shaders bound after
the draw; `tools/native/mesh_dump.py` decodes them into an OBJ file and a top-down picture.
On the skatepark route (frame 2200) the decoded geometry is the street corner on screen:
the building with the alley, the curved ledge at its base, the curb and the street.

What the dumps showed:

- **Buffers keep their data right after a 32-byte header.** A vertex buffer's words 24/28
  are its GPU fetch constant (address | 3, and size in words << 2 | endian); an index
  buffer's are its address and size in bytes. Indices are 16-bit, big-endian, in triangle
  strips.
- **A vertex declaration** holds its element count at +24 and its elements from +52, 12
  bytes each, like `D3DVERTEXELEMENT9`. The world uses about 50 layouts, all with a FLOAT3
  position followed by a packed normal (type 0x2A2190), binormal and tangent, a D3DCOLOR and
  one to five HALF2 texture coordinates.
- **World meshes are already in world space**: no per-mesh transform is needed to draw
  them. The 11 meshes whose positions are not plain floats are the characters (skinned);
  one wide, high mesh is the sky dome.
- **Shader objects carry the path of their effect file** (`c:\nxcommon\data\fxfiles_07\...`),
  which will name the effects in step 2.

## Step 2, first look: the shaders (2026-11-06)

The dump also writes each mesh's shader objects (the ones bound after its last pass) and
the memory their microcode pointer leads to; `--dump_shaders=<dir>` writes the microcode the
SDK actually translated, for comparison.

- **A vertex shader object** is type 7: word 6 points at a block that starts with the
  container word 0x102A1100 and holds the size and section offsets (words 11-16). **A pixel
  shader object** is type 6, with its pointer in word 8.
- **The objects name their effects**: each carries the path of the compiled effect file,
  for example `D_3ChkBox_Pass0_Default`, `D_N_S_4ChkBox_ReducedLOD`,
  `D_1VertexColorOnly_Default`, `Billboard_Fast_Default`, `WaterCubeRefraction_Default`.
  One frame of the skatepark uses 69 vertex and 69 pixel shaders.
- **The microcode the GPU runs is not at the objects' pointers.** The SDK keeps it as
  little-endian words; byte-swapped, 266 of the SDK's 469 shaders are found in the same memory
  region (0xE812xxxx-0xE818xxxx), but never at an object's pointer, and the vertex shaders
  never at all. The Xbox 360 Direct3D binds a vertex shader to the vertex declaration by
  patching its fetch instructions, so the code the GPU loads is a patched copy.

So the reliable link from a native draw to its shaders is the one the emulator already has:
the shader the command processor loaded for that draw (its microcode hash, which keys the
SDK's translated shader cache). That points the next step at the SDK: a native draw that
runs inside the Vulkan command processor, with its current render targets and the shaders
it has already translated, but with the mesh's vertex and index data uploaded once per
buffer instead of tracked through shared memory.

## Step 3: static mesh data, requested once (2026-11-06)

Before drawing natively, the cheapest piece of the emulated path to replace was the one the
engine hook can vouch for: the meshes' vertex data.

- **Where the time goes.** `--vulkan_gpu_timing` splits the command processor's CPU time per
  draw. On heavy frames (1500+ draws) of the skatepark route it spends about 5 ms preparing
  draws, 1.1-1.3 ms of it on vertex buffers: about 1000 requests a frame, comparing 4.6-5.7 MB
  of guest memory with the copy on the GPU.
- **The meshes' vertex data never changes.** `--native_vb_check` hashes the vertex data of
  every Engine_DrawMesh call and compares it with the previous frame: about 420 buffers
  (1.3 MB) a frame, none changed along the whole route.
- **But it sits on "hot" pages.** The SDK stops write-protecting a 4 KB page that the CPU keeps
  rewriting and compares it on every request instead. The meshes share pages with data the
  engine rewrites every frame, so about 820 requests a frame for static data paid for that
  comparison (0.46-0.50 ms), and about 20 more re-uploaded pages that other data had changed
  (0.15-0.25 ms).

So the SDK now has a registry of static ranges (`rex/graphics/native_ranges.h`, in the
runtime library so the game can reach it): the game's Engine_DrawMesh hook registers each
mesh's vertex data the first time it sees it (`--native_static_buffers`, on by default), and
the Vulkan command processor requests a registered range once per registration and then
trusts it (`--gpu_trust_native_ranges`, on by default). A buffer seen again with another size
or through another vertex buffer object is registered again, so it is requested again.

Results on the skatepark route, flipping `gpu_trust_native_ranges` every 30 frames in the
same run (`--perf_ab_cvar`), command thread work on heavy frames:

| run | off | on | paired difference |
| --- | --- | --- | --- |
| 1 | 8.16 ms | 6.52 ms | -1.62 ± 0.32 ms |
| 2 | 8.22 ms | 6.54 ms | -1.77 ± 0.36 ms |

The vertex buffer section of a heavy frame went from 1.2-1.35 ms to 0.34-0.58 ms (the rest of
the gain is shared memory work that no longer happens: fewer hot compares, lock waits and
uploads). Screenshots of the route with and without it show the same geometry.

**The one thing it trusts:** that the engine never puts new vertex data into the same
buffer object at the same address with the same size. Streaming in new meshes along the route
did not break that, but a level change has not been tested; `--gpu_trust_native_ranges=false`
turns it off. Finding the engine's mesh load and unload functions would make it exact (the
Skate 3 way: register on load, unregister on unload).

The SDK part is in `patches/rexglue-sdk.patch`; the vertex buffer requests by kind are logged
with `--vulkan_gpu_timing`.

## Step 4: static index buffers, converted once (2026-11-06)

A sampling profile of the command thread (`--sample_profile`) after step 3 showed two more
costs on the meshes' path:

- **The registry lookup itself** (about 4% of the thread): a `std::map` search under a lock
  for every vertex buffer. The command processor now keeps the answers by buffer start
  (found ones until a range is replaced, missing ones until anything is registered), and the
  registry looks up a whole buffer by its start first.
- **The strip restart index.** The game draws its strips with restart index 0x7FFF; Vulkan
  only restarts on 0xFFFF. So the emulator scanned each mesh's 16-bit indices and wrote a
  converted copy into a per-frame buffer: about 560 draws and 190 000 indices a heavy frame,
  every frame. The hook now registers the meshes' index buffers too, and for a registered one
  the primitive processor converts it once into a buffer kept for the whole run
  (`RequestPersistentConvertedIndexBuffer`, 16 MB host-visible chunks on Vulkan, up to 8) and
  reuses it: about 780 reused and 7 scanned a heavy frame afterwards.

Uncapped (`--frame_rate_limit=0`) A/B of `gpu_trust_native_ranges`, which now covers both:
heavy frames 7.80 ms off, 6.43 ms on (-1.27 ± 0.22 ms, 19 pairs). At the start of this work
the same frames took about 8.5 ms. The capture at the same point of the route shows the same
geometry.

Follow-up (same day): the registry keeps a bitmap of the 4 KB pages any range touched, so a
lookup for dynamic data is a bit test, and a static index buffer skips the lookup while no
range was replaced. Uncapped heavy frames: 6.10-6.16 ms. Tried and dropped: keeping a texture
binding when the guest writes the same fetch constant again (+0.29 ± 0.26 ms, no gain).

Follow-up (2026-11-11): the converted copies now live in video memory and their space is
reused. On Vulkan the 16 MB chunks are device-local. Where the device has a device-local,
host-visible heap of at least 1 GB (resizable BAR, integrated GPUs), the chunks are mapped
and written directly, and only the written bytes are flushed when the memory is not coherent.
Otherwise each conversion is written to a staging buffer, and the copy is recorded in the
submission-start command buffer, before every draw of that submission, with a barrier to
index reads. Each chunk has a first-fit free list. A conversion is released when its entry
is replaced by different contents, or when its range has been forgotten and not drawn for
300 frames (at once when space runs out). Its space is reused after the submission that
released it completes, which also covers native mesh draws (step 9) that bind the buffer
without going through the primitive processor. Entries whose range is still registered are
never evicted, for the same reason. When the budget is full, new meshes are converted every
frame as before, and they are tried again once space has been freed. Cvars:
`vulkan_static_index_buffer_budget_mb` (128; 0 converts every frame) and
`vulkan_static_index_buffer_direct_write` (true; false forces the staging path, for testing).

Also tried and dropped (same day), each with an in-run A/B: a material binding cache that
keeps what every texture fetch constant resolved to (it served about 1700 constants a heavy
frame, nearly all of them, and changed nothing: +0.015 ± 0.21 ms), and a per-submission cache
of texture descriptor sets by their image views and samplers (about 80 of 480 sets a frame
came from it; -0.38 ± 0.33 ms in one run, +0.38 ± 0.13 ms in another). Parsing constants and
writing descriptors is not what costs: the remaining CPU, about 3.5 µs a draw, is spread over
the whole emulated path. Big gains from here need draws that skip that path.

## Step 5: does each mesh always draw the same way? (2026-11-06)

Before drawing meshes outside the emulated path, their state has to be something that can be
built once. `--native_draw_census` (logged with `--vulkan_gpu_timing`) records, for every
draw whose vertex buffer is a registered engine mesh, the distinct states it is drawn with:
vertex and pixel shader, Vulkan pipeline and a hash of the used texture fetch constants. A
"draw item" is a vertex buffer, index buffer and index count.

On the skatepark route (whole run, menus included, about 3200 items):

- 65% of the items are always drawn with one state, 30% with two, 5% with three or four.
- The shader pair and the pipeline always change together, and items with two states are the
  most drawn: the same mesh in two passes (a depth or shadow pass and the color pass).
- **Per shader pair, the pipeline and the textures never change for 99.4% of the items (97%
  of their draws).**

So a mesh in a pass is a fixed thing: its pipeline, texture descriptor set, vertex buffer and
converted index buffer can be made once and kept. What changes per draw are the constants
(matrices, per-object values), which the engine knows.

### Plan for native draws, easiest first (step 1 done below)

1. **Replay a known item inside the command processor.** When a draw's item and shader pair
   match a kept entry, and a cheap check (render pass, used fetch constants, the render state
   registers the pipeline depends on) agrees, bind the kept pipeline and descriptor sets and
   skip the texture, pipeline and binding work. The constants are still uploaded as now. This
   stays inside the emulator, so a failed check just falls back.
2. **Constants from the engine.** The hook reads each mesh's matrices and material values
   from the record instead of the emulator collecting the guest constant registers.
3. **Shaders ahead of time.** The translated SPIR-V is already cached by the emulator per
   shader pair; the native path would load those for each kept item at level load.
4. **Drawing straight into the current render target.** Issuing the engine's draws from the
   hook, not from the guest command stream. This needs an SDK design decision on how native
   draws share the EDRAM render target with the emulator.

## Step 6: replaying a known mesh's textures and samplers (2026-11-06)

`native_draw_replay` (default on, hot reload) is the plan's first step. Per draw item and
shader translations, after a normal draw the command processor keeps what the sampler and
texture work produced: the used texture fetch constants, the textures, the samplers and the
descriptor image infos. When the same item comes back, it is replayed if:

- the used fetch constants are the same bytes (a new material on the mesh records again);
- no texture was destroyed and the scaled resolve pages did not change since (generations);
- none of its textures has outdated data (their memory was not written);
- its samplers, marked used once per submission, are still the same objects;
- `anisotropic_override` is unchanged.

Then the sampler parameters, `RequestTextures` and the image view lookups are skipped: the
textures are only marked used and transitioned for sampling, and the descriptor image infos
come from the entry. Any failed check drops the entry and the draw takes the normal path.

Skatepark route: about 470 mesh draws replayed per heavy frame, about 5 failed checks.
In-run A/B (`--perf_ab_cvar=native_draw_replay`, uncapped), heavy frames: 7.06 ms off,
6.34 ms on (-0.79 ± 0.15 ms); a second run -0.33 ± 0.26 ms (the machine was busy with
another game); a third run with the machine idle: 6.12 ms off, 5.60 ms on (-0.47 ± 0.16 ms,
about 164 -> 179 FPS of CPU work). Captures show the same image.

## Step 7: constants (2026-11-06)

The plan's step 2 ("constants from the engine") only saves CPU once draws skip the guest
command stream: until then the game still writes every constant as register writes, which the
command processor already takes in bulk (one byte-swapped copy per range, with dirty bits
only for constants the current shaders use). What a draw still did per constant was the
upload: a bit-scan loop copying 16 bytes per used constant.

`gpu_constant_runs` (default on, hot reload) works out the contiguous runs of the current
vertex and pixel constant maps once, when a map changes, and each draw copies its constants
with one memcpy per run. The bytes uploaded are the same. In-run A/B, uncapped heavy frames:
-0.27 ± 0.20 ms and -0.20 ± 0.33 ms: a small gain, not separable from noise in one run.

Reading constants straight from the engine waits for step 4 (native draws), where it replaces
the register writes altogether.

## Design: native draws (plan step 4, 2026-11-06)

### Where the time is

Heavy frames are bound by the GPU command thread: it handles about 1700 draws in ~5.6 ms,
about 3.3 µs a draw, of which engine meshes are 50 to 70%. Step 6 already skips the sampler
and texture work for known meshes. What every draw still pays (`--vulkan_gpu_timing`
sections, roughly in order of cost): bindings (constants and descriptor sets), render target
update, vertex buffer checks, primitive processing, system constants, pipeline lookup,
viewport and dynamic state, shader translation lookups, then the draw. For a mesh whose state
is known (99.4% of mesh items are, per pass, step 5) almost all of that recomputes the same
answer every frame.

### Where a native draw is issued

The engine's draws and the emulated ones must reach Vulkan in the order the game issued
them: they share the render target, blending and depth with everything around them, and
render targets live in EDRAM, which the emulator's render target cache owns. Drawing from the
hook on the game thread, straight into Vulkan, would race the command thread and break that.
So native draws are issued **on the command thread, in stream order, into the render pass the
emulator has open**. That answers the EDRAM question by construction: a native draw is just a
cheaper way to issue a draw the emulator was about to issue, into the same render target.

Two ways to get there:

- **A. Fast path in the command processor (recommended first).** The game runs unchanged; the
  command processor recognizes a known mesh draw and issues it from kept state, skipping
  nearly all of `IssueDraw`. Any failed check falls back to the normal path for that draw,
  so it can't leave the frame wrong.
- **B. Engine bypass.** The hook skips `Engine_DrawMesh`'s own Direct3D work and writes one
  marker packet into the command buffer instead; the command processor draws from it. This
  also saves the game thread's time, but cannot fall back on the command thread (the
  engine's packets are gone), needs the engine's Direct3D state shadow kept in step, and
  needs reverse engineering of how the engine writes its command buffer. Worth it only once
  the game thread, not the command thread, sets the frame time.

### A in detail

**Draw signature.** Each draw's state is the registers the game wrote. The command processor
keeps a running 64-bit hash of all render state registers (everything except float, bool,
loop and fetch constants, and a short list of registers that change every draw without
affecting rendering, found by measurement): `WriteRegister` updates it in O(1) by hashing out
the old (register, value) and hashing in the new one. Two draws with the same hash, shaders
and used fetch constants have the same pipeline, render target setup, viewport, system
constants and textures.

**Kept draw entry** (per mesh item and shader translations, extending step 6's entry):
the state hash it was made for, the pipeline and pipeline layout, the render pass and
framebuffer, the persistent converted index buffer and its offset, the host draw parameters
(primitive type, index count, base vertex), the system constants bytes, the dynamic state
(viewport, scissor, depth bias, stencil), and step 6's textures, samplers and image infos.

**Checks before a native draw** (all cheap compares):
1. A kept entry for the item, made for the current state hash.
2. The render target cache's current render pass and framebuffer are the entry's, and it has
   nothing pending (no ownership transfer or resolve since): a render target epoch, bumped by
   anything in the render target cache that changes EDRAM ownership or bindings.
3. Step 6's texture and sampler checks.
4. The vertex and index buffers are still trusted static ranges (registry generation).
5. No memexport, occlusion query or predication in the draw.

**Issuing it:** bind the kept pipeline if it differs, set the dynamic state that differs,
upload float constants (step 7's runs; these change per draw) and reuse the kept system
constants, bind descriptor sets, bind the kept index buffer, draw. Then mark the emulator's
"current" caches the native draw changed (bound pipeline, descriptor sets, dynamic state,
index buffer) as unknown, so the next emulated draw rebinds what it needs.

**Fallback:** any failed check runs the normal `IssueDraw`, which also refreshes the entry.

**Expected gain:** the per-draw cost of a known mesh draw going from about 3.3 µs to about
1 µs (constant upload, binds, draw); with 900 to 1200 mesh draws in a heavy frame, 2 to 3 ms
of the ~5.6 ms. On the phone, where the command thread takes 24 to 30 ms a heavy frame, the
same fraction matters more.

**Risks:**
- Registers that churn without mattering lower the hit rate; measured first, before any draw
  is skipped.
- System constants that depend on more than registers (resolution scale, render target
  layout): included in the entry's checks, or recomputed if found.
- Hash collisions: 64-bit, and an optional debug mode that runs the normal path and compares
  its result with the entry.

### Steps

1. **State hash and census.** Add the running register hash; log, per heavy frame, how many
   known mesh draws find their entry with the same hash, and which registers changed when they
   do not. Nothing is skipped yet. Decides whether A is worth building.
2. **Skip pipeline, render target update and system constants** for draws that pass, with the
   debug compare mode on during testing. A/B per step.
3. **Skip primitive processing and vertex buffer checks**, then the remaining sections, until
   a known mesh draw is constants + binds + draw.
4. Only then, if the game thread sets the frame time, consider B.

## Step 8: the render state hash (design step 1, 2026-11-06)

The command processor now keeps the running register hash from the design (`state_hash_`,
updated in `WriteRegister`; nothing reads it for drawing yet). `--native_state_census`
(with `--vulkan_gpu_timing`) counts, for each known mesh draw, whether the hash equals the one
at the item's previous draw, logs the registers that change most, and for up to 256 items
keeps a copy of the registers to log which ones differ when the hash does.

Hashing every non-constant register matched only 2.5% of known mesh draws. The per-item
comparison showed why: almost every mismatch was `CP_RB_WPTR` (the ring buffer write
pointer) and `D1GRPH_PRIMARY_SURFACE_ADDRESS` (the display surface, which flips every frame),
neither of which changes how a draw renders. With those and the other non-drawing registers
left out (`native_state_hash_exclude`, default: ring and display, scratch and callback,
coherency and wait, event initiator, resolve copy and clear, occlusion query address,
constant flushes), **99.5% of known mesh draws (about 640 per heavy frame) come with exactly
the same render state as their item's previous draw**. The remaining differences are real:
a mesh drawn into a different render target and viewport (another pass), stencil reference,
interpolator control.

The Direct3D layer writing only changed registers does not get in the way: the state an
engine mesh is drawn with is the same every frame, leftovers included. So the design's
fast path can key on (item, shaders, state hash) as planned. Next: design step 2, skipping
the pipeline, render target update and system constants for draws whose hash matches.

## Step 9: pipeline, viewport and system constants from the entry (design step 2, 2026-11-06)

`native_draw_fast` (default on, hot reload). A replayed mesh draw (step 6) whose render state
hash equals the one its entry was made with, and whose render pass is the same, takes its
pipeline and layout, viewport and system constants from the entry instead of configuring the
pipeline, computing the viewport and rebuilding the system constants. Otherwise the draw
computes them normally and the entry keeps the new results. The render target update still
runs for every draw: it tracks EDRAM ownership per draw, and skipping it safely needs the
render target cache to say nothing changed since its last update (next).

Fixed on the way, in step 6: the system constants take each texture's swizzle, signedness and
resolution scaling from the texture cache's bindings, which a replayed draw does not update,
so a replayed mesh could get the previous draw's values for a slot. The entry now records
them, and the system constants use the entry's while replaying.

`--native_draw_fast_verify` computes everything normally even when the entry could be used
and counts differences: on the skatepark route, about 700 checks per heavy frame, **0
pipeline and 0 viewport or system constant differences**. In-run A/B with the machine free:
heavy frames 7.03 ms off, 6.78 ms on (-0.21 ± 0.11 ms); runs while another game was running
were too noisy to read (±1.9 ms).

## Materials: what porting shaders by hand would cover (2026-11-06)

`--native_draw_census` now also ranks materials (vertex and pixel shader pairs) by draws. On
the skatepark route (whole run, menus included; the machine was busy, so read the shares, not
the times): 230 shader pairs. The two most drawn are tiny and not engine meshes (a 24-dword
vertex shader with a 9-dword pixel shader, 16.7% of draws, and the same vertex shader with no
pixel shader, 11.7%). Engine mesh materials are a long tail: the largest is 6% of draws, the
next ones 1 to 2% each; the top 20 pairs cover 63% of draws.

So porting shaders by hand, material by material, buys GPU time (simpler shaders than the
translated microcode, which matters most on the phone) a percent or two of draws at a time.
The CPU gain does not depend on porting: a native draw can use the shader the emulator already
translated, and the fast path covers every known mesh at once (about 60% of draws). Plan:

1. Finish the fast path (render target update, primitive processing, vertex buffer checks)
   for all known meshes, with the emulator's translated shaders: the CPU gain.
2. Port shaders by hand where the GPU time is: first find which pairs cost the most GPU time
   (draw count × pixels × shader length), starting with the 6% mesh material and the two tiny
   non-mesh pairs (find what draws them).

## Step 10: render target update skipped when nothing changes (2026-11-06)

`gpu_render_target_update_memo` (default on, hot reload; covers every draw, not only engine
meshes). The render target cache's `Update` reads the render target, scissor, window offset,
clip and stencil reference registers and the draw's depth control, color mask and discard
state; for clipped, non-rectangle draws it reads nothing else. When those inputs equal the
last update's, that update needed no EDRAM ownership transfers, and nothing else used the
render target cache since (a resolve, the end or start of a submission, the frame generation
depth sampling, clearing the cache), updating again would find the same render targets, make
no transfers and keep the same render pass and framebuffer, so it is skipped.

`--gpu_render_target_update_memo_verify` updates anyway and counts updates that changed
something where the memo would have skipped: 0 on the route. About 200 of 540 updates a frame
are skipped. In-run A/B, both while another game was running (heavy frames 10 to 12 ms):
-1.85 ± 0.47 ms and -0.76 ± 0.25 ms; the gain on a free machine is likely smaller.

## Engine bypass, phase 1: marker packets (2026-11-06)

The emulated command processor only sees Xbox 360 GPU packets, so it cannot tell which engine
mesh a draw belongs to. Phase 1 of the engine bypass gives it that: with
`--native_draw_markers`, every D3DDevice_DrawIndexedVertices (sub_82578638) made from inside
Engine_DrawMesh is preceded by a type-3 NOP packet with three payload dwords: the magic
`NATV`, the mesh record address and the pass (the draw's index within that Engine_DrawMesh
call). The game writes it into the device's command buffer the way the draw writes its own
packets (dev+48 last written dword, dev+56 limit, sub_8257B2D8 makes room). The SDK command
processor reads it in its NOP handler and the next draw packet consumes it.

How the draw is found, from the recompiled DrawIndexedVertices: the setters only update the
device's shadow and dirty masks (dev+0..+40); the draw flushes the dirty ALU constants
(sub_8258BD98, vertex from dev+1920 at register 0x4000, pixel from dev+6016 at 0x4400) and
register blocks (sub_8258B9F8, shadow blocks at dev+10112..+10832), then writes the draw
packets. State left dirty by a skipped draw is flushed by the next one, which is what phase 2
relies on.

Result on the route: every marker is consumed by a draw, none replaced before a draw; ~410 of
~475 known mesh draws per heavy frame carry one (the rest come from other engine paths into the
same buffers), ~1 marked draw per frame is not a known mesh draw yet. No visual change. An
in-run A/B of the markers alone gave +0.87 ± 0.45 ms on a busy machine, within noise of the
4 stores and one NOP per draw they cost. On by default since phase 2.

Next, phase 2: for a marked draw of a learned (record, pass), the command processor draws from
what it kept (pipeline, descriptor sets, host index buffer) instead of running the whole
emulated draw, and the engine side stops producing the Direct3D work it no longer needs.

## Engine bypass, phase 2: native draws of known meshes (2026-11-06)

Where the command thread's time goes decided the shape of this phase: in a profile of the
route, the draw function (IssueDraw) takes ~70% of the GPU command thread, the packets that
set registers and constants ~10%. So the game keeps emitting its state (the command processor's
registers stay exact), and the gain comes from not running the emulated draw for meshes it
already knows.

`native_mesh_draw` (on by default, with `--native_mesh_draw_verify` to check): the first full
draw of a marked (mesh record, pass) is learned when it was replayed with a valid fast entry
(steps 6 and 9), uses a static converted index buffer (step 4), only trusted static vertex
ranges (step 3) and the render target memo applies (step 10). The entry keeps the active
shaders, the draw and index buffer parameters, the persistent converted index buffer (buffer
and offset: the primitive processor's handle is per frame), the vertex fetches and the replay
key. A later marked draw of the same (record, pass) is drawn from it after cheap checks: same
draw and index buffer, same shaders, ranges not removed since, same render state hash as the
fast entry, same vertex fetch constants, render target memo hit with the same render pass,
textures and samplers still valid (the replay checks). It skips primitive processing, shader
modification and translation lookups, the replay key, vertex buffer requests, the pipeline
lookup, the viewport and system constant computation; it still updates the constants and
descriptor bindings (UpdateBindings) and binds the textures. Any failed check draws the usual
way.

Verify: the only difference between an entry and the full draw it would have replaced was the
index buffer handle (a per-frame index); keeping the persistent buffer and offset instead left
~0.1/frame differences in the render target registers, which the native path reads from the
current registers anyway. Route: ~285 of ~410 marked draws per heavy frame drawn natively;
the rest are first draws, ~70/frame not learnable (no static converted index buffer or
untrusted vertex data), ~40/frame render target changes. In-run A/B on a busy machine: heavy
frames -0.65 ± 0.22 ms and -0.68 ± 0.74 ms (9.05 -> 8.10 ms mean). No visual change in the
captures. Level changes are still untested.

Next: the ~70 not-learnable draws (guest DMA index buffers, other vertex data), keeping
descriptor sets per entry so UpdateBindings only uploads constants, then hand-ported shaders.

### Phase 2b: guest index buffers (2026-11-06)

A census of why marked draws were not learnable (logged with `vulkan_gpu_timing`) gave, per
heavy frame, ~60 draws whose index buffer is used straight from guest memory (kGuestDMA:
lists without restart, nothing to convert) and ~20 whose vertex data is not a registered
static range (likely animated characters). The primitive processor requests guest index
buffers on every draw. Now, when such an index buffer lies in a range the game registered as
static, the draw that learns the mesh (which just requested it) marks the range trusted, like
the static vertex ranges, and the entry keeps the shared memory buffer and the index offset.
Not learnable: ~60-80 -> ~15-25 per frame; verify still finds no differences; in-run A/B of
native draws on/off: heavy frames -1.52 ± 0.43 ms (busy machine, 10.65 -> 9.17 ms), up from
-0.65 ms before.

## Native render targets: design (2026-11-06)

### What the frame does today

From `--vulkan_gpu_timing --vulkan_gpu_timing_min_draws=1500 --vulkan_log_frame_sequence=1`
(heavy skatepark frame, ~1500 draws): occlusion points, two shadow depth maps (pitch 13 and 7
tiles), a small 320-wide pass, the main 1280x720 pass (color C0 at tile 0, depth D0 at 720),
then the post-processing chain: ~25 resolves (bloom downsamples into 08D68000, 08D62000,
08D5D000..., scene color into 07656000 / 085EC000) with small draws between them. Every
target is 1x, but each pass started with Direct3D's clear drawn on a 4x surface of half the
size (the Xbox way of clearing faster). The render target cache then made a 4x render target
and copied the EDRAM 1x -> 4x -> 1x around every clear. In heavy frames (busy machine) the
transfers were ~1 ms of GPU, the resolves ~0.9 ms, out of ~7.4 ms for the main scene.

### Decision

Native render targets are reached in steps, with the emulated EDRAM (the render target cache)
kept as the fallback for anything not recognized:

1. Clears made the native way: the 4x clears are drawn as 1x (done, below).
2. No EDRAM copies into a render target that the next draw overwrites whole (the shadow maps
   and the small pass inherit the main pass's EDRAM area and copy it, then clear it).
3. Resolves become direct textures: when a texture is fetched from a resolve destination that
   no one wrote since, sample the render target image itself (format, size and swizzle known)
   instead of copying it to guest memory and loading it back. The post-processing chain is
   almost all of this.
4. The main pass and the shadow maps in our own Vulkan images, with the engine's own clears and
   resolves hooked like Engine_DrawMesh, so the EDRAM model is no longer needed for them.

Steps 1-3 stay inside the SDK's render target cache and are checkable against it (verify
modes, captures); step 4 is the end state.

### Step 1: 4x clears drawn as 1x

`gpu_msaa_clear_as_1x` (on by default): a draw with Direct3D's clear vertex shader (ucode
0A6D1DD7767FDF27: screen-space rectangle, color from the vertices) or a draw that writes
nothing, on a 4x surface, is drawn on the 1x surface of the same EDRAM: surface pitch and HiZ
pitch doubled, MSAA 1x, the viewport transform enabled with scale 2 and offset 2*offset+0.5
(pixel' = 2 * (x * scale + offset + half pixel)), scissors doubled. The registers are changed
for that draw only and restored after. Every sample of a cleared pixel has the same value, so
the EDRAM contents are the same. Result: the 4x render targets and the 1x <-> 4x copies are
gone from the frame (~2.5 clears per frame); in-run A/B -0.28 ± 0.19 ms on heavy frames that
are CPU bound (the GPU saving is larger than what shows in frame time); shadows and lighting
unchanged in the captures. A guest access violation at startup was seen once in six runs with
it on and not reproduced since.

### Step 2 findings: the remaining transfers

The frame sequence log now also names the draw behind each transfer and, for Direct3D's
clears, their rectangle (from the clear's vertex buffer, fetch constant 95, 8-in-32 endian,
7 dwords per vertex: x y z then the color). Of the five transfers left after step 1:

- Into the two shadow maps (pitch 13 and 7 tiles) and the small pass (pitch 4): the draw is
  Direct3D's clear, the first draw on a target whose EDRAM held the previous pass. The copied
  contents are overwritten right away, but Direct3D splits a clear into chunks (the 1024x1024
  shadow map is cleared as x 0-960 and 960-1024 in 1x pixels; the main pass as 1280x720 in
  one), so the chunk that causes the transfer never covers the whole target.
- C0 at tile 0 from D0: a full-screen post-processing draw (vs ...0C0DE1F84) over EDRAM that
  held depth; possibly needed, not touched.
- Two 1-3 tile transfers in the bloom chain.

Plan: make Direct3D's clears render target cache clears (the cache already leaves a resolve
clear's rectangle out of its transfers): rectangle from the vertices, color from the vertices,
depth from z, stencil from RB_STENCILREFMASK, then no transfer for the cleared area and no
clear draw. Then step 3 (resolves as direct textures).

### Step 2: Direct3D's clears as render target cache clears

`gpu_clear_as_render_target_clear` (default on; `gpu_clear_as_render_target_clear_kinds`
1 = depth only, 2 = with color, 3 = both) recognizes Direct3D's clear (vs 0A6D1DD7767FDF27,
a 3-vertex rectangle list) before the draw and hands it to the render target cache as a
resolve clear (`VulkanRenderTargetCache::ClearHostRenderTargetsRectangle`): the cache gives
the rectangle to the cleared render targets without copying the EDRAM's previous contents,
and no clear draw is made. The rectangle, depth and color come from the vertices; the stencil
from RB_STENCILREFMASK (zpass ZERO or REPLACE, write mask 0xFF); 4x surfaces are cleared as
the 1x surface of twice the pitch (as in step 1). Anything else (a non-8-aligned rectangle,
blending, a partial color mask, a non-zero color, a pixel shader other than Direct3D's
clear one when it is needed) is drawn the usual way.

One rule found by bisecting: when the clear writes depth and color into the same EDRAM (the
game clears 1280x720 and 320x176 targets like this, depth base = color base = 0), which of the
two owns the EDRAM afterwards decides what later passes see; clearing both as a resolve clear
washed out the image (bloom/lighting), so those clears are left to the draw.

Result: the transfers into the shadow maps, the small pass and the main pass are gone; only the
1-3 tile bloom transfers remain. In-run A/B (heavy frames, 30-frame halves): -0.24 +- 0.14 ms.
Images match the step 1 captures.

Draws that are not recognized as clears but overwrite a whole render target (a rectangle list
with replace blending, a full color mask, depth and stencil "always" + write) also skip the
copy of the previous EDRAM contents (`render_target_skip_overwritten_transfers`, default on,
hot-reloadable; turn it off to A/B). The decision is made in the render target cache's
`Update`, before the pipeline is known, so the skipped transfers are kept until the draw is
recorded. If it is not (a placeholder pipeline while the real one compiles asynchronously,
whose pixel shader discards everything, or any failure after `Update` in `IssueDraw`), they are
performed then (`RenderTargetCache::OnLastUpdateDrawAbandoned`), which gives the same contents
as not skipping them since nothing was drawn in between. Pixel shaders that may kill pixels
(kill, memexport, alpha test other than "always", alpha to coverage) never skip.

### Step 3: resolves as direct textures

What resolves cost, measured on a heavy frame (3.1 ms GPU): about 37 color resolves, each
followed by exactly one load of the same texture (the 1280x720 targets, the 320x180 and 80x45
bloom/blur ping-pong at 08D68000/08D62000, the 32x32..1x1 luminance chain). Loads of those
targets: 0.23 ms GPU; resolves (dump to the EDRAM buffer, copy to shared memory): 0.07 ms
GPU, ~4 us CPU each. Skipping every color resolve (debug, wrong image) bounded the frame-work
gain at -0.13 +- 0.25 ms on this machine at 1x: the win is GPU-side and grows with resolution.

`gpu_resolve_direct_texture` (default on) does a resolve as one `vkCmdCopyImage` from the
render target image straight into the texture's image when:

- the source is one 1x k_8_8_8_8 render target owning every tile of the rectangle
  (`RenderTargetCache::GetSoleOwnerRenderTarget`), with no clear in the resolve;
- the destination is k_8_8_8_8 (no exponent bias, not an array slice, at 0,0) and a 2D tiled
  8888 texture without mips was already loaded at that address with the same pitch and endian,
  is current, and the resolve covers all of it.

Nothing is written to shared memory and the texture is not invalidated, so no load follows.
Red/blue swap in the resolve (most of the game's) is kept in the texture (`contents_rb_swapped`)
and undone in its image views. Coherence: the ranges written only to images are tracked; a
load from shared memory over one (a different texture over the same memory) puts that address
back on the usual path for good, and a usual resolve over a range makes it current again. In
the route only the 1x1 luminance target (07655000) and 0727D000 fall back.

Result: loads of resolved targets 36.7 -> 4-6 per heavy frame (0.23 -> 0.01-0.03 ms GPU), resolve
CPU ~4 -> ~2 us each. In-run A/B on a loaded machine: -0.40 +- 0.68 ms. Images unchanged
(colors checked, so the swap is right).

### Step 4: shadow maps and the main pass on native images

With host render targets the main pass, the shadow maps and the post-processing chain already
draw into their own Vulkan images; what tied them to the emulated EDRAM was clears (step 2)
and resolves, which went render target -> EDRAM buffer -> shared memory -> texture load. After
steps 1-3 the color side is native; this step does the same for depth:

- The two shadow maps (float24 depth, host D32_SFLOAT_S8_UINT, 1024x1024 at 06E18000 and
  512x512 at 05630000) are resolved by the game every frame and sampled as k_24_8_FLOAT
  (host R32_SFLOAT). `gpu_resolve_direct_texture` now copies their depth aspect into the
  texture through a scratch buffer (`vkCmdCopyImageToBuffer` + `vkCmdCopyBufferToImage`;
  depth -> color image copies are not allowed directly). GetResolveInfo reports a depth
  resolve's destination as the depth's texture format, so that is what is matched.
- Host depth is float32, the guest value float24: the texture now has the host value (what
  the depth test used), not the float24 rounding of it. Shadows checked: no acne, same edges.

Engine-level hooks for clears and resolves turned out unnecessary: recognizing them in the
command processor (Direct3D's clear rectangle, RB_COPY_CONTROL) gives the same native
operations without touching guest code, and keeps the emulated path for anything unusual.

Result: in-run A/B of gpu_resolve_direct_texture with color and depth: -0.83 +- 0.25 ms per
heavy frame. What is left of the EDRAM emulation in the route: ~10 small transfers
(~0.12 ms GPU, the shadow map's first draw over the main pass's EDRAM and the luminance
chain), the 1x1 and 0727D000 resolves (fall back, read through another texture), and the
emulated path for anything the checks above reject.

Correction after testing: people flickered with the shadow maps copied directly. By default
(`depth_float24_convert_in_pixel_shader` off) host depth is plain float32 and the usual resolve
converts it to float24; the direct copy gave the shadow test the unconverted value, and the
near-equal compares of characters shadowing themselves flip from frame to frame. The depth
direct path now runs only when the host depth is already float24-exact (that cvar on). To get
the -0.8 ms back by default, the copy needs the float32 -> float24 (truncating, or rounding with
depth_float24_round) -> float32 conversion in a small compute pass over the scratch buffer.

Follow-up (shadow maps back on by default, plus robustness):

- Depth goes through a small compute pass over the scratch buffer: float32 -> float24 (truncated,
  or rounded with depth_float24_round, as xenos::Float32To20e4) -> float32, the value the usual
  resolve and k_24_8_FLOAT load give. The depth direct path runs by default again.
- Color resolves with copy_dest_swap no longer keep red/blue swapped in the image with a view
  swizzle (native replays cache views, so a view change could be missed); they go through the
  same buffer pass, which swaps the bytes, so the image holds exactly what a load would give.
  Resolves without the swap stay a single vkCmdCopyImage.
- After a direct resolve, the other textures over the range are invalidated as a resolve would
  (`TextureCache::InvalidateRangeExcept`): a different format/size of the same memory would
  otherwise never reload; its reload then puts the address on the usual path.
- `gpu_resolve_direct_texture_color` / `_depth` select the two parts (both default on).
- Checked with the user watching runs: no flicker in normal play. The NPC outline that pulses
  is the game's own effect (it looked like a regression in A/B runs).
- A/B (whole direct path, loaded machine): -0.38 +- 0.29 and -0.21 +- 0.17 ms per heavy frame.

### Remaining transfers: merged clear chunks

Of the ~10 transfers left, the largest came from Direct3D clearing in chunks (shadow maps x
0-960 + 960-1024 and 0-480 + 480-512, the 256x256 target 0-240 + 240-256): the first chunk as a
render target cache clear made the cache copy the rest of the target from its previous owner,
then the second chunk overwrote it. The clear now waits (`pending_rt_clear_`) and the next
chunk with the same targets, values and rows, side by side, is merged into it; it is done at
the next draw that is not a mergeable clear, at a resolve and at the swap. The 1024 shadow
map's transfer went 0.031 -> 0.007 ms (what is left is the partial tile column past 1024).
Still on the emulated path (~0.09 ms GPU per heavy frame): the full-screen passes over memory
another target owned (C0 <- D0, the 1-3 tile luminance chain C0 1440 pitch 1 <-> pitch 4),
partial tiles past the cleared rectangles, and the two fallback resolves.

### Resolution scaling

Steps 2-4 were limited to `resolution_scale` 1. Now:
- Clears as render target clears pass the draw resolution scale in the synthetic ResolveInfo;
  the rectangle stays in guest pixels and the cache scales it, as for the guest's resolve clears.
- Direct resolves learn the textures the game samples at the current scale (`scaled_resolve`
  keys when scaled), copy from the render target at source x/y times the scale, with the
  texture's scaled size, and mark the range like the usual resolve (`MarkRangeAsResolved`:
  scaled pages, other textures over the range invalidated).
- The half-pixel offset gap (`resolve_resolution_scale_fill_half_pixel_offset`: scale / 2 host
  columns/rows on the left/top of the resolve region get the first covered column/row) is
  copied the same way: separate copy regions, none written twice. Only scales 1, 2 and 4 (where
  that count is unambiguous) take the direct path; scale 3 resolves the usual way.
- A/B at 1440p (`--resolution_scale=2`, unloaded machine, separate runs): heavy frames
  7.31 -> 6.45 ms (137 -> 155 fps), mean 277 -> 299 fps. GPU timing: resolved-target texture
  loads 0.98 -> 0.01 ms, transfers 0.31 -> ~0.07 ms; the clears now count under resolves.

## Engine bypass, phase 3: design (2026-11-07)

### Where the time is now (720p, uncapped, `profile_route.ps1 -Sample -Timeline`)

Heaviest 5% of frames (~7.3 ms in the diagnostic build): the GPU command thread works ~5.5 ms
(waits 1.2 ms for the game, 0.5 ms on fences); the game's render thread (XThread605C) works
~5.3 ms and waits ~1.9 ms. Over the run the render thread is 97% "busy", but 22% is the swap's
spin on the flip counter (`sub_824853A8`) and ~18% Direct3D's spin waiting for the GPU to
consume the command buffer (`sub_825845E8`, under `sub_8257AC80` / `sub_825740C8`). So the two
threads are about even and run partly in series: phase 3 pays only if it cuts both.

Render thread, inclusive: `Engine_DrawMesh` 25.6% (of which `Engine_BindTextures` 12%,
`DrawIndexedVertices` 8.8%, the constant flush `sub_8258BD98` 3.7%), `Engine_ApplyMaterial` 5%.
Our own hook's `RegisterStaticRange` was 2.8% (an unordered_map under a mutex, twice per mesh):
a direct-mapped per-record cache of the buffers' fetch words now skips it (1.0%).

### How Direct3D flushes a draw (sub_82578638)

The device mirrors the GPU registers at fixed offsets and keeps five 64-bit dirty masks:
dev+0 vertex shader float constants (dev+1920 -> register 0x4000), dev+8 pixel shader float
constants (dev+6016 -> 0x4400), dev+16 render state (ranges 0x2000/0x2100/0x2180/0x2200 from
dev+10368/10444/10528/10548, plus shader setup in `sub_8258CB30` / `sub_8258B660`), dev+24
fetch constants (`sub_8258BC38`, and 0x2280 from dev+10596), dev+32 the rest (0x2300, 0x2380,
bool/loop constants 0x4900 from dev+10112). The draw writes every dirty range into the command
buffer (dev+48 write pointer), clears the masks, then writes the DRAW_INDX packet. Census on
the route (`--native_mesh_stats`): every mesh draw has vertex constants (one group, likely the
world matrix) and pixel constants (~4 groups) and fetch constants dirty; 55% also render state.

### Plan

The engine compares some state against its own caches, not the device: `Engine_DrawMesh` sets
the vertex declaration only when it differs from a global (0x82C2ED10 + 11584). So skipping
Direct3D calls while the engine runs would desync those caches; each one must be found and
reset after a bypassed mesh.

1. **3a, capture instead of flush.** For a (record, pass) the command processor reports as
   drawable natively (a registry like `native_ranges`, written by the SDK), the hook replaces
   `DrawIndexedVertices` with one NOP packet carrying the record, pass and the used float and
   fetch constants read from the device mirror; the dirty masks stay set, so the next
   emulated draw writes everything still dirty. Only when the dirty render state is the one
   learned for that entry. Saves the flush on the render thread and the register packets on
   the command thread.
2. **3b, capture the setters.** Inside a bypassed `Engine_DrawMesh`, the Direct3D wrappers
   (`pg_d3d_trace.cpp` already wraps every entry point) record `SetTexture`, the constant
   setters, shaders and streams instead of running them; the engine's caches are reset after.
   Saves `Engine_BindTextures`' 2000 SetTexture calls a frame.
3. **3c, the main pass at our own resolution.** The native draws go into images of any size
   (e.g. 1.33x or 1.7x of 720p for FSR); shadow maps and post effects keep the integer
   resolution scale. Needs the post chain's reads of the main pass redirected to those images.

Expected: 3a+3b take roughly 1-1.5 ms off each thread in heavy frames, about 1.3x on heavy
frames (not 2x: the command thread still issues every draw, and the waits between the threads
remain). 3c is a GPU-side gain, at high resolutions.

### Phase 3 ceilings, measured (2026-11-07)

Measurement-only cvars: `native_experiment_drop_mesh_draws` (game side: Engine_DrawMesh's
DrawIndexedVertices is not called, so no flush, no packets, no draw) and
`native_experiment_drop_marked_draws` (SDK: the command processor drops marked draws; the game
still flushes and the packets are still parsed). Route, 720p, uncapped, frame time p95:

| | p95 | p99 | mean fps |
| --- | --- | --- | --- |
| base (two runs) | 5.00 / 5.19 ms | 6.25 / 6.64 ms | 560 / 528 |
| command processor drops mesh draws | 3.71 | 4.81 | 829 |
| game drops mesh draws (no flush) | 3.15 | 4.47 | 916 |
| meshes skipped entirely | 2.85 | 3.24 | 1125 |

So in a heavy frame the meshes cost ~1.4 ms in the command processor's (native) draws and the
GPU, ~0.55 ms in Direct3D's flush and the packets it makes (the most 3a can save, less the
packet it writes instead), and ~0.3 ms (p99 ~1.2 ms) in the engine's own mesh code and setters
(3b). The largest piece is the command processor's per-draw cost of native draws.

### The native draw's own cost (2026-11-07)

With the ceilings showing the command processor's native mesh draws as the largest piece, the
draw section timing (`vulkan_gpu_timing`, `vulkan_gpu_timing_min_draws=1500`) now splits the
native path (`native: ...` sections) and UpdateBindings (`bindings: ...`). Heavy frames, 720p:
~1.78 ms in native mesh draws (~300 a frame, ~5.8 us each, more than an emulated draw's
average), of which UpdateBindings 0.82 ms, and inside it writing descriptor sets 0.69 ms
(every native draw wrote a new transient texture descriptor set: the textures differ from the
previous draw's, so the reuse of the last set rarely applied).

- `native_texture_sets` (default on, hot reload): a replayed draw keeps its vertex and pixel
  texture descriptor sets in its replay entry, written once with the entry's image infos. They
  stay valid while the entry does: the entry is dropped when the texture lookup generation
  changes (a texture destroyed), the captured textures are outdated or the samplers are not the
  same objects, and re-recording it clears the sets. They come from their own pool
  (16384 sets); when it is full it is retired (reset once the frames using it are completed) and
  an epoch counter makes every kept set be written again in a fresh pool. Route: 99.6% of
  native texture bindings reuse a kept set; descriptor writes 0.69 -> 0.22 ms per heavy frame.
- TryNativeReplay re-checked each entry's samplers once per submission with a hash lookup each
  (`UseSampler`), more than half of its time: the entry now keeps handles to the texture
  cache's sampler entries, valid while no sampler was destroyed (`sampler_generation`), and
  marks them used directly. Its sampler part halved.
- A/B in one build (separate runs, unloaded machine), `native_texture_sets` on/off: GPU command
  thread work in 1500+ draw frames 4.24 / 4.13 ms vs 4.54 / 4.62 ms; frame time p95 4.53 / 4.41
  vs 4.64 / 4.73 ms; mean 600 / 620 vs 580 / 571 fps. (At the start of the day: 5.44 ms work,
  p95 5.0-5.2 ms.)

What is left per native draw (heavy frames): the replay checks ~0.3 ms (lookup, fetch constant
compare, textures, samplers), uploading constants ~0.2-0.35 ms (all draws), the checks before
the draw ~0.2 ms (including draws that then fall back), pipeline and dynamic state ~0.13 ms.

### Static ranges after the CPU writes them (2026-11-07)

Bug found by the user in play (night parking lot, after the skatepark): a head, the sky and the
upper half of buildings missing, "a vertex explosion". With `--gpu_trust_native_ranges=false`
it was gone: static ranges (step 3) were trusted forever once requested, and nothing ever
unregistered them, so when the game put other data where a registered mesh had been, the
emulator kept drawing the old data.

- `native_ranges::InvalidateWritten`, called from the shared memory's invalidation callback
  (a watched page faulted, or a file was read into memory): vertex ranges in the written pages
  are marked dirty (the command processor requests them again, clearing the mark, before
  trusting them; a native mesh draw over a dirty range falls back), index ranges are forgotten
  (their conversions are kept by generation; the hook registers them again when the mesh is
  drawn, also when the registry no longer has a range it registered).
  `native_ranges_invalidate_on_write` (default on) turns it off.
- First version forgot every written range: ~2-3 per frame on the route (pages shared between
  a mesh and data the CPU writes every frame, e.g. 14EC9000 with a range at 14EC9480), and every
  removal invalidated all native mesh entries, the found-range cache and with it every replay
  key: 4.2 -> 6.2 ms. Fixed by (1) native mesh entries keeping the generations of their own
  ranges and staying valid when only other ranges were removed, (2) the replay key looking up
  its vertex buffer itself, (3) vertex ranges marked dirty instead of removed.
- Cost of being right, A/B in one build: GPU command thread work in 1500+ draw frames
  4.80 / 4.67 ms on vs 4.26 / 4.27 ms off; the few ranges written every frame are requested
  (and their pages uploaded) each frame, ~8 draws fall back, and their pages fault again.

### 3a reconsidered: what the flush actually does (2026-11-07)

Read from the generated code before writing 3a:

- `sub_8258BD98` (float constants) and `sub_8258B9F8` (render state ranges, fetch range 0x2280,
  bool/loop constants) already write each run of dirty groups as one type-0 packet, the
  float constants with VMX copies of 64-byte groups (pad dwords 0x80000000 align them). The
  draw then writes `VGT_INDX_OFFSET` (0x2102) and a `DRAW_INDX` (with a predicated variant
  when dev+10940 bit 0 is set, and chunks of at most 65535 indices).
- The command processor already copies type-0 runs in bulk
  (`VulkanCommandProcessor::WriteRegistersFromMem`: one byte-swapping copy per run for float,
  bool/loop and fetch constants, plus the constant buffer invalidation).

So a packet carrying the same constants instead of the flush saves almost nothing on either
thread: the cost is the amount of state, which the draw needs anyway. The only form of 3a that
saves work sends deltas (per (record, pass), only the constant groups that changed since that
entry's last draw), but its state lives across packets, and the command buffers the game
replays (predicated tiling executes a buffer once per tile) would apply a delta out of order.
Not done; the per-draw cost of the command processor's native draw (~0.9 ms of the ~1.4 ms
ceiling: replay checks, constant uploads, pre-checks, pipeline state) is the larger target.

## Own shaders: where the GPU time goes (2026-11-07)

`--vulkan_gpu_timing_shaders` (with `--vulkan_gpu_timing`): a timestamp whenever the draws'
vertex/pixel shader pair changes, the time logged per pair of ucode hashes (top 24 per 5 s
window). Route at 1440p (scale 2x):

- Frames with 1500+ draws: ~6.0 ms GPU, but the command thread works ~7.3-7.7 ms, so they are
  CPU-bound; faster shaders would not show there. The most expensive pair is 0.27 ms
  (vs 281EBD690A5AC512 ps CD4D4F0F2DD1C86C, 15 draw runs a frame), then four at ~0.13-0.18 ms;
  the top 24 add up to ~2.7 ms, the rest is a long tail.
- All frames: ~3.7-5.2 ms GPU in the busy windows (mean ~250 fps, GPU-bound), of which
  **resolves ~0.7-1.4 ms** (20-36 a frame) and the swap's output copy 0.13 ms; no shader pair
  above 0.12 ms.

So no shader is worth replacing alone: writing the top ten by hand would take at most ~1 ms
off heavy frames (CPU-bound anyway) and less than 0.4 ms off the others. The GPU-side pieces
that are large are the resolves (EDRAM model; the direct paths and owning the main pass, 3c)
and, on the CPU side, the command processor's per-draw cost.

## Native EDRAM: design (2026-11-07)

Measured on a Poco F6 (Adreno 735): gameplay is GPU-bound at 720p, 27-36 ms of GPU a frame:
the main scene (D0 base 720 + C0 base 0, pitch 16) ~10.9 ms in ~10 render pass segments,
~48 resolves 5.6 ms, ~85 render pass begins a frame. On a tiled GPU every break stores and
reloads the framebuffer. The render passes end at texture loads in the middle of the scene,
at resolves, and at shared memory uploads (frame sequence of the skatepark route on PC).

Goal: the game's render targets become host images of our own size and lifetime, not EDRAM
ranges at an integer scale. Where the scale lives today (Vulkan only): ~150 uses of
`draw_resolution_scale_x/y` in the render target cache (image sizes, transfers, dumps,
clears), the command processor (viewport, scissor, system constants), the texture cache
(scaled resolve buffer, resolved texture sizes), `draw_util` (viewport, resolve info) and the
SPIR-V translator (pixel position), plus the precompiled resolve and transfer shaders, all
integer.

Steps, each testable on PC (route + screenshots) before the next:

1. **Per-render-target scale.** A render target key gets its own scale (n/16, so an 80x16
   EDRAM tile is still whole host pixels: 0.5 = 40x8, 0.75 = 60x12, 1.5 = 120x24). Only the
   main scene's key (1280x720, the most expensive) uses a scale other than the global one;
   draws into it get its viewport, scissor and pixel-position constants.
2. **Resolves and transfers out of it by blit.** Resolves from that target go through the
   direct-to-texture path with a scaled `vkCmdBlitImage`; transfers to or from targets with
   another scale are blits instead of the EDRAM sample-mapping shaders. Anything not covered
   falls back to rendering that target at the global scale (it is recreated).
3. **Resolved textures at the target's scale.** The scaled resolve buffer and texture keys
   carry the scale of the target they came from, so the post chain samples them at their own
   size.
4. **Fewer render pass breaks.** Texture loads found during the scene are done before the
   next render pass where possible (the loads of a submission's first draws are already known
   when it begins), and shared memory uploads of a frame are batched.

All four steps are done (2026-11-07, below): step 1, steps 2/3, then step 4 with the rest of
step 2 (the depth resolve). What is left on the twin path is the fallback the design allows:
transfers between render targets with different EDRAM layouts.

### Native EDRAM step 1: per-render-target scale (2026-11-07)

`--native_main_pass_scale_sixteenths=N` (hot-reloadable, GPU/Native, default 16 = off; 0 is
off too): the main scene's render targets are host images at N/16 of a guest pixel, 12 = 0.75
(960x540), 24 = 1.5 (1920x1080). Vulkan, host render targets path only; the rest of the
renderer keeps the global integer `draw_resolution_scale_x/y`.

How it works (SDK, `patches/rexglue-sdk.patch`):

- `RenderTargetKey` has a 6-bit `own_scale_sixteenths` (0 = global scale). A render target with
  its own scale is a different render target from the one with the same EDRAM layout at the
  global scale (its "unscaled twin"), so ownership, transfers and the update memo work as
  before; changing the cvar makes new keys (the command processor drops the update memo and the
  native replay entries when it changes).
- Which keys: `RenderTargetCache::IsMainPassKey`, color at EDRAM base 0 and depth at 720, pitch
  16 tiles (1280), 1x, 32bpp, measured on the skatepark route. A draw gets the scale only if all
  its bound render targets can have it (one framebuffer size); a resolve clear decides per key.
  `IsOwnScaleSupported` (Vulkan only, the D3D12 backend never scales) checks the size limits
  and that the format can be copied between scales.
- Draws into it: image and framebuffer sizes use the key's scale; the viewport is computed in
  guest pixels (`GetHostViewportInfo` with scale 1) and multiplied by N/16 in
  `UpdateDynamicState`, the scissor is rounded to host pixels, depth bias slope uses N/16, the
  point size system constant uses the guest-pixel viewport, and `ps_param_gen` (pixel position)
  gets a pixel shader modification `pixel_position_scale_sixteenths` (multiplied by 16/N; the
  scale is predicted from the registers before the update, `PredictDrawOwnScaleSixteenths`).
- Everything that reads or copies a render target with its own scale goes through its unscaled
  twin: resolves (direct-to-texture and through the EDRAM buffer dumps) first copy the source
  rectangle into the twin and read the twin; transfers from it read the twin; transfers into it
  are made into the twin (the usual EDRAM transfer shaders) and then copied into it; resolve
  clears clear the scaled rectangle. Color is copied with `vkCmdBlitImage` (linear). Depth /
  stencil by a small draw (fetch depth and stencil, write `FragDepth` and `FragStencilRefEXT`),
  because this AMD driver has no blit for D32_SFLOAT_S8_UINT; without
  VK_EXT_shader_stencil_export, depth is not scaled and so draws with depth stay at the global
  scale.

Route results (PC, 720p internal, 1080p window; screenshots at 30/38/46 s compared with a run
with the cvar off): at 12 and 24 the image is complete, no missing geometry, character outline
and post effects aligned. At 0.75 it is softer as expected; at 1.5 it is no sharper than 1x (a bit
softer, from the linear 1.5 -> 1 copy), because the post chain still gets the scene resolved to
1x (step 3). About 10 copies a heavy
frame between the scaled targets and their twins (color resolves, one depth copy, one copy
into the scaled color after a transfer).

GPU timing (`--vulkan_gpu_timing=true`, the two gameplay windows of the route, ms):

| N | main scene (D0 720 + C0 0) | full-screen C0 0 | resolves | all frames | 1500+ draw frames (GPU busy) |
|---|---|---|---|---|---|
| 16 (off) | 0.58 / 0.64 | 0.08 / 0.10 | 0.28 / 0.28 | 2.53 / 2.89 | 2.95 / 2.76 |
| 12 (0.75) | 0.49 / 0.54 | 0.06 / 0.07 | 0.31 / 0.32 | 2.55 / 2.85 | 2.90 / 2.72 |
| 24 (1.5) | 0.90 / 0.89 | 0.15 / 0.17 | 0.35 / 0.35 | 2.88 / 3.05 | 3.78 / 3.54 |

On this desktop GPU the main scene is ~0.6 ms of ~2.8 ms and not mostly fill-bound (0.56x the
pixels cost 0.84x the time), so 0.75 saves ~0.1 ms and the copies to the twins eat part of it
(resolves +0.03-0.04 ms). The phone, where the main scene is ~10.9 ms of 27-36 ms at 720p, is
where it should pay; not measured there yet (PC only for this step).

Limits and open points: memexport from pixel shaders into a scaled target is not handled (once
per guest pixel only holds at integer scales); the cvar was tested from the command line, not
changed during a run; the first route run of the session ended at 19 s with an access violation
inside ProvingGround.exe at startup (the unreproduced startup AV seen before, cvar off), every
later run was complete. Next: step 2/3, resolves straight from the scaled target into textures
at its scale, so 1.5 shows its detail and the twin copies go away.

### Native EDRAM steps 2/3: resolves at the target's own scale (2026-11-07)

With `native_main_pass_scale_sixteenths`, the main scene's 8888 resolves into textures the game
samples (the direct-to-texture ones, `gpu_resolve_direct_texture`) now go straight from the
scaled render target into a texture at the same scale, with no copy into the unscaled twin.
The post chain and the swap sample that texture, so 1.5 now shows its detail. Hot-reloadable
`--native_resolve_own_scale` (GPU/Native, default true) turns it off (back to the twin copy).

How it works (SDK):

- `TextureKey` has a 6-bit `own_scale_sixteenths` (a different texture from the guest-size
  one, host size N/16 of the guest size, in `CreateTexture` and `FindOrCreateTexture`'s
  size limits).
- The resolve (`VulkanRenderTargetCache::TryResolveDirectlyToTexture`, color, owner render
  target with its own scale, global scale 1) needs the guest-size texture learned as before
  (it may be outdated, it is not written), then copies the rectangle at the target's scale
  into the texture at that scale: `ResolveDirectlyToOwnScaleTexture` (`vkCmdCopyImage`), or,
  with the red/blue swap the main scene's resolves all have (`copy_dest_swap`),
  `ResolveThroughBufferToTexture` with the own scale (image to buffer, the swap compute pass,
  buffer to image). The range is then invalidated except for that texture, as the other direct
  resolves do, and `TextureCache::SetOwnScaleBase` records the address.
- `FindOrCreateTexture` for a guest key at such an address returns the texture at its own
  scale while it is current (not memoized, since only its current state decides). Once
  something else writes the range (a usual resolve, a CPU write) it is outdated, and the
  guest-size texture is used (loaded from memory as before) until the next resolve straight
  from the target. A change of address/scale bumps the lookup generation (memo and native
  replays) and resets the bindings. The swap scales its size by N/16. Textures at an own scale
  are never loaded from memory and never learned as direct-resolve targets.
- Depth resolves (the shadow maps are not scaled; the depth of the main scene is resolved once
  a frame into a float24 texture) still go through the twin.

Route results (PC, same route, screenshots at 30/38/46 s): at 24 the scene is clearly sharper
than at 16 (it was a bit softer before), colors right (the swap), character outline aligned; at
12 it is softer as expected. Per heavy frame at 24: 8 resolves at the own scale (07656000 x5,
085EC000 x2, 09268000, all 1280x720 with the swap), and 7 color copies + 2 depth copies left
between the scaled targets and their twins, all transfers when other render targets take the
EDRAM over (shadow map clears at base 0, the 1x1 pitch-1 target, a color target over the depth)
and the depth resolve.

GPU timing (`--vulkan_gpu_timing=true` only, the two gameplay windows, ms; same columns as
step 1):

| N | main scene | full-screen C0 0 | resolves | all frames | 1500+ draw frames (GPU busy) |
|---|---|---|---|---|---|
| 16 (off) | 0.59 / 0.61 | 0.08 / 0.10 | 0.28 / 0.27 | 2.53 / 2.73 | 2.94 / 2.80 |
| 12 (0.75) | 0.51 / 0.50 | 0.06 / 0.07 | 0.22 / 0.20 | 2.54 / 2.62 | 2.60 / 2.44 |
| 24 (1.5) | 0.94 / 0.93 | 0.16 / 0.18 | 0.51 / 0.48 | 3.09 / 3.19 | 4.13 / 3.78 |

At 0.75 the resolves are now cheaper than at 1x (no twin copy, fewer pixels): heavy frames
2.60/2.44 against 2.94/2.80 off (step 1: 2.90/2.72). At 1.5 the resolves cost more (the buffer
round trip of the swap at 2.25x the pixels) and the post effects sample bigger textures:
heavy frames 4.13/3.78 against 3.78/3.54 in step 1, the price of the detail. A swap done in a
copy shader instead of the buffer round trip would cut part of it.

Limits: depth resolves and resolves with a global draw resolution scale still go through the
twin; memory written only into the own-scale texture is stale for the CPU, as with the other
direct resolves (a load from memory over the range blocks the address). Next (step 4): fewer
render pass breaks (~40 a frame).

### Direct resolves: lazy write-back to shared memory (2026-11-11)

Before, a direct resolve (`gpu_resolve_direct_texture`, also at the target's own scale) marked
its pages valid and GPU-written in shared memory although only the texture image was written.
Anything else reading the range (another texture key over the memory, the unscaled fallback of
an own-scale texture, vertex fetch, memexport, a resolve readback) got the bytes from before,
and a texture load over the range blocked the address for good ("resolving it the usual way").
Now the data is given to shared memory when it is first needed, and the address stays direct:

- `VulkanTextureCache` tracks each direct-resolved range with the texture holding it
  (`direct_resolve_ranges_`: extent, base, texture, the resolve rectangle).
- `SharedMemory` has "deferred GPU writes" (`SetDeferredGpuWriteRanges`): one bit per page
  with such data and the bounds of the set bits. `RequestRanges` checks the bounds (two
  compares per range in the usual case), then the bits, and calls the texture cache before it
  hands the pages out. Everything that reads shared memory requests it first, so this covers
  texture loads, vertex and index buffers, memexport and the destination of a usual resolve.
  The widening of uploads skips pages with deferred data.
- The write-back (`WriteBackDirectResolve`): the image is copied into the scratch buffer, then
  a compute pass (GLSL compiled at first use) writes guest memory: tiled addresses, the
  texture's endianness, 8888 words, or the depth word from the float32 the texture holds
  (float24 for k_24_8_FLOAT, unorm24 for k_24_8; the stencil byte is 0). A texture at its own
  scale is brought to guest size like the copy into the unscaled twin (linear for color,
  nearest for depth). With a draw resolution scale the destination is the scaled resolve
  buffer in its layout, asked for by texture loads and resolves instead of by shared memory.
  The texture stays current, and the range is an ordinary resolved range afterwards.
- A CPU write to a page of the range outdates the texture as before. The tracking stays until
  the next reader: the image is written back first, then the CPU's pages are uploaded over it
  (kept in order in the submission, `PrepareDeferredGpuWrite`), so a partial CPU write is
  right too.
- A resolve of the same range replaces the tracked data without a write-back. A range not
  resolved into for 600 frames is written back at the next direct resolve, so its texture can
  be dropped; until then the texture is kept alive.
- `vulkan_readback_resolve`: a direct resolve is written back at once and read back like a
  usual one.
- Safety net only: if a write-back cannot be done (no scratch buffer or descriptor), the range
  is invalidated in shared memory (the CPU's copy is used), a warning is logged and the
  address is resolved the usual way from then on. The same invalidation, with a warning,
  happens if the texture is destroyed while tracked. Without the write-back pipeline no
  resolve is done directly.

No new cvars. In the common case (only the texture itself samples the range) nothing is
written back: per direct resolve there is one more `RequestRange` of pages that are already
valid. With `--vulkan_gpu_timing` sequence logging, each write-back is listed as
"direct resolve ... written back to memory". Not verified in-game yet (written without a
build): see the test steps in the change's report.

### Native EDRAM step 4 and the rest of step 2 (2026-11-07)

**Texture loads before the render pass** (`--native_hoist_texture_loads`, GPU/Native, default
true, hot-reloadable). A texture load met inside a render pass (a compute dispatch, which used
to end the pass) is recorded on its own and spliced into the command stream just before that
render pass began (`VulkanCommandProcessor::BeginHoistBeforeRenderPass` /
`EndHoistBeforeRenderPass`, `DeferredCommandBuffer::InsertAt`; the barriers, the compute
pipeline binding and `deferred_command_buffer()` go to the side stream meanwhile). It is safe
because inside an unbroken render pass nothing has written shared memory (an upload or a GPU
write ends the pass with its barrier), checked as: render pass open, no barriers pending,
shared memory in plain read use (`VulkanSharedMemory::IsReadWithoutBarrier`), the texture not
sampled by a draw of this render pass (`VulkanTexture::sampled_render_pass_serial`) and not a
scaled-resolve texture. The texture is also moved to the sampled layout there, so the draw needs
no barrier. Shared memory uploads were already batched at the submission start
(`shared_memory_upload_at_submission_start`: 650-800 KB a heavy frame at the start, 30-50 KB in
order).

**Depth resolve straight into textures.** The main scene's depth is kD24S8 (unorm24, host
D32_SFLOAT_S8_UINT on this AMD GPU) and is sampled as `k_24_8` (R32_SFLOAT, the unorm24 of the
word), so it never took the direct path, which only knew float24 depth into `k_24_8_FLOAT`. The
buffer pass of `ResolveThroughBufferToTexture` now has modes for float24 depth read as
`k_24_8`, unorm24 depth kept as float32 (rounded to 24 bits) and D24_UNORM words; `k_24_8`
textures are learned as direct targets. With `native_main_pass_scale_sixteenths` the depth
resolve goes into a texture at the target's scale like the color ones; at 1x it is a direct
resolve now too (no shared memory write and texture load each frame).

Per heavy frame at 24 the scaled targets now resolve straight into textures (8 color, 1
depth); left on the twin path: 7 color copies and 1 depth copy, all transfers between render
targets with other EDRAM layouts (the shadow-map pass over EDRAM 0/720, a depth-only clear
target over the color, the 1x1 pitch-1 target), which need the EDRAM sample-mapping shaders.

Route (PC, `--vulkan_gpu_timing=true` only, the two gameplay windows, ms; render pass begins
per 1500+ draw frame / of them ended by image barriers / by buffer-only barriers):

| run | all frames | 1500+ draw frames (GPU busy) | resolves | render passes |
|---|---|---|---|---|
| 16, steps 2/3 | 2.53 / 2.73 | 2.94 / 2.80 | 0.28 / 0.27 | 38.4 / 25.8 / 5.1, 38.6 / 25.6 / 6.6 |
| 16, hoist off | 2.39 / 2.56 | 2.78 / 2.63 | 0.28 / 0.26 | 40.9 / 28.0 / 4.8, 38.2 / 25.5 / 6.7 |
| 16 | 2.42 / 2.59 | 2.78 / 2.62 | 0.27 / 0.26 | 37.6 / 27.1 / 2.6, 34.8 / 25.5 / 3.2 |
| 12, steps 2/3 | 2.54 / 2.62 | 2.60 / 2.44 | 0.22 / 0.20 | 40.3 / 28.8 / 4.9, 36.4 / 26.2 / 5.4 |
| 12 | 2.39 / 2.62 | 2.41 / 2.28 | 0.20 / 0.19 | 38.1 / 28.8 / 2.5, 36.0 / 27.9 / 2.9 |
| 24, steps 2/3 | 3.09 / 3.19 | 4.13 / 3.78 | 0.51 / 0.48 | 42.6 / 30.1 / 5.4, 40.5 / 29.4 / 5.9 |
| 24 | 2.79 / 3.02 | 4.01 / 3.70 | 0.48 / 0.47 | 38.7 / 28.8 / 3.0, 38.2 / 29.1 / 3.5 |

The hoisting takes 2-4 render pass breaks a heavy frame (about half of the buffer-only ones);
on this desktop GPU that costs nothing measurable (16: 2.78/2.63 off, 2.78/2.62 on), the
gain is for tiled GPUs (each break stores and reloads the framebuffer), not measured on the
phone (PC only). The direct depth resolve and steps 2/3 together: 1x heavy frames
2.94/2.80 -> 2.78/2.62, 0.75 2.41/2.28, 1.5 4.01/3.70. The frame sequence log showed 121 of
152 texture loads hoisted in two heavy frames. Screenshots at 30/38/46 s at 16 and 24: complete,
depth effects (fog on the buildings, outlines) as before.

What stays: the ~29 image-barrier breaks a heavy frame are the game's own resolves and
render target switches (one pass per post effect, each sampling the previous one), not
removable without merging passes; one run of this batch ended at ~20 s at startup with the
known guest access violation (read of guest 0x00000008, cvar 16), the rerun was complete.

## Temporal upscaling: FSR 2/3 with real inputs (2026-11-11)

Not run in game yet: written and checked without the game (syntax on Linux and for Windows, the
shaders through glslang, the jitter and motion vector math in a standalone test). The test steps
are at the end.

**Before.** The presenter's `fsr2`/`fsr3` effects ran the FidelityFX upscaler while painting, on
the finished image, with `reset = true`, no jitter, the color image passed as depth and as motion
vectors, and a fixed 16.666 ms: a spatial upscale at the cost of the temporal pass chain. It also
ran once per paint, not once per guest frame, so its history could not have been right even with
inputs.

**Now (Vulkan).** The upscaler runs in the swap (`VulkanCommandProcessor::IssueSwap`), once per
guest frame, and the presenter only places the result:

- **Sizes.** The presenter publishes the size it paints the guest output at
  (`VulkanPresenter::GetTemporalUpscaleTarget`, set while painting when the effect is `fsr2` or
  `fsr3`). The swap refreshes the guest output at that size. The render size is the front
  buffer's, which with `resolution_scale` 1 is the 3D scene's own scale
  (`native_main_pass_scale_sixteenths`): the options menu sets both when FSR is chosen. A new
  output size (a window being resized) is taken after 20 swaps, since each needs a new upscaler.
- **Jitter.** Halton(2,3) in -0.5..0.5 of a render pixel, the period FSR recommends
  (8 x area scale), a new offset each swap. It is added to the host viewport
  (`UpdateDynamicState`) of the draws `IsTemporalJitterDraw` accepts: depth test on, the main
  scene's depth buffer bound (`RenderTargetCache::IsMainPassKey`), positions in clip space
  through the viewport. Full-screen passes and the HUD have no depth test and stay put; they
  read the jittered scene through textures. The viewport moves right and down by the offset,
  which is FSR's convention (projection translated by 2j/size, -2j/size for Y up).
- **Depth.** The main pass's host depth render target (the one the camera was recorded on),
  copied to R32F by the inputs compute pass. 0 near, far plane at infinity
  (`FFX_UPSCALE_ENABLE_DEPTH_INFINITE`), as frame generation already assumed.
- **Motion vectors.** Camera reprojection, the frame generation design: the world and
  world-view-projection constants of the main pass's first draw give the view-projection
  (`fsr3_camera_world_constant` 216, `fsr3_camera_world_view_projection_constant` 224, set in
  `pg_app.h`); each pixel's depth is unprojected with this frame's matrix and projected with the
  previous one. The pixel's position has the jitter taken off first, so the vectors have none.
  Things that move on their own (the skater, other skaters, cars) get the camera's motion only;
  FSR's own disocclusion and luminance checks have to cope, there is no per-object mask.
  `UpdateFrameGenerationCamera` now also scales the viewport of render targets with their own
  scale to host pixels (it was in guest pixels, which made the frame generation vectors wrong
  with `native_main_pass_scale_sixteenths` other than 16).
- **HUD.** The game draws the HUD into the same EDRAM target as the scene (color at tile 0), so
  the final image has it. Upscaling before the HUD would need that target to change scale in
  the middle of the frame (the scene and post chain at render size, the HUD at output size), with
  ownership transfers both ways every frame and the upscaler's output placed in an own-scale
  texture of another scale than its render target: not attempted blind. Instead: the last
  resolve of the same target before the front buffer's (the frame generation "hudless" address)
  is compared with the final image; where they differ by more than a threshold (3x3 widened) is
  the HUD mask. The mask goes to FSR as reactive and transparency-and-composition mask, and
  after FSR a compute pass puts the bilinearly scaled final image over the output where the mask
  is set (the HUD is not jittered, so through FSR it would wobble by the jitter).
- **Reset.** On the first frame, a new upscaler or size, a frame without a camera (menus,
  loading, videos: constant depth, zero vectors, reset every frame), more than 500 ms since the
  last swap, a missed refresh, and a camera cut: far points or a near point reprojecting more
  than `fsr_temporal_camera_cut` of the width away, or behind the camera.
- **Frame time** is the time between swaps.
- **Recording.** FidelityFX records into a real `VkCommandBuffer`, the command processor into
  its deferred stream: `DeferredCommandBuffer::CmdExternal` calls
  `VulkanPresenter::RecordTemporalUpscale` when the stream is executed (on the submission thread
  with `vulkan_async_submission`). The upscaler is created by the command processor thread after
  awaiting every earlier submission, so the two never overlap. If the dispatch fails there, a
  bilinear blit writes the output instead.
- **Fallbacks.** No FidelityFX runtime, creation failed, render size above the output size, the
  3D scene not at the final image's size (`resolution_scale` above 1 with another own scale),
  `fsr_temporal_upscaling=false`: the guest output stays at render size and the presenter paints
  it with FSR 1 (EASU + RCAS), one log line. FSR 1 itself (`present_effect=fsr`) is unchanged.
- **Direct3D 12.** Not done: that renderer records no camera, has no own-scale main pass and no
  compute passes for the inputs (they would need DXBC built with the Windows SDK). Its `fsr2`/
  `fsr3` now scale with FSR 1 (`d3d12_fsr_temporal_without_history=true` brings back the old
  per-paint dispatch, now with the real frame time), and the options menu offers FSR with Vulkan
  only.

Cost per frame on top of the scene at render size: the gamma pass (as before, now into the
color input), one render-size compute pass for depth and vectors, one for the mask, FSR itself,
and an output-size compute pass that exits early outside the HUD. No full-size copies: FSR
writes the guest output image directly.

Cvars (GPU/Vulkan unless noted): `fsr_temporal_upscaling` (true), `fsr_temporal_jitter` (1; 0
off, 2 only draws using the camera constants), `fsr_temporal_hud_mask` (true),
`fsr_temporal_hud_mask_threshold` (0.1), `fsr_temporal_camera_cut` (0.35),
`fsr_temporal_debug_view` (false); UI/Presenter: `fsr_temporal_depth_infinite` (true),
`fsr_temporal_depth_inverted` (false), `fsr_temporal_view_space_to_meters` (1.0); UI/D3D12:
`d3d12_fsr_temporal_without_history` (false).

To check in game (Vulkan, a 1080p or larger screen):

1. Anti-aliasing: AMD FSR 3.1, FSR quality: Quality; restart if the note about Effects and HUD
   shows. The log has "FidelityFX temporal upscaler ... created for 1280x720 to 1920x1080" and
   no "Temporal upscaling is not used".
2. Stand still: edges (rails, wires, fences) settle within a few frames and stay; no shimmer.
   Compare with Anti-aliasing Off + Sharpness 50% (FSR 1) at the same Resolution.
3. Skate and turn the camera: no trails behind the skater, no smearing of the ground; a cut
   (bail camera, replay, menu to game) shows no ghost of the previous view.
4. HUD (score, special meter, trick text): sharp, steady, no trail when the camera moves.
   If the whole image shimmers instead, the image before the HUD is not what the mask assumes:
   try `--fsr_temporal_hud_mask=false` (the HUD may then trail) or raise
   `--fsr_temporal_hud_mask_threshold`.
5. `--fsr_temporal_debug_view=true` shows FSR's view of the inputs: motion vectors should be
   zero when standing still and follow the camera when turning; depth should show the scene.
   Vectors pointing the wrong way or doubled mean a sign or scale error
   (`fsr_temporal_jitter=0` separates jitter errors from vector errors: the image is then
   steady but no sharper than FSR 1).
6. Post effects aligned (bloom, the character outline, depth fog): a doubled or blurred outline
   means a depth-tested full-screen pass is being jittered; `--fsr_temporal_jitter=2` limits
   the jitter to draws that use the camera constants.
7. Menus, loading screens and videos look as with FSR 1 (they are reset every frame).
8. `--vulkan_gpu_timing=true`: frame GPU time at Quality against the same Resolution with FSR 1,
   and against Native AA.
9. Resize the window: the image stretches for about 20 frames, then is rebuilt at the new size.
