// Single C translation unit that holds the Clay implementation and the
// Raylib renderer shipped with Clay. Kept in C because the renderer uses
// C compound literals that are not valid C++.
#include <math.h>
#include <stdlib.h>

#include "raylib.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "clay_renderer_raylib.c"

void ClayBridge_SetMeasureText(Font *fonts) {
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);
}
