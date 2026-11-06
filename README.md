# render3d

A multithreaded **software 3D rasterizer** written in C11. It renders triangle meshes entirely on the CPU into a
`uint32_t` framebuffer, using a tile-based pipeline (vertex stage, binning, rasterization) that scales across all
CPU cores. [raylib](https://www.raylib.com/) is used only as a thin presentation layer: the framebuffer is uploaded
to a `Texture2D` and drawn once per frame.

Features:

- Perspective / translation / rotation matrix helpers (4x4, row-major)
- Near-plane clipping (Sutherland-Hodgman), frustum culling, optional backface culling
- Tile-based binning with a **parallel prefix sum**
- Exact fixed-point (sub-pixel) triangle rasterization with a top-left style tie-break and a depth buffer
- Wavefront `.obj` / `.mtl` loader (flat `Kd` colors, polygons triangulated as fans)
- Persistent thread pool with custom barriers (works on macOS, which has no `pthread_barrier_t`)

> The code was developed and tested on **macOS**. The Linux and Windows (MSYS2) instructions below follow the
> same dependencies and the code has portability shims for them, but they have not been tested on those systems.

---

## Table of contents

1. [Building](#building)
2. [Running](#running)
3. [Project structure](#project-structure)
4. [Public API](#public-api)
5. [Conventions](#conventions)
6. [The rendering pipeline](#the-rendering-pipeline)
7. [Parallelism and memory optimizations](#parallelism-and-memory-optimizations)
8. [Limitations](#limitations)

---

## Building

Dependencies: a C11 compiler with `<stdatomic.h>` and pthreads, `pkg-config`, and raylib (5.x).

### macOS

```sh
brew install raylib pkg-config
cc -O3 -Wall -Wextra -o render3d main.c render_raylib.c render.c render_obj.c \
   $(pkg-config --cflags --libs raylib) -framework IOKit -framework Cocoa -framework OpenGL
```

### Linux (Debian / Ubuntu shown)

```sh
sudo apt install build-essential pkg-config libraylib-dev   # or build raylib from source
gcc -O3 -Wall -Wextra -o render3d main.c render_raylib.c render.c render_obj.c \
    $(pkg-config --cflags --libs raylib) -lm -lpthread
```

If your distribution does not package raylib, build it from source
(<https://github.com/raysan5/raylib>) and make sure `pkg-config --libs raylib` works. On Fedora / Arch install the
equivalent `raylib-devel` / `raylib` packages.

### Windows (MSYS2)

Open the **MSYS2 UCRT64** shell and install the toolchain and raylib:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-raylib
gcc -O3 -Wall -Wextra -o render3d.exe main.c render_raylib.c render.c render_obj.c \
    $(pkg-config --cflags --libs raylib) -lpthread
```

(Use the `mingw-w64-x86_64-*` packages instead if you work in the MINGW64 shell.) The raylib `pkg-config` file already
adds `-lopengl32 -lgdi32 -lwinmm`. Run the executable from the same shell, or copy the required MSYS2 DLLs next to it.

---

## Running

```sh
./render3d "path/to/model/folder"
```

The argument is a **folder** containing one `.obj` file (and the `.mtl` it references). Without an argument, `main.c`
falls back to the `BRIDGE_DIR` macro, which points to the author's local copy of the *Historic Boardman Bridge* model
— change it or always pass a folder.

`main.c` loads the model, normalizes it to fit in `[-1, 1]`, enables backface culling, and spins the model around the
Y axis while recomputing `MVP = P * V * M` every frame.

---

## Project structure

| File | Purpose |
|------|---------|
| `render.h` | Public API: math types, `Triangle`, renderer control functions, matrix helpers, OBJ loader declarations. Has no raylib dependency. |
| `render.c` | The renderer: global state, barriers, matrix helpers, the four pipeline stages, thread pool. |
| `render_obj.c` | Wavefront OBJ/MTL loader and `triangles_normalize`. |
| `render_raylib.h` / `render_raylib.c` | Presentation layer: opens the window, uploads the framebuffer to a `Texture2D`, draws it. |
| `main.c` | Demo: loads a model from a folder and spins it. |

The renderer (`render.*`) knows nothing about raylib; any other front end can call `render()` and read
`get_framebuffer()`.

---

## Public API

### Renderer (`render.h`)

| Function | Description |
|----------|-------------|
| `void init(width, height, triangle_count)` | Allocates all buffers and starts one worker thread per logical CPU. |
| `void close_render(void)` | Stops and joins the workers and frees everything. (Named so because `close` clashes with `<unistd.h>`.) |
| `void set_triangles(Triangle*)` | Sets the mesh (read-only; at most `triangle_count` triangles). |
| `void set_matrix(float*)` | Sets the 4x4 row-major matrix applied to every vertex (typically `MVP`). The pointer is read at every frame, so you can update the contents in place. |
| `void set_backface_culling(bool)` | Enables/disables backface culling (default: off). |
| `void render(void)` | Renders one frame and blocks until it is finished. Call from one thread only. |
| `const uint32_t* get_framebuffer(void)` | Pixels, `width * height`, `0xAABBGGRR` (bytes R,G,B,A in memory). |

### Matrices (row-major, `m[row * 4 + col]`)

`mat4_identity`, `mat4_mul(out, a, b)` (`out = a * b`, aliasing allowed), `mat4_perspective(m, fov_y, aspect, z_near, z_far)`,
`mat4_translation(m, x, y, z)`, `mat4_rotation_x/y/z(m, angle)`.

### Models

- `uint32_t load_obj_folder(dir, Triangle** out)` — loads the first `.obj` found in `dir`, resolves `mtllib`/`usemtl`,
  uses `Kd` as the face color, and applies a fixed directional shade per face (the OBJ only has flat colors). Returns
  the triangle count (0 on failure); release `*out` with `free()`.
- `void triangles_normalize(tris, count, radius)` — centers the mesh at the origin and scales it to `[-radius, radius]`.

### raylib layer (`render_raylib.h`)

`rr_init(width, height, triangle_count, title)`, `rr_render_and_draw()` (call between `BeginDrawing()`/`EndDrawing()`),
`rr_close()`.

---

## Conventions

- **Matrices**: 4x4, row-major, column-vector convention (`v' = M * v`). Translation lives in `m[3], m[7], m[11]`.
- **Camera**: right-handed, looks down **-Z**. NDC depth range is `[-1, 1]` (OpenGL style).
- **Screen space**: origin at the top-left, `y` grows downwards. Pixel centers are sampled at `(x + 0.5, y + 0.5)`.
- **Winding**: counter-clockwise triangles are front-facing (as in OBJ). Backface culling is off by default because
  not every mesh has consistent winding.
- **Color**: `uint32_t` `0xAABBGGRR`, which matches raylib's `PIXELFORMAT_UNCOMPRESSED_R8G8B8A8` on little-endian CPUs.
- **Depth**: smaller is closer; the depth buffer is cleared to `FLT_MAX`.

---

## The rendering pipeline

Each frame runs four stages. All worker threads run every stage; stages are separated by barriers.

```
render()  ──►  clear_buffers ─► vertex_shader ─► binning ─► rasterize  ──►  framebuffer
                    │               │              │            │
                 barrier         barrier        barrier      (frame barrier)
```

### 1. `clear_buffers`

Zeroes the tile offset/temp arrays, the framebuffer and the depth buffer (`FLT_MAX`), and resets the tile work queue
and the extra-slot counter (`extra_count`). Each thread clears its own contiguous chunk.

### 2. `vertex_shader`

Per triangle (each thread handles a contiguous block of the mesh):

1. `transform_vec` multiplies each vertex by `states.matrix` (row-major 4x4) giving clip-space `vec4`.
2. **Frustum culling**: a 6-bit *outcode* per vertex records which frustum planes (`-w <= x,y,z <= w`) it violates. If
   the AND of the three outcodes is non-zero, all three vertices are outside the same plane and the triangle is dropped.
3. **Near-plane clipping**: Sutherland-Hodgman against `z + w >= 0`. A triangle becomes 0, 1 or 2 triangles (a clipped
   quad is split as a fan). This is what makes geometry crossing the camera render correctly instead of vanishing.
4. `project_vec` performs the perspective divide and the viewport transform → pixel `x/y` and NDC `z`.
5. `setup_triangle` builds the `TransformedTriangle` and its pixel-space bounding box (clamped to the screen). It
   returns `false` — and the triangle is simply dropped — if `is_visible` rejects it (non-finite or degenerate, entirely
   beyond the far plane, or back-facing when backface culling is enabled; sign of the screen-space area) or if the box
   is empty.
6. `store_triangle` writes the triangle into its slot and counts, per tile, how many triangles touch it.

Visible triangles are stored **compactly**: thread `t` writes them one after another starting at the beginning of its own
block, and records how many in `thread_valid[t]`. A rejected triangle leaves no trace, so no slot ever has to be
"cleared" or marked empty.

### 3. `binning`

A counting sort of triangles by tile (a CSR-style layout):

- **`tile_prefix_sum`** converts the per-tile counts into offsets (see the next section). Afterwards
  `tile_triangle_offset[t] .. tile_triangle_offset[t+1]` is the range of tile `t` in `tile_triangle_indices`, and
  `tile_triangle_offset_temp[t]` equals `offset[t+1]`.
- **Scatter**: every thread walks its triangles; for every tile a triangle overlaps it does
  `idx = --temp[tile]; indices[idx] = triangle_id`. Because `temp[t]` starts at the *end* of tile `t`'s range and is only
  ever decremented, each triangle gets a unique slot and the temp array ends up equal to `offset`.

### 4. `rasterize`

Threads repeatedly grab the next tile from a shared atomic counter. For each triangle in the tile (`rasterize_triangle`):

- The bounding box is intersected with the tile.
- Vertices are snapped to a fixed-point grid (8 sub-pixel bits) and the three **edge functions** are evaluated with
  `int64` arithmetic, so they are exact; they are then stepped incrementally across x and y.
- A pixel is covered if all three edge values are `>= 0`, with a one-unit bias on edges that the triangle does not
  "own" (a direction-based tie-break). Two triangles that share an edge traverse it in opposite directions, so exactly
  one of them claims the pixels on it: **no cracks and no double-drawn pixels**.
- **Depth interpolation**: the edge values are the (unnormalized) barycentric coordinates, so
  `z = (w0*z0 + w1*z1 + w2*z2) / area`. NDC `z/w` is affine in screen space, so this linear interpolation is correct
  for perspective projections. Pixels with `z > 1` (past the far plane) are discarded; the rest are depth-tested and
  written to the framebuffer and depth buffer.

---

## Parallelism and memory optimizations

### Thread pool and frame lifecycle

`init()` creates `N` worker threads (one per logical CPU: `sysconf(_SC_NPROCESSORS_ONLN)`, or `GetSystemInfo` on
Windows). They live for the whole program, so no thread is created or destroyed per frame. Each worker runs
`pipeline()`:

```
loop:
    frame_barrier (N+1 participants)      // park until render() is called
    if (!running) break
    clear → barrier → vertex → barrier → binning → barrier → rasterize
    frame_barrier (N+1 participants)      // tell render() the frame is done
```

`render()` is simply "enter the frame barrier twice" on the main thread: the first wait releases the workers, the second
one returns when every worker has finished. The mesh and the matrix are therefore only touched by the workers between
those two barriers; the application can safely modify them in between frames (this is why `main.c` updates `MVP` right
before `render()`).

Shutdown works the same way: `close_render()` sets `running = false` *while the workers are parked at the frame barrier*
and then enters it, so every worker sees the same value and exits together. (If threads sampled the flag at different
moments, some could leave while others waited forever at a barrier.)

### Custom barriers

macOS does not implement `pthread_barrier_t`, so the project has its own barrier built on C11 atomics: an arrival counter
and a *generation* number. Each thread reads the generation, atomically increments the counter, and:

- if it is the last to arrive, it resets the counter and increments the generation, releasing everybody;
- otherwise it spins (`sched_yield()` in the loop) until the generation changes.

Reading the generation *before* arriving makes the barrier reusable without ABA problems: the generation cannot change
until every participant, including the reader, has arrived. Two instances exist — `worker_barrier` (N threads, used
between stages) and `frame_barrier` (N + 1, includes the main thread). Sequentially-consistent atomics make a barrier a
full synchronization point, so everything written before it is visible after it, which is what lets the stages use plain
(non-atomic) loads and stores for data produced in an earlier stage.

### Work distribution

- **Uniform work (clear, vertex stage, scatter, prefix sum)** uses *static partitioning*: thread `t` processes
  `[t * ceil(n/N), (t+1) * ceil(n/N))` clamped to `n`. Contiguous chunks give sequential memory access and need no
  coordination.
- **Non-uniform work (rasterization)** uses *dynamic scheduling*: a shared `atomic current_tile` counter is incremented
  with `fetch_add` and each value is a tile to process. Tiles near a big object cost far more than empty ones, so
  threads that finish early simply take more tiles — automatic load balancing with one atomic operation per tile.

### Tiles and why rasterization needs no locks

The screen is divided into `32 x 32` tiles (`TILE_SIZE = 1 << TILE_SHIFT`). A tile is owned by exactly one thread at a
time, so the framebuffer and the depth buffer are written **without atomics or locks** and the depth test is a plain
compare. A tile's color (32*32*4 = 4 KiB) and depth (4 KiB) data fit comfortably in the L1 cache, so overdraw inside a
tile is cheap even for models with many overlapping triangles.

Because `TILE_SIZE` is a power of two, tile arithmetic uses bit operations instead of division/multiplication:
`x / TILE_SIZE → x >> TILE_SHIFT`, `tile * TILE_SIZE → tile << TILE_SHIFT`, and `ceil(v / TILE_SIZE) → (v + TILE_MASK) >> TILE_SHIFT`
with `TILE_MASK = TILE_SIZE - 1`.

### Parallel prefix sum (binning)

The per-tile triangle counts must become offsets before triangles can be scattered. The scan is a classic
three-phase *blocked* algorithm, where the tile array is split into one block per thread:

1. **Block sums**: each thread sums the counts of its block into `scan_partial[t_id]`. → *barrier*
2. **Block scan**: each thread computes its starting value as the sum of `scan_partial[0 .. t_id-1]` (at most N values — a
   trivial serial loop) and then does an inclusive scan of its block into `tile_triangle_offset_temp`. In the same phase
   thread 0 adds up all partial sums to get the total number of (triangle, tile) pairs and grows
   `tile_triangle_indices` if necessary. → *barrier*
3. **Offsets**: each thread writes `offset[i + 1] = temp[i]` for its block (and `offset[0] = 0`). → *barrier*

After phase 3 both arrays contain the prefix sum: `offset` is the *exclusive* form (start of every tile, with a sentinel at
index `tile_count`), `temp` is the *inclusive* form (end of every tile), used as a decrementing write cursor during the
scatter. The third barrier is required because the scatter modifies `temp`, which phase 3 is still reading. The work is
`O(tiles / N + N)` per thread.

### Memory optimizations

- **Per-thread tile counters.** In the vertex stage a triangle increments one counter for each tile it touches. Doing
  that with an atomic on a shared array makes all threads fight over the same cache lines. Instead every thread
  increments its private `local_counts[t_id][tile]` (plain integers; `threads * tiles * 4` bytes, ~120 KiB for 16 threads
  at 1600x1200) and, at the end of the stage, adds each non-zero entry into the shared array with one atomic add — that is
  `O(tiles)` atomics per thread instead of `O(triangles x tiles touched)`. The local array is reset while flushing, so it
  needs no separate clear.
- **Counting sort instead of per-tile lists.** All tile → triangle lists live in one flat `uint32_t` array addressed
  through the offset array, rather than in `tiles x triangle_count` slots or linked lists. This is compact, cache-friendly
  to read during rasterization (a tile's triangle ids are contiguous), and needs no allocation per frame.
- **Growable index buffer.** The worst case for the index array (`tiles x triangles`) is enormous, so it starts small and is
  grown with `realloc` by thread 0 inside the prefix-sum phase, when the exact total is known and every other thread is
  waiting at a barrier.
- **Compact triangle storage, no stale data.** Each thread writes its visible triangles contiguously from the start of
  its own block (`thread_block()` gives the same block to the vertex stage and to binning) and publishes the count in
  `thread_valid[t]`. Binning of that thread visits only `[start, start + thread_valid[t])`, so it never reads a slot that
  was not written this frame. Culled triangles cost nothing — no write, and no "empty" marker to store or test — and
  binning does not iterate over invisible triangles at all.
- **Overflow slots for clipping.** A triangle clipped against the near plane may become two triangles, but that is rare.
  The first goes into the thread's block; the second goes into a separate "extra" area after the first
  `triangle_count` slots, claimed with an atomic counter *only after* the triangle has been validated (so the area has no
  holes either). Binning visits `[0, extra_count)` of that area, split evenly between threads.
- **Fixed-point rasterization.** Integer edge functions are exact and cheap to step incrementally (two additions per
  pixel per edge) and avoid the float accumulation errors that cause cracks between triangles.
- **Relaxed atomics where ordering is provided by barriers.** Counter updates inside a stage use
  `memory_order_relaxed` because the following barrier (sequentially consistent) publishes the results; only the barrier and
  the tile queue need stronger ordering.
- **Early culling.** Frustum and backface culling remove triangles before they reach binning and rasterization, which are
  the stages whose cost scales with the number of covered tiles and pixels.
- **Deterministic output.** The scatter order inside a tile depends on thread timing, but the depth test makes the
  image independent of it, except for exact depth ties between different triangles.

---

## Limitations

- Only the near plane is geometrically clipped. Screen edges are handled by the bounding-box clamp, and the far plane by
  a per-pixel depth check.
- Single global renderer state: one renderer instance per process.
- Flat per-face color, no textures, no per-pixel lighting (the OBJ loader bakes a fixed directional shade into the color).
- Workers wait on barriers by spinning with `sched_yield()`, which keeps latency low but burns CPU while waiting.
- `set_triangles` / `set_matrix` data must not be modified while `render()` is running.
- The mesh must contain at most `triangle_count` triangles passed to `init()`.
