/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LEVI_WINE_BRIDGE_INPUT_FIX_H
#define LEVI_WINE_BRIDGE_INPUT_FIX_H

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC 0
#endif

typedef struct {
    int accum;
    ULONGLONG last_event_ms;
    ULONGLONG last_step_ms;
} LeviWheelState;

/* Quantize high-frequency trackpad/Magic Mouse scroll deltas into discrete
 * WHEEL_DELTA steps while preserving zero-latency discrete scroll wheels.
 */
static short levi_wheel_filter(LeviWheelState *st, int delta, ULONGLONG now_ms) {
    if (!st || delta == 0) return 0;
    if (st->last_event_ms == 0 ||
        now_ms - st->last_event_ms > 250 ||
        (st->accum > 0 && delta < 0) ||
        (st->accum < 0 && delta > 0)) {
        st->accum = 0;
    }
    st->last_event_ms = now_ms;
    st->accum += delta;
    if (st->accum > -WHEEL_DELTA && st->accum < WHEEL_DELTA) {
        return 0;
    }
    if (st->last_step_ms != 0 && now_ms - st->last_step_ms < 55) {
        st->accum = st->accum > 0 ? (WHEEL_DELTA - 1) : -(WHEEL_DELTA - 1);
        return 0;
    }
    short step = st->accum > 0 ? WHEEL_DELTA : -WHEEL_DELTA;
    st->accum = 0;
    st->last_step_ms = now_ms;
    return step;
}

/* Drop anomalous single-packet mouse jumps right after regaining focus. */
static int levi_raw_mouse_is_spike(LONG dx, LONG dy, USHORT flags, ULONGLONG now_ms, ULONGLONG guard_until_ms) {
    if (now_ms > guard_until_ms) return 0;
    if (flags & MOUSE_MOVE_ABSOLUTE) return 0;
    return dx > 180 || dx < -180 || dy > 180 || dy < -180;
}

#ifndef LEVI_INPUT_FIX_PURE_TEST

static HWND volatile input_hwnd;
static WNDPROC volatile input_orig_wndproc;
static LeviWheelState input_wheel_state;
static ULONGLONG input_focus_guard_until_ms;
static DWORD input_held_mask;
static int input_wheel_enabled = 1;
static int input_cmd_shortcut_down;

static const UINT input_tracked_vks[] = {
    VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
    VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
    VK_MENU, VK_LMENU, VK_RMENU,
    VK_LWIN, VK_RWIN,
    'W', 'A', 'S', 'D', VK_SPACE
};

static int input_tracked_index(UINT vk) {
    for (size_t i = 0; i < sizeof input_tracked_vks / sizeof input_tracked_vks[0]; ++i) {
        if (input_tracked_vks[i] == vk) return (int)i;
    }
    return -1;
}

static LPARAM input_keyup_lparam(UINT vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    return (LPARAM)(1u | ((sc & 0xFFu) << 16) | (1u << 30) | (1u << 31));
}

static void input_release_stuck_keys(HWND hwnd, WNDPROC orig) {
    DWORD mask = input_held_mask;
    input_held_mask = 0;
    input_cmd_shortcut_down = 0;
    input_wheel_state.accum = 0;
    if (!mask || !orig) return;
    for (size_t i = 0; i < sizeof input_tracked_vks / sizeof input_tracked_vks[0]; ++i) {
        if (mask & (1u << i)) {
            UINT vk = input_tracked_vks[i];
            CallWindowProcW(orig, hwnd, WM_KEYUP, (WPARAM)vk, input_keyup_lparam(vk));
        }
    }
}

static int input_is_clipboard_vk(UINT vk) {
    return vk == 'C' || vk == 'V' || vk == 'X' || vk == 'A';
}

static LRESULT CALLBACK input_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = (WNDPROC)InterlockedCompareExchangePointer((PVOID volatile *)&input_orig_wndproc, NULL, NULL);
    if (!orig) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_NCDESTROY:
        input_held_mask = 0;
        input_cmd_shortcut_down = 0;
        InterlockedCompareExchangePointer((PVOID volatile *)&input_hwnd, NULL, (PVOID)hwnd);
        break;

    case WM_SETFOCUS:
        input_focus_guard_until_ms = GetTickCount64() + 180;
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            input_release_stuck_keys(hwnd, orig);
        } else {
            input_focus_guard_until_ms = GetTickCount64() + 180;
        }
        break;

    case WM_KILLFOCUS:
        input_release_stuck_keys(hwnd, orig);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wParam;
        int idx = input_tracked_index(vk);
        if (idx >= 0) input_held_mask |= (1u << idx);
        if (vk == VK_LWIN || vk == VK_RWIN) {
            input_cmd_shortcut_down = 1;
        } else if (input_is_clipboard_vk(vk) &&
                   (input_cmd_shortcut_down ||
                    (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                    (GetAsyncKeyState(VK_RWIN) & 0x8000)) &&
                   !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            /* Translate macOS Cmd+C/V/X/A into Ctrl+C/V/X/A for in-game text fields. */
            BYTE state[256];
            if (GetKeyboardState(state)) {
                BYTE prev_ctrl = state[VK_CONTROL];
                BYTE prev_lctrl = state[VK_LCONTROL];
                state[VK_CONTROL] |= 0x80;
                state[VK_LCONTROL] |= 0x80;
                SetKeyboardState(state);
                LRESULT res = CallWindowProcW(orig, hwnd, WM_KEYDOWN, wParam, lParam);
                state[VK_CONTROL] = prev_ctrl;
                state[VK_LCONTROL] = prev_lctrl;
                SetKeyboardState(state);
                return res;
            }
        }
        break;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        UINT vk = (UINT)wParam;
        int idx = input_tracked_index(vk);
        if (idx >= 0) input_held_mask &= ~(1u << idx);
        if (vk == VK_LWIN || vk == VK_RWIN) input_cmd_shortcut_down = 0;
        break;
    }

    case WM_MOUSEWHEEL:
        if (input_wheel_enabled) {
            short step = levi_wheel_filter(&input_wheel_state, (short)HIWORD(wParam), GetTickCount64());
            if (step == 0) return 0;
            wParam = MAKEWPARAM(LOWORD(wParam), (WORD)step);
        }
        break;

    case WM_INPUT: {
        ULONGLONG now = GetTickCount64();
        if (now <= input_focus_guard_until_ms) {
            RAWINPUT raw;
            UINT size = sizeof raw;
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                raw.header.dwType == RIM_TYPEMOUSE &&
                levi_raw_mouse_is_spike(raw.data.mouse.lLastX, raw.data.mouse.lLastY,
                                        raw.data.mouse.usFlags, now, input_focus_guard_until_ms)) {
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            }
        }
        break;
    }

    default:
        break;
    }

    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

typedef struct {
    DWORD pid;
    HWND found;
    LONG best_area;
} InputWindowSearch;

static BOOL CALLBACK input_enum_windows(HWND hwnd, LPARAM lParam) {
    InputWindowSearch *search = (InputWindowSearch *)lParam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    LONG w = rc.right - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w < 200 || h < 200) return TRUE;
    LONG area = w * h;
    if (area > search->best_area) {
        search->best_area = area;
        search->found = hwnd;
    }
    return TRUE;
}

static void input_try_subclass(void) {
    HWND current = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&input_hwnd, NULL, NULL);
    if (current && IsWindow(current)) return;

    InputWindowSearch search;
    search.pid = GetCurrentProcessId();
    search.found = NULL;
    search.best_area = 0;
    EnumWindows(input_enum_windows, (LPARAM)&search);
    if (!search.found) return;

    WNDPROC existing = (WNDPROC)GetWindowLongPtrW(search.found, GWLP_WNDPROC);
    if (!existing || existing == input_wndproc) return;

    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)&input_wndproc, &pinned)) {
        return;
    }

    InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, (PVOID)existing);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(search.found, GWLP_WNDPROC, (LONG_PTR)&input_wndproc);
    if (!prev) {
        InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, NULL);
        return;
    }
    if (prev != existing) {
        InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, (PVOID)prev);
    }
    InterlockedExchangePointer((PVOID volatile *)&input_hwnd, (PVOID)search.found);
    OutputDebugStringA("LeviWineBridge: Wine input fix ready\n");
}

static DWORD WINAPI input_fix_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 300; ++i) {
        input_try_subclass();
        Sleep(200);
    }
    return 0;
}

static int start_input_fix(void) {
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char flag[16];
    DWORD n = GetEnvironmentVariableA("LEVI_WINE_INPUT_FIX", flag, sizeof flag);
    if (n > 0 && n < sizeof flag && !strcmp(flag, "0")) return 0;

    /* Activate when explicitly requested or when launched with any LeviLauncher bridge. */
    char tmp[8];
    int active = (n > 0 && n < sizeof flag) ||
                 GetEnvironmentVariableA("LEVI_GAME_AUTH_TOKEN", tmp, sizeof tmp) > 0 ||
                 GetEnvironmentVariableA("LEVI_CONSOLE_TOKEN", tmp, sizeof tmp) > 0;
    if (!active) return 0;

    n = GetEnvironmentVariableA("LEVI_WINE_WHEEL_FIX", flag, sizeof flag);
    if (n > 0 && n < sizeof flag && !strcmp(flag, "0")) {
        input_wheel_enabled = 0;
    }

    HANDLE thread = CreateThread(NULL, 0, input_fix_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

#endif /* LEVI_INPUT_FIX_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_INPUT_FIX_H */
