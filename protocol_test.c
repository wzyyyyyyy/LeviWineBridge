#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"

typedef uint64_t ULONGLONG;
typedef int32_t LONG;
typedef uint16_t USHORT;
#define MOUSE_MOVE_ABSOLUTE 1
#define LEVI_HEAP_GUARD_PURE_TEST
#include "heap_guard.h"
#define LEVI_INPUT_FIX_PURE_TEST
#include "input_fix.h"
#define LEVI_RENDER_SCALE_PURE_TEST
#include "render_scale.h"
#define LEVI_DLSS_QUALITY_PURE_TEST
#include "dlss_quality.h"
#define LEVI_GPTK4_RTX_FIX_PURE_TEST
#include "gptk4_rtx_fix.h"
#define LEVI_SHELL_FIX_PURE_TEST
#include "shell_fix.h"

typedef struct MockNgxParam {
    const LeviNgxParameterVtbl *lpVtbl;
    void *callback;
    int perf_quality;
    int seen_quality_in_cb;
    unsigned int out_w;
    unsigned int out_h;
} MockNgxParam;

static void mock_set_void(LeviNgxParameter *self, const char *name, void *value) {
    MockNgxParam *m = (MockNgxParam *)self;
    if (!strcmp(name, "DLSSOptimalSettingsCallback")) m->callback = value;
}

static void mock_set_int(LeviNgxParameter *self, const char *name, int value) {
    MockNgxParam *m = (MockNgxParam *)self;
    if (!strcmp(name, "PerfQualityValue")) m->perf_quality = value;
}

static uint32_t mock_get_void(const LeviNgxParameter *self, const char *name, void **out) {
    const MockNgxParam *m = (const MockNgxParam *)self;
    if (!strcmp(name, "DLSSOptimalSettingsCallback") && m->callback) {
        *out = m->callback;
        return LEVI_NGX_RESULT_SUCCESS;
    }
    return 0xBAD00001u;
}

static uint32_t mock_apple_optimal_cb(LeviNgxParameter *self) {
    MockNgxParam *m = (MockNgxParam *)self;
    m->seen_quality_in_cb = m->perf_quality;
    if (m->perf_quality == LEVI_NGX_PERF_QUALITY_PERFORMANCE) {
        m->out_w = 1280;
        m->out_h = 720;
    } else {
        m->out_w = 1706;
        m->out_h = 960;
    }
    return LEVI_NGX_RESULT_SUCCESS;
}

static LeviNgxOptimalSettingsCb g_test_orig_cb = NULL;
static uint32_t mock_wrapped_optimal_cb(LeviNgxParameter *self) {
    levi_ngx_apply_perf_quality(self, LEVI_NGX_PERF_QUALITY_PERFORMANCE);
    return g_test_orig_cb(self);
}

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

    /* GPTK DLSS quality override helpers. */
    assert(levi_parse_gptk_dlss_quality(NULL) == -1);
    assert(levi_parse_gptk_dlss_quality("") == -1);
    assert(levi_parse_gptk_dlss_quality("auto") == -1);
    assert(levi_parse_gptk_dlss_quality("quality") == -1);
    assert(levi_parse_gptk_dlss_quality("invalid") == -1);
    assert(levi_parse_gptk_dlss_quality("ultra_performance") == LEVI_NGX_PERF_QUALITY_ULTRA_PERFORMANCE);
    assert(levi_parse_gptk_dlss_quality("performance") == LEVI_NGX_PERF_QUALITY_PERFORMANCE);
    assert(levi_parse_gptk_dlss_quality("BALANCED") == LEVI_NGX_PERF_QUALITY_BALANCED);
    assert(levi_parse_gptk_dlss_quality("  dlaa\n") == LEVI_NGX_PERF_QUALITY_DLAA);
    assert(!strcmp(levi_dlss_quality_name(LEVI_NGX_PERF_QUALITY_ULTRA_PERFORMANCE), "ultra_performance"));
    assert(!strcmp(levi_dlss_quality_name(LEVI_NGX_PERF_QUALITY_PERFORMANCE), "performance"));
    assert(!strcmp(levi_dlss_quality_name(LEVI_NGX_PERF_QUALITY_BALANCED), "balanced"));
    assert(!strcmp(levi_dlss_quality_name(LEVI_NGX_PERF_QUALITY_DLAA), "dlaa"));
    assert(!strcmp(levi_dlss_quality_name(-1), "unknown"));

    assert(levi_dlss_is_target_dll(L"nvngx-on-metalfx.dll") == 1);
    assert(levi_dlss_is_target_dll(L"C:\\windows\\system32\\NVNGX-ON-METALFX.DLL") == 1);
    assert(levi_dlss_is_target_dll(L"nvngx.dll") == 1);
    assert(levi_dlss_is_target_dll(L"C:\\windows\\system32\\_NVNGX.DLL") == 1);
    assert(levi_dlss_is_target_dll(L"nvngx_dlss.dll") == 0);
    assert(levi_dlss_is_target_dll(NULL) == 0);

    /* GPTK 3 stub: mov rax, [rip+0x1ab1]; test rax, rax; je +3; jmp rax */
    static const uint8_t gptk3_stub[] = {
        0x48, 0x8b, 0x05, 0xb1, 0x1a, 0x00, 0x00,
        0x48, 0x85, 0xc0, 0x74, 0x03, 0x48, 0xff, 0xe0
    };
    int32_t disp = 0;
    size_t next_off = 0;
    assert(levi_find_rip_mov_rax_slot(gptk3_stub, sizeof gptk3_stub, &disp, &next_off) == 1);
    assert(disp == 0x1ab1 && next_off == 7);

    /* GPTK 4 beta 3 stub: push rsi; sub rsp, 0x30; mov rax, [rip+0x22ac]; test rax, rax */
    static const uint8_t gptk4_stub[] = {
        0x56, 0x48, 0x83, 0xec, 0x30,
        0x48, 0x8b, 0x05, 0xac, 0x22, 0x00, 0x00,
        0x48, 0x85, 0xc0, 0x74, 0x08
    };
    assert(levi_find_rip_mov_rax_slot(gptk4_stub, sizeof gptk4_stub, &disp, &next_off) == 1);
    assert(disp == 0x22ac && next_off == 12);

    /* Mock NVSDK_NGX_Parameter callback wrapping & PerfQualityValue rewrite. */
    LeviNgxParameterVtbl vtbl;
    memset(&vtbl, 0, sizeof vtbl);
    vtbl.SetVoidPointer = mock_set_void;
    vtbl.SetInt = mock_set_int;
    vtbl.GetVoidPointer = mock_get_void;
    MockNgxParam mock_param = { &vtbl, (void *)mock_apple_optimal_cb, LEVI_NGX_PERF_QUALITY_QUALITY, -1, 0, 0 };
    assert(levi_ngx_wrap_optimal_callback((LeviNgxParameter *)&mock_param,
                                          mock_wrapped_optimal_cb, &g_test_orig_cb) == 1);
    assert(g_test_orig_cb == mock_apple_optimal_cb);
    assert(mock_param.callback == (void *)mock_wrapped_optimal_cb);
    LeviNgxOptimalSettingsCb cb = (LeviNgxOptimalSettingsCb)mock_param.callback;
    assert(cb((LeviNgxParameter *)&mock_param) == LEVI_NGX_RESULT_SUCCESS);
    assert(mock_param.seen_quality_in_cb == LEVI_NGX_PERF_QUALITY_PERFORMANCE);
    assert(mock_param.out_w == 1280 && mock_param.out_h == 720);

    /* GPTK 4 RTX heap storage-mode fix helpers. */
    assert(levi_rtx_should_activate(NULL, NULL, NULL) == 0);
    assert(levi_rtx_should_activate(NULL, "1", NULL) == 0);
    assert(levi_rtx_should_activate(NULL, "0", "/runtime/d3dmetal-gptk4-beta3") == 0);
    assert(levi_rtx_should_activate(NULL, "1", "/runtime/d3dmetal-gptk4-beta3") == 1);
    assert(levi_rtx_should_activate("0", "1", "/runtime/d3dmetal-gptk4-beta3") == 0);
    assert(levi_rtx_should_activate("1", NULL, NULL) == 1);

    assert(levi_rtx_is_d3d12_dll(L"d3d12.dll") == 1);
    assert(levi_rtx_is_d3d12_dll(L"C:\\windows\\system32\\D3D12.DLL") == 1);
    assert(levi_rtx_is_d3d12_dll(L"d3d12core.dll") == 0);
    assert(levi_rtx_is_d3d12_dll(NULL) == 0);

    LeviD3D12HeapDesc custom_gpu_heap = {
        1048576ull,
        { LEVI_D3D12_HEAP_TYPE_CUSTOM, LEVI_D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE, 2u, 1u, 1u },
        65536ull,
        0x40u,
        0u
    };
    LeviD3D12HeapDesc norm_heap;
    memset(&norm_heap, 0, sizeof norm_heap);
    assert(levi_rtx_normalize_heap_desc(&custom_gpu_heap, &norm_heap) == 1);
    assert(norm_heap.SizeInBytes == 1048576ull);
    assert(norm_heap.Alignment == 65536ull);
    assert(norm_heap.Flags == 0x40u);
    assert(norm_heap.Properties.Type == LEVI_D3D12_HEAP_TYPE_DEFAULT);
    assert(norm_heap.Properties.CPUPageProperty == LEVI_D3D12_CPU_PAGE_PROPERTY_UNKNOWN);
    assert(norm_heap.Properties.MemoryPoolPreference == LEVI_D3D12_MEMORY_POOL_UNKNOWN);
    assert(norm_heap.Properties.CreationNodeMask == 1u && norm_heap.Properties.VisibleNodeMask == 1u);

    /* CPU-accessible CUSTOM heap (WRITE_COMBINE = 2) must NOT be rewritten to DEFAULT. */
    LeviD3D12HeapDesc custom_cpu_heap = custom_gpu_heap;
    custom_cpu_heap.Properties.CPUPageProperty = 2u;
    assert(levi_rtx_normalize_heap_desc(&custom_cpu_heap, &norm_heap) == 0);

    /* Non-CUSTOM heaps remain untouched. */
    LeviD3D12HeapDesc default_heap = custom_gpu_heap;
    default_heap.Properties.Type = LEVI_D3D12_HEAP_TYPE_DEFAULT;
    default_heap.Properties.CPUPageProperty = 0u;
    assert(levi_rtx_normalize_heap_desc(&default_heap, &norm_heap) == 0);

    /* Placed AS fallback only triggers for AS resources on non-DEFAULT heaps. */
    assert(levi_rtx_needs_placed_as_fallback(LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &custom_cpu_heap) == 1);
    assert(levi_rtx_needs_placed_as_fallback(LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &default_heap) == 0);
    assert(levi_rtx_needs_placed_as_fallback(0u, &custom_cpu_heap) == 0);

    /* GPTK 3 D3D12CreateDevice stub: mov rax, [rip+0x1df1]; rex.W jmp rax */
    static const uint8_t d3d12_gptk3_stub[] = {
        0x48, 0x8b, 0x05, 0xf1, 0x1d, 0x00, 0x00,
        0x48, 0xff, 0xe0
    };
    assert(levi_find_d3d12_create_device_slot(d3d12_gptk3_stub, sizeof d3d12_gptk3_stub, &disp, &next_off) == 1);
    assert(disp == 0x1df1 && next_off == 7);

    /* GPTK 4 beta 3 D3D12CreateDevice stub: first slot is EnsureDXGIInitialized (jne + call),
     * second slot is gGFXTDispatch (je + jmp rax) and must be the one selected. */
    static const uint8_t d3d12_gptk4_stub[] = {
        0x48, 0x8b, 0x05, 0xe4, 0xa5, 0x00, 0x00, /* mov rax, [rip+0xa5e4] (EnsureDXGIInitialized) */
        0x48, 0x85, 0xc0,                         /* test rax, rax */
        0x75, 0x04,                               /* jne +4 */
        0x90, 0x90, 0x90, 0x90,
        0xff, 0xd0,                               /* call rax */
        0x48, 0x8b, 0x05, 0xa8, 0x35, 0x00, 0x00, /* mov rax, [rip+0x35a8] (gGFXTDispatch) */
        0x48, 0x85, 0xc0,                         /* test rax, rax */
        0x74, 0x08,                               /* je +8 */
        0x48, 0x89, 0xf1,                         /* mov rcx, rsi */
        0x48, 0x83, 0xc4, 0x60,                   /* add rsp, 0x60 */
        0x48, 0xff, 0xe0                          /* rex.W jmp rax */
    };
    assert(levi_find_d3d12_create_device_slot(d3d12_gptk4_stub, sizeof d3d12_gptk4_stub, &disp, &next_off) == 1);
    assert(disp == 0x35a8 && next_off == 25);

    /* SuspendThread heap/loader guard target module matching. */
    const wchar_t *gdir = L"Z:\\Users\\test\\Instances\\123\\";
    size_t gdir_len = wcslen(gdir);
    assert(levi_heap_guard_is_target_module(L"Z:\\Other\\PreLoader.dll", L"PreLoader.dll", gdir, gdir_len) == 1);
    assert(levi_heap_guard_is_target_module(L"Z:\\Users\\test\\Instances\\123\\mods\\LeviLamina\\LeviLamina.dll", L"LeviLamina.dll", gdir, gdir_len) == 1);
    assert(levi_heap_guard_is_target_module(L"Z:\\Users\\test\\Instances\\123\\mods\\LeviOptimize\\LeviOptimize.dll", L"LeviOptimize.dll", gdir, gdir_len) == 1);
    assert(levi_heap_guard_is_target_module(L"Z:\\Users\\test\\Instances\\123\\mods\\LeviClient\\LeviWineBridge.dll", L"LeviWineBridge.dll", gdir, gdir_len) == 0);
    assert(levi_heap_guard_is_target_module(L"Z:\\Users\\test\\Instances\\123\\Minecraft.Windows.exe", L"Minecraft.Windows.exe", gdir, gdir_len) == 0);
    assert(levi_heap_guard_is_target_module(L"C:\\windows\\system32\\kernel32.dll", L"kernel32.dll", gdir, gdir_len) == 0);
    return 0;
}
