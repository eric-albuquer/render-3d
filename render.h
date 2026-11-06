#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float x, y, z, w; } vec4;

typedef struct {
    vec3 v0, v1, v2;
    uint32_t color; // bytes R,G,B,A in memory (0xAABBGGRR on little-endian)
} Triangle;

void init(uint32_t width, uint32_t height, uint32_t triangle_count);
void close_render(void);
void set_triangles(Triangle* triangles);
void set_matrix(float* matrix); // 4x4 row-major, read on every render()
void set_backface_culling(bool enabled); // front = counter-clockwise; default: off
void render(void);              // blocks until the frame is done
const uint32_t* get_framebuffer(void);

void mat4_identity(float* m);
void mat4_mul(float* out, const float* a, const float* b);
void mat4_perspective(float* m, float fov_y, float aspect, float z_near, float z_far);
void mat4_translation(float* m, float x, float y, float z);
void mat4_rotation_x(float* m, float angle);
void mat4_rotation_y(float* m, float angle);
void mat4_rotation_z(float* m, float angle);

// Loads the first .obj in the folder (and the referenced .mtl, Kd colors). Faces with more than
// 3 vertices become a triangle fan. Returns the count; *out_triangles must be released with free().
uint32_t load_obj_folder(const char* dir, Triangle** out_triangles);
// Centers the model at the origin and scales it to fit a cube of half-extent `radius`.
void triangles_normalize(Triangle* tris, uint32_t count, float radius);

#endif
