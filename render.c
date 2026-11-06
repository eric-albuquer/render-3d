#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>
#include <float.h>

#include "render.h"

#define TILE_SHIFT 5
#define TILE_SIZE (1u << TILE_SHIFT)
#define TILE_MASK (TILE_SIZE - 1u)

// v0..v2: x/y in pixels, z in NDC [-1, 1]
// min/max: bounding box in pixels, max exclusive. Discarded triangle => empty box
typedef struct {
    vec3 v0, v1, v2;
    uint32_t color;
    uint32_t min_x, min_y, max_x, max_y;
} TransformedTriangle;

typedef struct {
    float* matrix; // 4x4, row-major: m[row * 4 + col]

    uint32_t width;
    uint32_t height;

    uint32_t* framebuffer;
    float* zbuffer;

    uint32_t triangle_count;

    Triangle* raw_triangles;
    TransformedTriangle* transformed_triangles;

    uint32_t tile_rows;
    uint32_t tile_cols;
    uint32_t tile_count; // tile_rows * tile_cols

    // offset: tile_count + 1 entries, offset[t]..offset[t+1] = range of tile t (exclusive prefix sum)
    // temp:   tile_count + 1 entries, temp[t] = offset[t+1] (decremented during binning)
    atomic_uint_fast32_t* tile_triangle_offset;
    atomic_uint_fast32_t* tile_triangle_offset_temp;
    uint32_t* tile_triangle_indices;
    uint32_t tile_triangle_capacity;
    uint32_t* local_counts; // thread_count * tile_count: per-thread counts (avoids one atomic per triangle)
    atomic_uint_fast32_t extra_count; // extra slots (2nd triangle produced by clipping) used this frame
    uint32_t* thread_valid; // per-thread number of visible triangles written by vertex_shader
    uint32_t* scan_partial; // per-thread partial sum (parallel prefix sum)

    atomic_uint_fast32_t current_tile;

    atomic_bool running;
    atomic_bool cull_backface; // discards back-facing triangles (clockwise on screen)

    uint32_t thread_count;
    uint32_t* thread_ids;
    pthread_t* threads;
} RenderStates;

RenderStates states;

/* ---------- barriers (pthread_barrier_t does not exist on macOS) ---------- */

typedef struct {
    atomic_uint count;
    atomic_uint generation;
} Barrier;

static Barrier worker_barrier; // worker threads only
static Barrier frame_barrier;  // worker threads + main thread

static void barrier_wait(Barrier* b, uint32_t n) {
    uint32_t gen = atomic_load(&b->generation);
    if (atomic_fetch_add(&b->count, 1) + 1 == n) {
        atomic_store(&b->count, 0);
        atomic_fetch_add(&b->generation, 1);
    } else {
        while (atomic_load(&b->generation) == gen)
            sched_yield();
    }
}

void barrier(void) {
    barrier_wait(&worker_barrier, states.thread_count);
}

/* ---------- utilities ---------- */

static inline uint32_t ceil_uint(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

static inline uint32_t min_uint(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static inline uint32_t max_uint(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

// ceil(v / TILE_SIZE)
static inline uint32_t tile_ceil(uint32_t v) {
    return (v + TILE_MASK) >> TILE_SHIFT;
}

/* ---------- 4x4 matrices (row-major, column vector: v' = M * v) ---------- */

void mat4_identity(float* m) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (out may alias a or b)
void mat4_mul(float* out, const float* a, const float* b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                           a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}

// Camera looks down -Z, NDC z in [-1, 1], fov_y in radians
void mat4_perspective(float* m, float fov_y, float aspect, float z_near, float z_far) {
    float f = 1.0f / tanf(fov_y * 0.5f);
    memset(m, 0, sizeof(float) * 16);
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (z_far + z_near) / (z_near - z_far);
    m[11] = (2.0f * z_far * z_near) / (z_near - z_far);
    m[14] = -1.0f;
}

void mat4_translation(float* m, float x, float y, float z) {
    mat4_identity(m);
    m[3] = x;
    m[7] = y;
    m[11] = z;
}

void mat4_rotation_x(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[5] = c;  m[6] = -s;
    m[9] = s;  m[10] = c;
}

void mat4_rotation_y(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[0] = c;   m[2] = s;
    m[8] = -s;  m[10] = c;
}

void mat4_rotation_z(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[0] = c;  m[1] = -s;
    m[4] = s;  m[5] = c;
}

/* ---------- pipeline ---------- */

void* pipeline(void* arg);

static uint32_t cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
#else
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return cpus > 0 ? (uint32_t)cpus : 1;
#endif
}

void init(uint32_t width, uint32_t height, uint32_t triangle_count) {
    states.width = width;
    states.height = height;

    states.framebuffer = malloc(sizeof(uint32_t) * width * height);
    states.zbuffer = malloc(sizeof(float) * width * height);

    states.triangle_count = triangle_count;
    states.transformed_triangles = malloc(sizeof(TransformedTriangle) * 2 * triangle_count);

    states.tile_rows = ceil_uint(height, TILE_SIZE);
    states.tile_cols = ceil_uint(width, TILE_SIZE);
    states.tile_count = states.tile_cols * states.tile_rows;
    uint32_t tile_count = states.tile_count;

    states.tile_triangle_offset = malloc(sizeof(atomic_uint_fast32_t) * (tile_count + 1));
    states.tile_triangle_offset_temp = malloc(sizeof(atomic_uint_fast32_t) * (tile_count + 1));

    // grows on demand during binning (the worst case, tile_count * triangle_count, would be huge)
    states.tile_triangle_capacity = max_uint(triangle_count * 4, 1);
    states.tile_triangle_indices = malloc(sizeof(uint32_t) * states.tile_triangle_capacity);

    states.thread_count = cpu_count();
    states.threads = malloc(sizeof(pthread_t) * states.thread_count);
    states.thread_ids = malloc(sizeof(uint32_t) * states.thread_count);
    states.local_counts = calloc((size_t)states.thread_count * tile_count, sizeof(uint32_t));
    states.thread_valid = calloc(states.thread_count, sizeof(uint32_t));
    states.scan_partial = malloc(sizeof(uint32_t) * states.thread_count);

    atomic_store(&worker_barrier.count, 0);
    atomic_store(&worker_barrier.generation, 0);
    atomic_store(&frame_barrier.count, 0);
    atomic_store(&frame_barrier.generation, 0);

    atomic_store(&states.running, true);

    for (uint32_t i = 0; i < states.thread_count; i++){
        states.thread_ids[i] = i;
        pthread_create(&states.threads[i], NULL, pipeline, &states.thread_ids[i]);
    }
}

// renamed: "close" clashed with close() from <unistd.h>
void close_render(void) {
    atomic_store(&states.running, false);
    barrier_wait(&frame_barrier, states.thread_count + 1); // wake the workers so they can exit
    for (uint32_t i = 0; i < states.thread_count; i++){
        pthread_join(states.threads[i], NULL);
    }

    free(states.framebuffer);
    free(states.zbuffer);

    free(states.transformed_triangles);

    free(states.tile_triangle_offset_temp);
    free(states.tile_triangle_offset);
    free(states.tile_triangle_indices);
    free(states.scan_partial);
    free(states.thread_valid);
    free(states.local_counts);

    free(states.thread_ids);
    free(states.threads);
}

void set_triangles(Triangle* triangles) {
    states.raw_triangles = triangles;
}

void set_matrix(float* matrix) {
    states.matrix = matrix;
}

// Enables/disables backface culling (front = counter-clockwise, as in OBJ). Call between frames.
void set_backface_culling(bool enabled) {
    atomic_store(&states.cull_backface, enabled);
}

const uint32_t* get_framebuffer(void) {
    return states.framebuffer;
}

// Renders one frame (blocks until done). Call from the main thread.
void render(void) {
    if (!states.matrix || !states.raw_triangles)
        return;
    barrier_wait(&frame_barrier, states.thread_count + 1); // frame start
    barrier_wait(&frame_barrier, states.thread_count + 1); // frame end
}

void clear_buffers(uint32_t t_id) {
    uint32_t tile_count = states.tile_count + 1; // +1: offset sentinel
    uint32_t batch_size = ceil_uint(tile_count, states.thread_count);
    uint32_t start = min_uint(t_id * batch_size, tile_count);
    uint32_t end = min_uint(start + batch_size, tile_count);

    for (uint32_t i = start; i < end; i++){
        atomic_store_explicit(&states.tile_triangle_offset[i], 0, memory_order_relaxed);
        atomic_store_explicit(&states.tile_triangle_offset_temp[i], 0, memory_order_relaxed);
    }

    uint32_t pixels = states.height * states.width;
    batch_size = ceil_uint(pixels, states.thread_count);
    start = min_uint(t_id * batch_size, pixels);
    end = min_uint(start + batch_size, pixels);

    for (uint32_t i = start; i < end; i++){
        states.framebuffer[i] = 0;
        states.zbuffer[i] = FLT_MAX;
    }

    if (t_id == 0) {
        atomic_store(&states.current_tile, 0);
        atomic_store(&states.extra_count, 0);
    }
}

// Applies states.matrix (4x4 row-major) to (x,y,z,1). Returns clip-space coordinates.
static inline vec4 transform_vec(vec3 v) {
    const float* m = states.matrix;
    return (vec4){
        m[0]  * v.x + m[1]  * v.y + m[2]  * v.z + m[3],
        m[4]  * v.x + m[5]  * v.y + m[6]  * v.z + m[7],
        m[8]  * v.x + m[9]  * v.y + m[10] * v.z + m[11],
        m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15],
    };
}

// Perspective divide + viewport. x/y in pixels, z in NDC.
static inline vec3 project_vec(vec4 c) {
    float inv_w = 1.0f / c.w;
    return (vec3){
        (c.x * inv_w * 0.5f + 0.5f) * (float)states.width,
        (0.5f - c.y * inv_w * 0.5f) * (float)states.height,
        c.z * inv_w,
    };
}

// Triangles entirely beyond the far plane, or degenerate, are discarded.
// (partial far-plane overlap is handled per pixel in the rasterizer)
static inline bool is_visible(vec3 v0, vec3 v1, vec3 v2) {
    if (!isfinite(v0.x + v0.y + v0.z + v1.x + v1.y + v1.z + v2.x + v2.y + v2.z))
        return false;
    if (v0.z > 1.0f && v1.z > 1.0f && v2.z > 1.0f)
        return false;

    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    // screen y points down: a camera-facing triangle (counter-clockwise in the world) => area < 0
    if (atomic_load_explicit(&states.cull_backface, memory_order_relaxed))
        return area < 0.0f;
    return area != 0.0f;
}

// Computes the pixel-space bounding box. Returns false if the triangle is not visible
// (degenerate, behind the far plane, back-facing, or outside the screen).
static bool setup_triangle(TransformedTriangle* out, vec3 v0, vec3 v1, vec3 v2, uint32_t color) {
    if (!is_visible(v0, v1, v2))
        return false;

    float fw = (float)states.width, fh = (float)states.height;
    // clamp in float before the cast (negative float -> uint is UB)
    float fmin_x = fmaxf(floorf(fminf(fminf(v0.x, v1.x), v2.x)), 0.0f);
    float fmin_y = fmaxf(floorf(fminf(fminf(v0.y, v1.y), v2.y)), 0.0f);
    float fmax_x = fminf(ceilf(fmaxf(fmaxf(v0.x, v1.x), v2.x)), fw);
    float fmax_y = fminf(ceilf(fmaxf(fmaxf(v0.y, v1.y), v2.y)), fh);
    if (fmin_x >= fmax_x || fmin_y >= fmax_y)
        return false;

    *out = (TransformedTriangle){
        .v0 = v0, .v1 = v1, .v2 = v2, .color = color,
        .min_x = (uint32_t)fmin_x, .min_y = (uint32_t)fmin_y,
        .max_x = (uint32_t)fmax_x, .max_y = (uint32_t)fmax_y,
    };
    return true;
}

// Stores the triangle in its slot and counts the tiles it covers in the thread-local counters.
static void store_triangle(uint32_t slot, uint32_t* local, const TransformedTriangle* t) {
    states.transformed_triangles[slot] = *t;

    uint32_t tile_x_start = t->min_x >> TILE_SHIFT;
    uint32_t tile_y_start = t->min_y >> TILE_SHIFT;
    uint32_t tile_x_end = tile_ceil(t->max_x);
    uint32_t tile_y_end = tile_ceil(t->max_y);

    for (uint32_t ty = tile_y_start; ty < tile_y_end; ty++){
        for (uint32_t tx = tile_x_start; tx < tile_x_end; tx++){
            local[ty * states.tile_cols + tx]++;
        }
    }
}

// Bits of the frustum planes (-w <= x,y,z <= w) that the vertex violates.
static inline uint32_t outcode(vec4 c) {
    return (c.x < -c.w) | ((c.x > c.w) << 1) | ((c.y < -c.w) << 2) | ((c.y > c.w) << 3) |
           ((c.z < -c.w) << 4) | ((c.z > c.w) << 5);
}

static inline vec4 lerp_vec4(vec4 a, vec4 b, float t) {
    return (vec4){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

// Static block of items [start, end) that thread t_id handles out of n.
static inline void thread_block(uint32_t n, uint32_t t_id, uint32_t* start, uint32_t* end) {
    uint32_t batch_size = ceil_uint(n, states.thread_count);
    *start = min_uint(t_id * batch_size, n);
    *end = min_uint(*start + batch_size, n);
}

// Visible triangles are written compactly (no gaps), so no slot ever holds stale data:
//  - thread t_id writes into [start, start + thread_valid[t_id]) of its own block;
//  - the 2nd triangle produced by near-plane clipping (rare) goes to an extra slot in
//    [n, n + extra_count).
// binning() visits exactly these slots.
void vertex_shader(uint32_t t_id) {
    uint32_t n = states.triangle_count;
    uint32_t* local = states.local_counts + (size_t)t_id * states.tile_count;
    uint32_t start, end;
    thread_block(n, t_id, &start, &end);
    uint32_t valid = 0;

    for (uint32_t i = start; i < end; i++){
        Triangle t = states.raw_triangles[i];
        vec4 in[3] = { transform_vec(t.v0), transform_vec(t.v1), transform_vec(t.v2) };

        // frustum culling: all 3 vertices outside the same plane => invisible triangle
        if (outcode(in[0]) & outcode(in[1]) & outcode(in[2]))
            continue;

        // Sutherland-Hodgman against the near plane (z + w >= 0)
        vec4 poly[4];
        uint32_t count = 0;
        for (int a = 0; a < 3; a++){
            vec4 p = in[a], q = in[(a + 1) % 3];
            float dp = p.z + p.w, dq = q.z + q.w;
            if (dp >= 0.0f)
                poly[count++] = p;
            if ((dp >= 0.0f) != (dq >= 0.0f))
                poly[count++] = lerp_vec4(p, q, dp / (dp - dq));
        }

        // triangle fan over the clipped polygon (0, 1 or 2 triangles)
        uint32_t tris = count >= 3 ? count - 2 : 0;
        for (uint32_t k = 0; k < tris; k++){
            if (poly[0].w <= 1e-6f || poly[k + 1].w <= 1e-6f || poly[k + 2].w <= 1e-6f)
                continue;

            TransformedTriangle out;
            if (!setup_triangle(&out, project_vec(poly[0]), project_vec(poly[k + 1]),
                                project_vec(poly[k + 2]), t.color))
                continue;

            uint32_t slot = k == 0 ? start + valid++
                                   : n + atomic_fetch_add_explicit(&states.extra_count, 1, memory_order_relaxed);
            store_triangle(slot, local, &out);
        }
    }
    states.thread_valid[t_id] = valid;

    // add the local counts into the global counters (one atomic per tile instead of per triangle)
    for (uint32_t k = 0; k < states.tile_count; k++){
        if (local[k]) {
            atomic_fetch_add_explicit(&states.tile_triangle_offset[k], local[k], memory_order_relaxed);
            local[k] = 0;
        }
    }
}

// Parallel prefix sum: counts in offset[0..n) -> offset[0] = 0, offset[i+1] = inclusive sum
// up to i; temp[i] = offset[i+1]. Also ensures tile_triangle_indices has enough capacity.
static void tile_prefix_sum(uint32_t t_id) {
    uint32_t start, end;
    thread_block(states.tile_count, t_id, &start, &end);

    // 1) local sum of each block
    uint32_t sum = 0;
    for (uint32_t i = start; i < end; i++)
        sum += states.tile_triangle_offset[i];
    states.scan_partial[t_id] = sum;

    barrier();

    // 2) local scan of the block, offset by the total of the previous blocks
    uint32_t run = 0;
    for (uint32_t i = 0; i < t_id; i++)
        run += states.scan_partial[i];

    for (uint32_t i = start; i < end; i++){
        run += states.tile_triangle_offset[i];
        states.tile_triangle_offset_temp[i] = run;
    }

    if (t_id == 0) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < states.thread_count; i++)
            total += states.scan_partial[i];

        if (total > states.tile_triangle_capacity) {
            uint32_t cap = max_uint(total, states.tile_triangle_capacity * 4);
            uint32_t* p = realloc(states.tile_triangle_indices, sizeof(uint32_t) * cap);
            if (!p) {
                perror("realloc tile_triangle_indices");
                abort();
            }
            states.tile_triangle_indices = p;
            states.tile_triangle_capacity = cap;
        }
    }

    barrier();

    // 3) exclusive offset; temp must be fully read before the scatter starts decrementing it
    if (t_id == 0)
        states.tile_triangle_offset[0] = 0;
    for (uint32_t i = start; i < end; i++)
        states.tile_triangle_offset[i + 1] = states.tile_triangle_offset_temp[i];

    barrier();
}

static inline void bin_slot(uint32_t i) {
    const TransformedTriangle* t = &states.transformed_triangles[i];

    uint32_t tile_x_start = t->min_x >> TILE_SHIFT;
    uint32_t tile_y_start = t->min_y >> TILE_SHIFT;

    uint32_t tile_x_end = tile_ceil(t->max_x);
    uint32_t tile_y_end = tile_ceil(t->max_y);

    for (uint32_t j = tile_y_start; j < tile_y_end; j++){
        for (uint32_t k = tile_x_start; k < tile_x_end; k++){
            uint32_t offset_idx = j * states.tile_cols + k;
            uint32_t idx = atomic_fetch_sub_explicit(&states.tile_triangle_offset_temp[offset_idx],
                                                     1, memory_order_relaxed) - 1;
            states.tile_triangle_indices[idx] = i;
        }
    }
}

void binning(uint32_t t_id) {
    tile_prefix_sum(t_id);

    // the same block this thread filled in vertex_shader, only up to its valid count
    uint32_t start, end;
    thread_block(states.triangle_count, t_id, &start, &end);
    for (uint32_t i = start; i < start + states.thread_valid[t_id]; i++)
        bin_slot(i);

    // extra slots from clipping
    uint32_t extra = (uint32_t)atomic_load(&states.extra_count);
    thread_block(extra, t_id, &start, &end);
    for (uint32_t i = start; i < end; i++)
        bin_slot(states.triangle_count + i);
}

// Fixed-point rasterization (8 subpixel bits): edge functions are exact, so neighboring
// triangles never leave gaps nor draw a shared-edge pixel twice.
#define SUBPIXEL_BITS 8
#define SUBPIXEL_ONE (1 << SUBPIXEL_BITS)
#define COORD_LIMIT 1000000.0f

typedef struct { int64_t x, y; } ivec2;

static inline ivec2 snap(vec3 v) {
    return (ivec2){
        llrintf(fminf(fmaxf(v.x, -COORD_LIMIT), COORD_LIMIT) * SUBPIXEL_ONE),
        llrintf(fminf(fmaxf(v.y, -COORD_LIMIT), COORD_LIMIT) * SUBPIXEL_ONE),
    };
}

static inline int64_t edge_fn(ivec2 a, ivec2 b, int64_t px, int64_t py) {
    return (px - a.x) * (b.y - a.y) - (py - a.y) * (b.x - a.x);
}

// Tie-break rule: edge a->b only "belongs" to the triangle if it goes in this direction.
// On a shared edge the two triangles traverse it in opposite directions, so only one claims it.
static inline int64_t edge_bias(ivec2 a, ivec2 b) {
    int64_t dx = b.x - a.x, dy = b.y - a.y;
    return (dy > 0 || (dy == 0 && dx > 0)) ? 0 : -1;
}

// Rasterizes the triangle inside [min_x, max_x) x [min_y, max_y) (one tile).
// Per-pixel z = barycentric interpolation of the vertex z values (linear in screen space).
static inline void rasterize_triangle(TransformedTriangle t, uint32_t min_x, uint32_t max_x, uint32_t min_y, uint32_t max_y) {
    min_x = max_uint(min_x, t.min_x);
    max_x = min_uint(max_x, t.max_x);
    min_y = max_uint(min_y, t.min_y);
    max_y = min_uint(max_y, t.max_y);
    if (min_x >= max_x || min_y >= max_y)
        return;

    ivec2 p0 = snap(t.v0), p1 = snap(t.v1), p2 = snap(t.v2);
    float z0 = t.v0.z, z1 = t.v1.z, z2 = t.v2.z;

    int64_t area = edge_fn(p0, p1, p2.x, p2.y);
    if (area == 0)
        return;
    if (area < 0) { // normalize the winding (no backface culling here)
        ivec2 tp = p1; p1 = p2; p2 = tp;
        float tz = z1; z1 = z2; z2 = tz;
        area = -area;
    }
    float inv_area = 1.0f / (float)area;

    // weights: w0 -> v0 (edge p1->p2), w1 -> v1 (p2->p0), w2 -> v2 (p0->p1); all >= 0 inside
    int64_t bias0 = edge_bias(p1, p2), bias1 = edge_bias(p2, p0), bias2 = edge_bias(p0, p1);
    int64_t a0 = (p2.y - p1.y) * SUBPIXEL_ONE, b0 = -(p2.x - p1.x) * SUBPIXEL_ONE;
    int64_t a1 = (p0.y - p2.y) * SUBPIXEL_ONE, b1 = -(p0.x - p2.x) * SUBPIXEL_ONE;
    int64_t a2 = (p1.y - p0.y) * SUBPIXEL_ONE, b2 = -(p1.x - p0.x) * SUBPIXEL_ONE;

    int64_t px = ((int64_t)min_x << SUBPIXEL_BITS) + SUBPIXEL_ONE / 2;
    int64_t py = ((int64_t)min_y << SUBPIXEL_BITS) + SUBPIXEL_ONE / 2;
    int64_t row_w0 = edge_fn(p1, p2, px, py);
    int64_t row_w1 = edge_fn(p2, p0, px, py);
    int64_t row_w2 = edge_fn(p0, p1, px, py);

    for (uint32_t y = min_y; y < max_y; y++){
        int64_t w0 = row_w0, w1 = row_w1, w2 = row_w2;
        uint32_t base = y * states.width;

        for (uint32_t x = min_x; x < max_x; x++){
            if ((w0 + bias0) >= 0 && (w1 + bias1) >= 0 && (w2 + bias2) >= 0) {
                float z = ((float)w0 * z0 + (float)w1 * z1 + (float)w2 * z2) * inv_area;
                if (z <= 1.0f && z < states.zbuffer[base + x]) { // z > 1: beyond the far plane
                    states.zbuffer[base + x] = z;
                    states.framebuffer[base + x] = t.color;
                }
            }
            w0 += a0; w1 += a1; w2 += a2;
        }

        row_w0 += b0; row_w1 += b1; row_w2 += b2;
    }
}

void rasterize(uint32_t t_id) {
    (void)t_id;
    uint32_t tile_count = states.tile_count;
    while (1) {
        uint32_t tile_idx = atomic_fetch_add_explicit(&states.current_tile, 1, memory_order_relaxed);
        if (tile_idx >= tile_count)
            break;

        uint32_t tile_row = tile_idx / states.tile_cols;
        uint32_t min_y = tile_row << TILE_SHIFT;
        uint32_t min_x = (tile_idx - tile_row * states.tile_cols) << TILE_SHIFT;

        uint32_t max_y = min_uint(min_y + TILE_SIZE, states.height);
        uint32_t max_x = min_uint(min_x + TILE_SIZE, states.width);

        uint32_t start = states.tile_triangle_offset[tile_idx];
        uint32_t end = states.tile_triangle_offset[tile_idx + 1];

        for (uint32_t i = start; i < end; i++){
            uint32_t idx = states.tile_triangle_indices[i];
            rasterize_triangle(states.transformed_triangles[idx], min_x, max_x, min_y, max_y);
        }
    }
}

void* pipeline(void* arg) {
    uint32_t t_id = *(uint32_t*)arg;
    while (1) {
        barrier_wait(&frame_barrier, states.thread_count + 1); // wait for render()
        if (!atomic_load(&states.running)) // only changes before the barrier, so all threads agree
            break;

        clear_buffers(t_id);
        barrier();
        vertex_shader(t_id);
        barrier();
        binning(t_id);
        barrier();
        rasterize(t_id);

        barrier_wait(&frame_barrier, states.thread_count + 1); // end of frame
    }
    return NULL;
}
