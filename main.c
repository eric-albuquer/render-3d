#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raylib.h"
#include "render_raylib.h"

#define W 1440
#define H 960

#define BRIDGE_DIR "Dynamic Flight SM-11"

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : BRIDGE_DIR;

    Triangle* model_tris;
    uint32_t count = load_obj_folder(dir, &model_tris);
    if (count == 0) {
        fprintf(stderr, "nao foi possivel carregar o modelo de '%s'\n", dir);
        return 1;
    }
    printf("%u triangulos carregados de '%s'\n", count, dir);
    triangles_normalize(model_tris, count, 1.0f); // fits in [-1, 1]

    rr_init(W, H, count, "render3d");
    //SetTargetFPS(180);

    float proj[16], mvp[16];
    mat4_perspective(proj, 60.0f * 3.14159265f / 180.0f, (float)W / H, 0.1f, 100.0f);

    set_triangles(model_tris);
    set_matrix(mvp);

    float angle = 0;
    while (!WindowShouldClose()) {
        angle += GetFrameTime() * 0.5f;

        memcpy(mvp, proj, sizeof(mvp));
        mat4_translate(mvp, 0, 0.2f, -2.1f); // camera looks down -Z
        mat4_rotate_x(mvp, -1.0f); // tilt to look from above
        mat4_rotate_z(mvp, angle);

        BeginDrawing();
        //ClearBackground(BLACK);
        rr_render_and_draw();
        DrawFPS(10, 10);
        EndDrawing();
    }

    rr_close();
    free(model_tris);
    return 0;
}
