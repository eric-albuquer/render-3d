#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <stdint.h>
#include <math.h>
#include <float.h>
#include <dirent.h>
#include <ctype.h>
#include "render.h"

typedef struct {
    char name[128];
    uint32_t color;
} Material;

typedef struct {
    Material* items;
    size_t count, cap;
} MaterialList;

#define DEFAULT_COLOR 0xFFB4B4B4u

static char* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    if (size)
        *size = (size_t)len;
    return buf;
}

static inline char* skip_blank(char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    return p;
}

static inline char* next_line(char* p) {
    while (*p && *p != '\n')
        p++;
    return *p ? p + 1 : p;
}

// Copies the word at p into dst and returns the pointer after it.
static char* read_word(char* p, char* dst, size_t cap) {
    p = skip_blank(p);
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (n + 1 < cap)
            dst[n++] = *p;
        p++;
    }
    dst[n] = '\0';
    return p;
}

static uint32_t pack_color(float r, float g, float b) {
    uint32_t R = (uint32_t)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t G = (uint32_t)(fminf(fmaxf(g, 0.0f), 1.0f) * 255.0f + 0.5f);
    uint32_t B = (uint32_t)(fminf(fmaxf(b, 0.0f), 1.0f) * 255.0f + 0.5f);
    return 0xFF000000u | (B << 16) | (G << 8) | R;
}

// Reads newmtl/Kd from a .mtl.
static void load_mtl(const char* path, MaterialList* list) {
    char* data = read_file(path, NULL);
    if (!data)
        return;

    int have = 0;
    for (char* p = data; *p; p = next_line(p)) {
        p = skip_blank(p);
        if (strncmp(p, "newmtl", 6) == 0 && isspace((unsigned char)p[6])) {
            if (list->count == list->cap) {
                list->cap = list->cap ? list->cap * 2 : 16;
                list->items = realloc(list->items, sizeof(Material) * list->cap);
            }
            Material* m = &list->items[list->count++];
            read_word(p + 6, m->name, sizeof(m->name));
            m->color = DEFAULT_COLOR;
            have = 1;
        } else if (have && p[0] == 'K' && p[1] == 'd' && isspace((unsigned char)p[2])) {
            char* end;
            float r = strtof(p + 2, &end);
            float g = strtof(end, &end);
            float b = strtof(end, &end);
            list->items[list->count - 1].color = pack_color(r, g, b);
        }
    }
    free(data);
}

// Finds the first file with the given extension in the folder.
static int find_file(const char* dir, const char* ext, char* out, size_t cap) {
    DIR* d = opendir(dir);
    if (!d)
        return 0;
    size_t ext_len = strlen(ext);
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > ext_len && e->d_name[0] != '.' && strcasecmp(e->d_name + n - ext_len, ext) == 0) {
            snprintf(out, cap, "%s/%s", dir, e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

// Darkens/brightens the color according to the face normal (light fixed in model space).
// The OBJ only has flat colors, and without this the relief would be unreadable.
static uint32_t shade(uint32_t color, vec3 a, vec3 b, vec3 c) {
    vec3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
    vec3 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
    vec3 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len < 1e-12f)
        return color;
    float d = fabsf(n.x * 0.35f + n.y * 0.80f + n.z * 0.48f) / len; // |n . L|, L ~ unit
    float k = 0.45f + 0.55f * d;
    uint32_t r = (uint32_t)((color & 0xFF) * k);
    uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * k);
    uint32_t bl = (uint32_t)(((color >> 16) & 0xFF) * k);
    return 0xFF000000u | (bl << 16) | (g << 8) | r;
}

// Vertex index of a token "v", "v/vt", "v//vn" or "v/vt/vn" (1-based; negative = relative).
static int parse_face_index(char** pp, size_t vcount, long* out) {
    char* p = *pp;
    char* end;
    long v = strtol(p, &end, 10);
    if (end == p)
        return 0;
    while (*end && !isspace((unsigned char)*end)) // ignore /vt/vn
        end++;
    *pp = end;
    if (v < 0)
        v = (long)vcount + v + 1;
    if (v < 1 || (size_t)v > vcount)
        return 0;
    *out = v - 1;
    return 1;
}

uint32_t load_obj_folder(const char* dir, Triangle** out_triangles) {
    *out_triangles = NULL;

    char obj_path[1024];
    if (!find_file(dir, ".obj", obj_path, sizeof(obj_path))) {
        fprintf(stderr, "load_obj_folder: nenhum .obj em '%s'\n", dir);
        return 0;
    }

    size_t size;
    char* data = read_file(obj_path, &size);
    if (!data) {
        fprintf(stderr, "load_obj_folder: falha ao ler '%s'\n", obj_path);
        return 0;
    }

    MaterialList mats = {0};
    size_t vcap = 1024, vcount = 0;
    vec3* verts = malloc(sizeof(vec3) * vcap);
    size_t tcap = 1024, tcount = 0;
    Triangle* tris = malloc(sizeof(Triangle) * tcap);
    uint32_t current = DEFAULT_COLOR;

    for (char* p = data; *p; p = next_line(p)) {
        p = skip_blank(p);
        if (p[0] == 'v' && isspace((unsigned char)p[1])) {
            if (vcount == vcap) {
                vcap *= 2;
                verts = realloc(verts, sizeof(vec3) * vcap);
            }
            char* end;
            verts[vcount].x = strtof(p + 1, &end);
            verts[vcount].y = strtof(end, &end);
            verts[vcount].z = strtof(end, &end);
            vcount++;
        } else if (p[0] == 'f' && isspace((unsigned char)p[1])) {
            long idx[3];
            long first = -1, prev = -1;
            int have = 0;
            char* q = p + 1;
            for (;;) {
                q = skip_blank(q);
                if (*q == '\n' || *q == '\0')
                    break;
                long v;
                if (!parse_face_index(&q, vcount, &v))
                    break;
                if (!have) {
                    first = v;
                    have = 1;
                } else if (prev < 0) {
                    prev = v;
                } else {
                    // fan: (first, prev, v) for polygons with more than 3 vertices
                    idx[0] = first; idx[1] = prev; idx[2] = v;
                    if (tcount == tcap) {
                        tcap *= 2;
                        tris = realloc(tris, sizeof(Triangle) * tcap);
                    }
                    Triangle t = {verts[idx[0]], verts[idx[1]], verts[idx[2]], 0};
                    t.color = shade(current, t.v0, t.v1, t.v2);
                    tris[tcount++] = t;
                    prev = v;
                }
            }
        } else if (strncmp(p, "usemtl", 6) == 0 && isspace((unsigned char)p[6])) {
            char name[128];
            read_word(p + 6, name, sizeof(name));
            current = DEFAULT_COLOR;
            for (size_t i = 0; i < mats.count; i++) {
                if (strcmp(mats.items[i].name, name) == 0) {
                    current = mats.items[i].color;
                    break;
                }
            }
        } else if (strncmp(p, "mtllib", 6) == 0 && isspace((unsigned char)p[6])) {
            char name[256], path[1400];
            read_word(p + 6, name, sizeof(name));
            snprintf(path, sizeof(path), "%s/%s", dir, name);
            load_mtl(path, &mats);
        }
    }

    free(data);
    free(verts);
    free(mats.items);

    if (tcount == 0) {
        free(tris);
        return 0;
    }
    *out_triangles = realloc(tris, sizeof(Triangle) * tcount);
    return (uint32_t)tcount;
}

void triangles_normalize(Triangle* tris, uint32_t count, float radius) {
    if (count == 0)
        return;
    vec3 lo = {FLT_MAX, FLT_MAX, FLT_MAX}, hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (uint32_t i = 0; i < count; i++) {
        const vec3* v = &tris[i].v0;
        for (int k = 0; k < 3; k++) {
            lo.x = fminf(lo.x, v[k].x); hi.x = fmaxf(hi.x, v[k].x);
            lo.y = fminf(lo.y, v[k].y); hi.y = fmaxf(hi.y, v[k].y);
            lo.z = fminf(lo.z, v[k].z); hi.z = fmaxf(hi.z, v[k].z);
        }
    }
    vec3 c = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    float ext = fmaxf(fmaxf(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z) * 0.5f;
    float s = ext > 0 ? radius / ext : 1.0f;
    for (uint32_t i = 0; i < count; i++) {
        vec3* v = &tris[i].v0;
        for (int k = 0; k < 3; k++) {
            v[k].x = (v[k].x - c.x) * s;
            v[k].y = (v[k].y - c.y) * s;
            v[k].z = (v[k].z - c.z) * s;
        }
    }
}
