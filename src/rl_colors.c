#include "pocketpy.h"

#define ADD_COLOR(name, r, g, b, a) \
    py_newcolor32(py_emplacedict(mod, py_name(#name)), (c11_color32){{r, g, b, a}})

void py__add_raylib_colors(void) {
    py_GlobalRef mod = py_getmodule("raylib");
    ADD_COLOR(LIGHTGRAY, 200, 200, 200, 255);
    ADD_COLOR(GRAY, 130, 130, 130, 255);
    ADD_COLOR(DARKGRAY, 80, 80, 80, 255);
    ADD_COLOR(YELLOW, 253, 249, 0, 255);
    ADD_COLOR(GOLD, 255, 203, 0, 255);
    ADD_COLOR(ORANGE, 255, 161, 0, 255);
    ADD_COLOR(PINK, 255, 109, 194, 255);
    ADD_COLOR(RED, 230, 41, 55, 255);
    ADD_COLOR(MAROON, 190, 33, 55, 255);
    ADD_COLOR(GREEN, 0, 228, 48, 255);
    ADD_COLOR(LIME, 0, 158, 47, 255);
    ADD_COLOR(DARKGREEN, 0, 117, 44, 255);
    ADD_COLOR(SKYBLUE, 102, 191, 255, 255);
    ADD_COLOR(BLUE, 0, 121, 241, 255);
    ADD_COLOR(DARKBLUE, 0, 82, 172, 255);
    ADD_COLOR(PURPLE, 200, 122, 255, 255);
    ADD_COLOR(VIOLET, 135, 60, 190, 255);
    ADD_COLOR(DARKPURPLE, 112, 31, 126, 255);
    ADD_COLOR(BEIGE, 211, 176, 131, 255);
    ADD_COLOR(BROWN, 127, 106, 79, 255);
    ADD_COLOR(DARKBROWN, 76, 63, 47, 255);
    ADD_COLOR(WHITE, 255, 255, 255, 255);
    ADD_COLOR(BLACK, 0, 0, 0, 255);
    ADD_COLOR(BLANK, 0, 0, 0, 0);
    ADD_COLOR(MAGENTA, 255, 0, 255, 255);
    ADD_COLOR(RAYWHITE, 245, 245, 245, 255);
}
