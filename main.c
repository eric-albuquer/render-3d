#include <stdio.h>
#include <stdlib.h>
#include "raylib.h"
#include "render_raylib.h"

#define W 1440
#define H 960

#define BRIDGE_DIR "Historic Boardman Bridge (1888)"

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
    SetTargetFPS(60);

    float proj[16], view[16], rx[16], rz[16], model[16], mv[16], mvp[16];
    mat4_perspective(proj, 60.0f * 3.14159265f / 180.0f, (float)W / H, 0.1f, 100.0f);
    mat4_translation(view, 0, 0.5, -2.1f); // camera looks down -Z
    mat4_rotation_x(rx, -1.0f);          // tilt to look from above

    set_triangles(model_tris);
    set_matrix(mvp);
    set_backface_culling(true);

    float angle = 0;
    while (!WindowShouldClose()) {
        angle += GetFrameTime() * 0.5f;

        mat4_rotation_z(rz, angle);
        mat4_mul(model, rx, rz);
        mat4_mul(mv, view, model);
        mat4_mul(mvp, proj, mv); // MVP = P * V * M (updated before render(), while no worker is running)

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
