// C bridge between the C++ application and Clay's Raylib renderer.
// clay.h is compiled (CLAY_IMPLEMENTATION) once in clay_impl.c together with
// the renderer; the C++ side only includes the header.
#pragma once

#include "raylib.h"
#include "clay.h"

#ifdef __cplusplus
extern "C" {
#endif

void Clay_Raylib_Initialize(int width, int height, const char *title, unsigned int flags);
void Clay_Raylib_Close(void);
void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts);

// Registers the renderer's text measuring function with Clay.
void ClayBridge_SetMeasureText(Font *fonts);

#ifdef __cplusplus
}
#endif
