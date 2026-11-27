/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LEVI_WINE_BRIDGE_INPUT_FIX_H
#define LEVI_WINE_BRIDGE_INPUT_FIX_H

#ifndef MAPVK_VK_TO_VSC
#define MAPVK_VK_TO_VSC 0
#endif

/* Drop anomalous single-packet mouse jumps right after regaining focus. */
static int levi_raw_mouse_is_spike(LONG dx, LONG dy, USHORT flags, ULONGLONG now_ms, ULONGLONG guard_until_ms) {
    if (now_ms > guard_until_ms) return 0;
    if (flags & MOUSE_MOVE_ABSOLUTE) return 0;
    return dx > 180 || dx < -180 || dy > 180 || dy < -180;
}

#ifndef LEVI_INPUT_FIX_PURE_TEST

typedef UINT (WINAPI *GetRawInputDataFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

static HWND volatile input_hwnd;
static WNDPROC volatile input_orig_wndproc;
static ULONGLONG volatile input_focus_guard_until_ms;
static DWORD input_held_mask;
static int input_cmd_shortcut_down;
static GetRawInputDataFn real_get_raw_input_data;

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

static UINT WINAPI bridge_get_raw_input_data(HRAWINPUT hRawInput, UINT uiCommand,
                                             LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    if (!real_get_raw_input_data) return (UINT)-1;
    UINT ret = real_get_raw_input_data(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (ret == (UINT)-1 || uiCommand != RID_INPUT || !pData ||
        ret < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        return ret;
    }
    RAWINPUT *raw = (RAWINPUT *)pData;
    if (raw->header.dwType == RIM_TYPEMOUSE &&
        levi_raw_mouse_is_spike(raw->data.mouse.lLastX, raw->data.mouse.lLastY,
                                raw->data.mouse.usFlags, GetTickCount64(),
                                input_focus_guard_until_ms)) {
        raw->data.mouse.lLastX = 0;
        raw->data.mouse.lLastY = 0;
    }
    return ret;
}

static LRESULT CALLBACK input_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = (WNDPROC)InterlockedCompareExchangePointer((PVOID volatile *)&input_orig_wndproc, NULL, NULL);
    if (!orig) return DefWindowProcW(hwnd, msg, wParam, lParam);

    (void)render_scale_adjust_message(hwnd, msg, &lParam);

    switch (msg) {
    case WM_NCDESTROY:
        input_held_mask = 0;
        input_cmd_shortcut_down = 0;
        InterlockedCompareExchangePointer((PVOID volatile *)&input_hwnd, NULL, (PVOID)hwnd);
        InterlockedCompareExchangePointer((PVOID volatile *)&g_scale_hwnd, NULL, (PVOID)hwnd);
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

    case WM_INPUT: {
        if (!real_get_raw_input_data) {
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
        }
        break;
    }

    default:
        break;
    }

    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

static void input_patch_exe_iat(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!exe || !user32) return;

    if (!real_get_raw_input_data) {
        real_get_raw_input_data = (GetRawInputDataFn)(void *)GetProcAddress(user32, "GetRawInputData");
    }
    if (!real_get_raw_input_data) return;

    BYTE *base = (BYTE *)exe;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;

    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)&bridge_get_raw_input_data, &pinned)) {
        return;
    }

    for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        IMAGE_THUNK_DATA64 *lookup = (IMAGE_THUNK_DATA64 *)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            if (strcmp((const char *)imp->Name, "GetRawInputData") != 0) continue;
            if ((PVOID)(ULONG_PTR)iat->u1.Function == (PVOID)&bridge_get_raw_input_data) return;
            DWORD protection;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protection)) return;
            InterlockedExchangePointer((PVOID volatile *)&iat->u1.Function, (PVOID)&bridge_get_raw_input_data);
            VirtualProtect(&iat->u1.Function, sizeof(void *), protection, &protection);
            return;
        }
    }
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

static void input_try_subclass_hwnd(HWND target) {
    if (!target || !IsWindow(target)) return;
    HWND current = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&input_hwnd, NULL, NULL);
    if (current == target && IsWindow(current)) return;

    WNDPROC existing = (WNDPROC)GetWindowLongPtrW(target, GWLP_WNDPROC);
    if (!existing || existing == input_wndproc) return;

    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)&input_wndproc, &pinned)) {
        return;
    }

    InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, (PVOID)existing);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(target, GWLP_WNDPROC, (LONG_PTR)&input_wndproc);
    if (!prev) {
        InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, NULL);
        return;
    }
    if (prev != existing) {
        InterlockedExchangePointer((PVOID volatile *)&input_orig_wndproc, (PVOID)prev);
    }
    InterlockedExchangePointer((PVOID volatile *)&input_hwnd, (PVOID)target);
    OutputDebugStringA("LeviWineBridge: Wine input fix ready\n");
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
    input_try_subclass_hwnd(search.found);
}

static DWORD WINAPI input_fix_thread(void *unused) {
    (void)unused;
    input_patch_exe_iat();
    for (int i = 0; i < 300; ++i) {
        input_try_subclass();
        if (render_scale_is_active()) {
            render_scale_patch_loaded_mods();
        }
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
    if (n > 0 && n < sizeof flag && !strcmp(flag, "0") && !render_scale_is_active()) return 0;

    /* Activate when explicitly requested, when render scaling is active, or when launched with any LeviLauncher bridge. */
    char tmp[8];
    int active = render_scale_is_active() ||
                 (n > 0 && n < sizeof flag) ||
                 GetEnvironmentVariableA("LEVI_GAME_AUTH_TOKEN", tmp, sizeof tmp) > 0 ||
                 GetEnvironmentVariableA("LEVI_CONSOLE_TOKEN", tmp, sizeof tmp) > 0;
    if (!active) return 0;

    HANDLE thread = CreateThread(NULL, 0, input_fix_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

#endif /* LEVI_INPUT_FIX_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_INPUT_FIX_H */
