#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"

typedef uint64_t ULONGLONG;
typedef int32_t LONG;
typedef uint16_t USHORT;
#define MOUSE_MOVE_ABSOLUTE 1
#define LEVI_INPUT_FIX_PURE_TEST
#include "input_fix.h"
#define LEVI_RENDER_SCALE_PURE_TEST
#include "render_scale.h"

int main(void) {
    char code[9];
    assert(levi_device_code(L"Open the page and enter the code\n\nAB12CD34\n\nContinue", code));
    assert(!strcmp(code, "AB12CD34"));
    assert(!levi_device_code(L"An unrelated warning", code));
    assert(!levi_device_code(L"enter the code\nAB12CD345", code));
    assert(!levi_device_code(L"enter the code\nAB12\n", code));
    assert(!levi_device_code(L"enter the code\nAB12&X34", code));

    /* Raw mouse spike filter: only drops huge relative jumps during focus grace window. */
    assert(levi_raw_mouse_is_spike(300, 0, 0, 1050, 1180) == 1);
    assert(levi_raw_mouse_is_spike(0, -250, 0, 1100, 1180) == 1);
    assert(levi_raw_mouse_is_spike(40, -30, 0, 1100, 1180) == 0);
    assert(levi_raw_mouse_is_spike(400, 0, 0, 1200, 1180) == 0);
    assert(levi_raw_mouse_is_spike(400, 0, MOUSE_MOVE_ABSOLUTE, 1050, 1180) == 0);

    /* Virtual render scale helpers. */
    assert(levi_parse_render_scale(NULL) == 1.0);
    assert(levi_parse_render_scale("1.0") == 1.0);
    assert(levi_parse_render_scale("bad") == 1.0);
    assert(levi_parse_render_scale("9.0") == 1.0);
    assert(levi_parse_render_scale("2.00") == 2.0);
    assert(levi_scale_down_dim(3420, 2.0) == 1710);
    assert(levi_scale_down_dim(2082, 2.0) == 1041);
    assert(levi_scale_down_dim(1710, 1.5) == 1140);
    assert(levi_scale_down_dim(100, 2.0) == 64);
    assert(levi_scale_down_coord(1000, 2.0) == 500);
    assert(levi_scale_up_coord(500, 2.0) == 1000);
    return 0;
}
