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
#define LEVI_SHELL_FIX_PURE_TEST
#include "shell_fix.h"

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

    /* Shell bridge helpers: explorer arg parsing, cmd /c parsing, endpoint & JSON parsing. */
    assert(levi_shell_is_open_verb(NULL) == 1);
    assert(levi_shell_is_open_verb(L"") == 1);
    assert(levi_shell_is_open_verb(L"OPEN") == 1);
    assert(levi_shell_is_open_verb(L"explore") == 1);
    assert(levi_shell_is_open_verb(L"print") == 0);

    assert(levi_shell_is_explorer_exe(L"explorer.exe") == 1);
    assert(levi_shell_is_explorer_exe(L"C:\\Windows\\EXPLORER.EXE") == 1);
    assert(levi_shell_is_explorer_exe(L"\"C:\\Windows\\explorer\"") == 1);
    assert(levi_shell_is_explorer_exe(L"notepad.exe") == 0);

    wchar_t target[256];
    int reveal = -1;
    assert(levi_shell_parse_explorer_args(L"/select,\"C:\\shader packs\\BSL.zip\"", target, 256, &reveal) == 1);
    assert(reveal == 1 && wcscmp(target, L"C:\\shader packs\\BSL.zip") == 0);

    assert(levi_shell_parse_explorer_args(L"/e, \"Z:\\Users\\test\\shaderpacks\"", target, 256, &reveal) == 1);
    assert(reveal == 0 && wcscmp(target, L"Z:\\Users\\test\\shaderpacks") == 0);

    assert(levi_shell_parse_explorer_args(L"/desktop=shell,1024x768", target, 256, &reveal) == 0);
    assert(levi_shell_parse_explorer_args(L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", target, 256, &reveal) == 0);

    assert(levi_shell_parse_cmdline_explorer(L"\"C:\\windows\\explorer.exe\" /select,\"C:\\a\\b.txt\"", target, 256, &reveal) == 1);
    assert(reveal == 1 && wcscmp(target, L"C:\\a\\b.txt") == 0);

    assert(levi_shell_parse_cmdline_explorer(L"cmd.exe /c start \"\" \"Z:\\Users\\a\\shaderpacks\"", target, 256, &reveal) == 1);
    assert(reveal == 0 && wcscmp(target, L"Z:\\Users\\a\\shaderpacks") == 0);

    assert(levi_shell_parse_cmdline_explorer(L"cmd.exe /c start \"\" \"Z:\\a\" && calc.exe", target, 256, &reveal) == 0);

    unsigned short port = 0;
    char base_path[128];
    assert(levi_shell_parse_endpoint("http://127.0.0.1:54321/v1/shell/abc123", &port, base_path, sizeof base_path) == 1);
    assert(port == 54321 && !strcmp(base_path, "/v1/shell/abc123"));
    assert(levi_shell_parse_endpoint("http://example.com:80/v1/shell/abc", &port, base_path, sizeof base_path) == 0);

    char escaped[128];
    size_t pos = 0;
    assert(levi_json_append_escaped(escaped, sizeof escaped, &pos, "a\"b\\c\n\xe4\xb8\xad") == 1);
    assert(!strcmp(escaped, "a\\\"b\\\\c\\n\xe4\xb8\xad"));

    char pbuf[512];
    size_t offs[8];
    int cancelled = 1, count = 0;
    assert(levi_json_parse_dialog_response(
               "{\"cancelled\":false,\"paths\":[\"/Users/a/123\\u4e2d\\u6587/a.txt\",\"/Users/a/b \\\"2\\\".txt\"]}",
               &cancelled, pbuf, sizeof pbuf, offs, 8, &count) == 1);
    assert(cancelled == 0 && count == 2);
    assert(!strcmp(pbuf + offs[0], "/Users/a/123\xe4\xb8\xad\xe6\x96\x87/a.txt"));
    assert(!strcmp(pbuf + offs[1], "/Users/a/b \"2\".txt"));

    assert(levi_json_parse_dialog_response("{\"cancelled\":true,\"paths\":[]}", &cancelled, pbuf, sizeof pbuf, offs, 8, &count) == 1);
    assert(cancelled == 1 && count == 0);
    return 0;
}
