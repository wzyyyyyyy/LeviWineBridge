/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LEVI_WINE_BRIDGE_FONT_FIX_H
#define LEVI_WINE_BRIDGE_FONT_FIX_H

static DWORD WINAPI font_fix_thread(void *unused) {
    (void)unused;
    HMODULE pinned = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)&font_fix_thread, &pinned);

    const WCHAR *dst = L"C:\\Windows\\Fonts\\msyh.ttc";
    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
        return 0;
    }

    static const WCHAR *const candidates[] = {
        L"Z:\\System\\Library\\Fonts\\Hiragino Sans GB.ttc",
        L"Z:\\System\\Library\\Fonts\\STHeiti Light.ttc",
        L"Z:\\System\\Library\\Fonts\\STHeiti Medium.ttc",
        L"Z:\\System\\Library\\Fonts\\PingFang.ttc",
        L"Z:\\System\\Library\\Fonts\\Supplemental\\Songti.ttc",
    };

    const WCHAR *src = NULL;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        DWORD attr = GetFileAttributesW(candidates[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            src = candidates[i];
            break;
        }
    }
    if (!src) return 0;

    CreateDirectoryW(L"C:\\Windows\\Fonts", NULL);

    WCHAR tmp[MAX_PATH];
    int n = _snwprintf(tmp, MAX_PATH, L"C:\\Windows\\Fonts\\msyh.ttc.%lu.tmp",
                       (unsigned long)GetCurrentProcessId());
    if (n <= 0 || n >= MAX_PATH) return 0;

    if (!CopyFileW(src, tmp, FALSE)) {
        DeleteFileW(tmp);
        return 0;
    }

    /* Another process may have finished installing msyh.ttc while we copied. */
    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tmp);
        return 0;
    }

    if (MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING)) {
        OutputDebugStringA("LeviWineBridge: installed macOS CJK fallback font as msyh.ttc\n");
    } else {
        DeleteFileW(tmp);
    }
    return 0;
}

static int start_font_fix(void) {
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char flag[16];
    DWORD n = GetEnvironmentVariableA("LEVI_WINE_FONT_FIX", flag, sizeof flag);
    if (n > 0 && n < sizeof flag && !strcmp(flag, "0")) return 0;

    HANDLE thread = CreateThread(NULL, 0, font_fix_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

#endif
