#ifndef RENDER_RAYLIB_H
#define RENDER_RAYLIB_H

#include <stdint.h>
#include "render.h"

// Opens the raylib window and initializes the renderer.
void rr_init(uint32_t width, uint32_t height, uint32_t triangle_count, const char* title);
void rr_close(void);

// Renders a frame into the framebuffer, uploads it to the texture and draws it.
// Must be called between BeginDrawing()/EndDrawing() (so a HUD can be drawn on top).
void rr_render_and_draw(void);

#endif
