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

int main(void) {
    char code[9];
    assert(levi_device_code(L"Open the page and enter the code\n\nAB12CD34\n\nContinue", code));
    assert(!strcmp(code, "AB12CD34"));
    assert(!levi_device_code(L"An unrelated warning", code));
    assert(!levi_device_code(L"enter the code\nAB12CD345", code));
    assert(!levi_device_code(L"enter the code\nAB12\n", code));
    assert(!levi_device_code(L"enter the code\nAB12&X34", code));

    /* Scroll wheel quantization: discrete wheel fires immediately; trackpad burst is tamed. */
    LeviWheelState ws = {0, 0, 0};
    assert(levi_wheel_filter(&ws, 120, 1000) == 120);
    assert(levi_wheel_filter(&ws, 120, 1020) == 0);   /* within 55ms momentum cooldown */
    assert(levi_wheel_filter(&ws, 120, 1060) == 120); /* after 55ms cooldown */

    /* Sub-notch trackpad deltas accumulate to 120, reset on direction flip. */
    LeviWheelState tp = {0, 0, 0};
    assert(levi_wheel_filter(&tp, 40, 2000) == 0);
    assert(levi_wheel_filter(&tp, 40, 2010) == 0);
    assert(levi_wheel_filter(&tp, 40, 2020) == 120);
    assert(levi_wheel_filter(&tp, -50, 2100) == 0);
    assert(levi_wheel_filter(&tp, -70, 2110) == -120);

    /* Raw mouse spike filter: only drops huge relative jumps during focus grace window. */
    assert(levi_raw_mouse_is_spike(300, 0, 0, 1050, 1180) == 1);
    assert(levi_raw_mouse_is_spike(0, -250, 0, 1100, 1180) == 1);
    assert(levi_raw_mouse_is_spike(40, -30, 0, 1100, 1180) == 0);
    assert(levi_raw_mouse_is_spike(400, 0, 0, 1200, 1180) == 0);
    assert(levi_raw_mouse_is_spike(400, 0, MOUSE_MOVE_ABSOLUTE, 1050, 1180) == 0);
    return 0;
}
