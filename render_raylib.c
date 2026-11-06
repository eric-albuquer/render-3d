#include <raylib.h>
#include "render_raylib.h"

static Texture2D fb_texture;
static uint32_t fb_width, fb_height;

void rr_init(uint32_t width, uint32_t height, uint32_t triangle_count, const char* title) {
    fb_width = width;
    fb_height = height;

    InitWindow((int)width, (int)height, title);
    init(width, height, triangle_count);

    // uint32 pixels 0xAABBGGRR == bytes R,G,B,A
    Image img = GenImageColor((int)width, (int)height, BLACK);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    fb_texture = LoadTextureFromImage(img);
    UnloadImage(img);
}

void rr_close(void) {
    UnloadTexture(fb_texture);
    close_render();
    CloseWindow();
}

void rr_render_and_draw(void) {
    render();
    UpdateTexture(fb_texture, get_framebuffer());
    DrawTexture(fb_texture, 0, 0, WHITE);
}
