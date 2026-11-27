/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Virtual window/input scaling for Wine D3DMetal + MetalFX spatial upscaling.
 *
 * Activated only under Wine when LEVI_WINE_RENDER_SCALE > 1.01 (e.g. 1.33, 1.50, 2.00).
 * Scales the game's reported client dimensions down by 1/scale and maps client-space
 * pointer coordinates cleanly so Minecraft renders at W/scale x H/scale while the
 * macOS native helper (libLeviMetalFX.dylib) upscales zero-copy via MTLFXSpatialScaler.
 */
#ifndef LEVI_WINE_BRIDGE_RENDER_SCALE_H
#define LEVI_WINE_BRIDGE_RENDER_SCALE_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double levi_parse_render_scale(const char *value) {
    if (!value || !value[0]) return 1.0;
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (!end || end == value || !isfinite(parsed) || parsed <= 1.01 || parsed > 4.0) {
        return 1.0;
    }
    return parsed;
}

static LONG levi_scale_down_dim(LONG value, double scale) {
    if (scale <= 1.01 || value <= 64) return value;
    LONG scaled = (LONG)llround((double)value / scale);
    if (scaled < 64) scaled = 64;
    if (scaled >= value) scaled = value - 1;
    return scaled;
}

static LONG levi_scale_down_coord(LONG value, double scale) {
    if (scale <= 1.01) return value;
    return (LONG)llround((double)value / scale);
}

static LONG levi_scale_up_coord(LONG value, double scale) {
    if (scale <= 1.01) return value;
    return (LONG)llround((double)value * scale);
}

#ifndef LEVI_RENDER_SCALE_PURE_TEST

#include <tlhelp32.h>

typedef BOOL (WINAPI *GetClientRectFn)(HWND, LPRECT);
typedef BOOL (WINAPI *ScreenToClientFn)(HWND, LPPOINT);
typedef BOOL (WINAPI *ClientToScreenFn)(HWND, LPPOINT);
typedef HWND (WINAPI *CreateWindowExWFn)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef LRESULT (WINAPI *DispatchMessageWFn)(const MSG *);

static double g_render_scale = 1.0;
static HWND volatile g_scale_hwnd = NULL;
static HMODULE g_scale_self_module = NULL;
static WCHAR g_scale_game_dir[MAX_PATH];
static size_t g_scale_game_dir_len = 0;
static volatile LONG g_dispatch_msg_hooked = 0;
static GetClientRectFn real_get_client_rect = NULL;
static ScreenToClientFn real_screen_to_client = NULL;
static ClientToScreenFn real_client_to_screen = NULL;
static CreateWindowExWFn real_create_window_ex_w = NULL;
static DispatchMessageWFn real_dispatch_message_w = NULL;

static void input_try_subclass_hwnd(HWND hwnd);

static int render_scale_is_active(void) {
    return g_render_scale > 1.01;
}

static int render_scale_is_target_hwnd(HWND hwnd) {
    if (!hwnd || !render_scale_is_active()) return 0;
    HWND current = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&g_scale_hwnd, NULL, NULL);
    if (hwnd == current) return 1;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return 0;
    RECT rc;
    GetClientRectFn fn = real_get_client_rect ? real_get_client_rect : GetClientRect;
    if (!fn(hwnd, &rc)) return 0;
    LONG w = rc.right - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w < 200 || h < 200) return 0;
    InterlockedExchangePointer((PVOID volatile *)&g_scale_hwnd, (PVOID)hwnd);
    return 1;
}

static BOOL WINAPI bridge_get_client_rect(HWND hwnd, LPRECT lpRect) {
    GetClientRectFn fn = real_get_client_rect ? real_get_client_rect : GetClientRect;
    if (!fn(hwnd, lpRect)) return FALSE;
    if (lpRect && render_scale_is_target_hwnd(hwnd)) {
        LONG w = lpRect->right - lpRect->left;
        LONG h = lpRect->bottom - lpRect->top;
        if (w >= 200 && h >= 200) {
            lpRect->right = lpRect->left + levi_scale_down_dim(w, g_render_scale);
            lpRect->bottom = lpRect->top + levi_scale_down_dim(h, g_render_scale);
        }
    }
    return TRUE;
}

static BOOL WINAPI bridge_screen_to_client(HWND hwnd, LPPOINT lpPoint) {
    ScreenToClientFn fn = real_screen_to_client ? real_screen_to_client : ScreenToClient;
    if (!fn(hwnd, lpPoint)) return FALSE;
    if (lpPoint && render_scale_is_target_hwnd(hwnd)) {
        lpPoint->x = levi_scale_down_coord(lpPoint->x, g_render_scale);
        lpPoint->y = levi_scale_down_coord(lpPoint->y, g_render_scale);
    }
    return TRUE;
}

static BOOL WINAPI bridge_client_to_screen(HWND hwnd, LPPOINT lpPoint) {
    ClientToScreenFn fn = real_client_to_screen ? real_client_to_screen : ClientToScreen;
    if (!lpPoint || !render_scale_is_target_hwnd(hwnd)) {
        return fn(hwnd, lpPoint);
    }
    POINT scaled;
    scaled.x = levi_scale_up_coord(lpPoint->x, g_render_scale);
    scaled.y = levi_scale_up_coord(lpPoint->y, g_render_scale);
    if (!fn(hwnd, &scaled)) return FALSE;
    *lpPoint = scaled;
    return TRUE;
}

static HWND WINAPI bridge_create_window_ex_w(DWORD exStyle, LPCWSTR className, LPCWSTR windowName,
                                             DWORD style, int x, int y, int w, int h,
                                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    CreateWindowExWFn fn = real_create_window_ex_w ? real_create_window_ex_w : CreateWindowExW;
    /* Minecraft persists its last client dimensions using GetClientRect (which we scale down),
     * so restore saved top-level window dimensions back to physical pixels on launch. */
    if (render_scale_is_active() && !(style & WS_CHILD) &&
        w != CW_USEDEFAULT && h != CW_USEDEFAULT && w >= 200 && h >= 200) {
        RECT work = {0, 0, 0, 0};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            int maxW = (int)(work.right - work.left);
            int maxH = (int)(work.bottom - work.top);
            int scaledW = (int)levi_scale_up_coord((LONG)w, g_render_scale);
            int scaledH = (int)levi_scale_up_coord((LONG)h, g_render_scale);
            if (maxW >= 200 && scaledW > maxW) scaledW = maxW;
            if (maxH >= 200 && scaledH > maxH) scaledH = maxH;
            w = scaledW;
            h = scaledH;
            if (x != CW_USEDEFAULT && x + w > work.right) x = (int)work.left;
            if (y != CW_USEDEFAULT && y + h > work.bottom) y = (int)work.top;
        }
    }
    HWND hwnd = fn(exStyle, className, windowName, style, x, y, w, h, parent, menu, inst, param);
    if (hwnd && !(style & WS_CHILD) && (w == CW_USEDEFAULT || (w >= 200 && h >= 200))) {
        InterlockedExchangePointer((PVOID volatile *)&g_scale_hwnd, (PVOID)hwnd);
        input_try_subclass_hwnd(hwnd);
    }
    return hwnd;
}

static int render_scale_is_client_mouse_msg(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        return 1;
    default:
        return 0;
    }
}

static LRESULT WINAPI bridge_dispatch_message_w(const MSG *lpMsg) {
    DispatchMessageWFn fn = real_dispatch_message_w ? real_dispatch_message_w : DispatchMessageW;
    if (lpMsg && render_scale_is_active() &&
        render_scale_is_client_mouse_msg(lpMsg->message) &&
        render_scale_is_target_hwnd(lpMsg->hwnd)) {
        MSG scaled = *lpMsg;
        LONG x = (LONG)(short)LOWORD(scaled.lParam);
        LONG y = (LONG)(short)HIWORD(scaled.lParam);
        WORD sx = (WORD)(short)levi_scale_down_coord(x, g_render_scale);
        WORD sy = (WORD)(short)levi_scale_down_coord(y, g_render_scale);
        scaled.lParam = MAKELPARAM(sx, sy);
        return fn(&scaled);
    }
    return fn(lpMsg);
}

static int render_scale_adjust_message(HWND hwnd, UINT msg, LPARAM *lParam) {
    if (!lParam || !render_scale_is_active()) return 0;
    if (!render_scale_is_target_hwnd(hwnd)) return 0;
    if (msg == WM_SIZE) {
        LONG w = (LONG)LOWORD(*lParam);
        LONG h = (LONG)HIWORD(*lParam);
        if (w >= 64 && h >= 64) {
            WORD sw = (WORD)levi_scale_down_dim(w, g_render_scale);
            WORD sh = (WORD)levi_scale_down_dim(h, g_render_scale);
            *lParam = MAKELPARAM(sw, sh);
            return 1;
        }
        return 0;
    }
    /* When DispatchMessageW is hooked, queued mouse messages are already scaled once
     * before reaching any subclassed WndProc (including mod overlays such as arke). */
    if (!InterlockedCompareExchange(&g_dispatch_msg_hooked, 0, 0) &&
        render_scale_is_client_mouse_msg(msg)) {
        LONG x = (LONG)(short)LOWORD(*lParam);
        LONG y = (LONG)(short)HIWORD(*lParam);
        WORD sx = (WORD)(short)levi_scale_down_coord(x, g_render_scale);
        WORD sy = (WORD)(short)levi_scale_down_coord(y, g_render_scale);
        *lParam = MAKELPARAM(sx, sy);
        return 1;
    }
    return 0;
}

static int render_scale_patch_module_iat(HMODULE mod, int isMainExe) {
    if (!mod || mod == g_scale_self_module) return 0;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return 0;

    if (!real_get_client_rect) real_get_client_rect = (GetClientRectFn)(void *)GetProcAddress(user32, "GetClientRect");
    if (!real_screen_to_client) real_screen_to_client = (ScreenToClientFn)(void *)GetProcAddress(user32, "ScreenToClient");
    if (!real_client_to_screen) real_client_to_screen = (ClientToScreenFn)(void *)GetProcAddress(user32, "ClientToScreen");
    if (!real_create_window_ex_w) real_create_window_ex_w = (CreateWindowExWFn)(void *)GetProcAddress(user32, "CreateWindowExW");
    if (!real_dispatch_message_w) real_dispatch_message_w = (DispatchMessageWFn)(void *)GetProcAddress(user32, "DispatchMessageW");

    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return 0;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;

    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)&bridge_get_client_rect, &pinned)) {
        return 0;
    }

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        IMAGE_THUNK_DATA64 *lookup = (IMAGE_THUNK_DATA64 *)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            const char *sym = (const char *)imp->Name;
            PVOID replacement = NULL;
            if (!strcmp(sym, "GetClientRect") && real_get_client_rect) {
                replacement = (PVOID)&bridge_get_client_rect;
            } else if (!strcmp(sym, "ScreenToClient") && real_screen_to_client) {
                replacement = (PVOID)&bridge_screen_to_client;
            } else if (!strcmp(sym, "ClientToScreen") && real_client_to_screen) {
                replacement = (PVOID)&bridge_client_to_screen;
            } else if (isMainExe && !strcmp(sym, "CreateWindowExW") && real_create_window_ex_w) {
                replacement = (PVOID)&bridge_create_window_ex_w;
            } else if (isMainExe && !strcmp(sym, "DispatchMessageW") && real_dispatch_message_w) {
                replacement = (PVOID)&bridge_dispatch_message_w;
            }
            if (!replacement || (PVOID)(ULONG_PTR)iat->u1.Function == replacement) continue;
            DWORD protection;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protection)) continue;
            InterlockedExchangePointer((PVOID volatile *)&iat->u1.Function, replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void *), protection, &protection);
            if (replacement == (PVOID)&bridge_dispatch_message_w) {
                InterlockedExchange(&g_dispatch_msg_hooked, 1);
            }
            patched++;
        }
    }
    return patched;
}

static int render_scale_is_game_module_path(const WCHAR *path) {
    if (!path || g_scale_game_dir_len == 0) return 0;
    return _wcsnicmp(path, g_scale_game_dir, g_scale_game_dir_len) == 0;
}

static void render_scale_patch_loaded_mods(void) {
    if (!render_scale_is_active() || g_scale_game_dir_len == 0) return;
    HMODULE exe = GetModuleHandleW(NULL);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me)) {
        do {
            if (!me.hModule || me.hModule == exe || me.hModule == g_scale_self_module) continue;
            if (!render_scale_is_game_module_path(me.szExePath)) continue;
            (void)render_scale_patch_module_iat(me.hModule, 0);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static int start_render_scale(HMODULE selfModule) {
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char raw[32];
    DWORD n = GetEnvironmentVariableA("LEVI_WINE_RENDER_SCALE", raw, sizeof raw);
    if (n == 0 || n >= sizeof raw) return 0;
    double scale = levi_parse_render_scale(raw);
    if (scale <= 1.01) return 0;

    g_scale_self_module = selfModule;
    g_render_scale = scale;

    HMODULE exe = GetModuleHandleW(NULL);
    DWORD len = exe ? GetModuleFileNameW(exe, g_scale_game_dir, MAX_PATH) : 0;
    if (len > 0 && len < MAX_PATH) {
        WCHAR *slash = wcsrchr(g_scale_game_dir, L'\\');
        if (slash) {
            slash[1] = L'\0';
            g_scale_game_dir_len = (size_t)(slash + 1 - g_scale_game_dir);
        }
    }

    /* Patch main EXE IAT synchronously during DllMain so the very first window & swapchain use scaled dimensions. */
    int patched = render_scale_patch_module_iat(exe, 1);
    if (patched > 0) {
        char msg[128];
        snprintf(msg, sizeof msg, "LeviWineBridge: virtual render scale %.2fx ready (%d hooks)\n", g_render_scale, patched);
        OutputDebugStringA(msg);
    }
    return 1;
}

#endif /* LEVI_RENDER_SCALE_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_RENDER_SCALE_H */
