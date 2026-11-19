#include <windows.h>
#include <stdio.h>
static HANDLE heap_locked;
static LONG volatile heap_released;
static int fx_shift_up;
static int fx_w_up;
static int fx_cmd_v_ctrl;

static LRESULT CALLBACK fx_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYUP:
        if (wParam == VK_SHIFT) ++fx_shift_up;
        if (wParam == 'W') ++fx_w_up;
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) fx_cmd_v_ctrl = 1;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
static DWORD WINAPI hold_heap(void *unused) {
    (void)unused;
    HeapLock(GetProcessHeap());
    SetEvent(heap_locked);
    Sleep(500);
    InterlockedExchange(&heap_released, 1);
    HeapUnlock(GetProcessHeap());
    Sleep(1000);
    return 0;
}
int main(void) {
    HMODULE preloader = LoadLibraryW(L"PreLoader.dll");
    HMODULE runtime = LoadLibraryW(L"xgameruntime.dll");
    HMODULE client = LoadLibraryW(L"LeviWineBridge.dll");
    Sleep(500);
    int prompt = -1, other = -1;
    if (runtime && client) {
        int (*show)(void) = (int (*)(void))GetProcAddress(runtime, "fixture_prompt");
        int (*unrelated)(void) = (int (*)(void))GetProcAddress(runtime, "fixture_other");
        void (*result)(void) = (void (*)(void))GetProcAddress(runtime, "fixture_result");
        if (show && unrelated && result) { prompt = show(); other = unrelated(); result(); }
    }
    int guard = -1;
    char environment[8];
    if (preloader && GetEnvironmentVariableA("LEVI_GAME_AUTH_TOKEN", environment, sizeof environment)) {
        DWORD (*suspend)(HANDLE) = (DWORD (*)(HANDLE))GetProcAddress(preloader, "fixture_suspend");
        heap_locked = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE worker = CreateThread(NULL, 0, hold_heap, NULL, 0, NULL);
        WaitForSingleObject(heap_locked, INFINITE);
        DWORD count = suspend(worker);
        guard = count == 0 && InterlockedCompareExchange(&heap_released, 0, 0) == 1;
        if (guard) {
            void *allocation = HeapAlloc(GetProcessHeap(), 0, 1048576);
            guard = allocation != NULL;
            HeapFree(GetProcessHeap(), 0, allocation);
        }
        ResumeThread(worker);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker); CloseHandle(heap_locked);
    }
    char path[MAX_PATH], body[160];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *name = strrchr(path, '\\');
    if (!name) return 1;
    int console_ok = -1;
    if (GetEnvironmentVariableA("LEVI_CONSOLE_TOKEN", environment, sizeof environment)) {
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD cmode = 0, written = 0, rchars = 0;
        int vt_ok = GetConsoleMode(hout, &cmode) && (cmode & 0x0004);
        int type_ok = GetFileType(hout) == FILE_TYPE_CHAR;
        SetConsoleTextAttribute(hout, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        const WCHAR msg[] = L"\x4F60\x597D-W\n";
        int w_ok = WriteConsoleW(hout, msg, (DWORD)(wcslen(msg)), &written, NULL) && written == wcslen(msg);
        printf("printf-ok\n");
        fflush(stdout);
        WCHAR wline[64] = {0};
        int rw_ok = ReadConsoleW(hin, wline, 63, &rchars, NULL) && rchars > 0 && wcsncmp(wline, L"cmd1", 4) == 0;
        char aline[64] = {0};
        int fg_ok = fgets(aline, (int)sizeof aline, stdin) != NULL && strncmp(aline, "cmd2", 4) == 0;
        printf("echo:%s", aline);
        fflush(stdout);
        Sleep(200);
        console_ok = (vt_ok && type_ok && w_ok && rw_ok && fg_ok) ? 1 : 0;
    }
    int input_ok = -1;
    if (GetEnvironmentVariableA("LEVI_WINE_INPUT_FIX", environment, sizeof environment) && environment[0] == '1') {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = fx_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"LeviInputFixtureWindow";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    100, 100, 400, 300, NULL, NULL, wc.hInstance, NULL);
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hwnd);
            /* Wait for LeviWineBridge's input watcher to subclass this window. */
            for (int i = 0; i < 20; ++i) {
                if ((WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != fx_wndproc) break;
                Sleep(100);
            }
            /* Stuck Shift + W released on WM_KILLFOCUS. */
            SendMessageW(hwnd, WM_KEYDOWN, VK_SHIFT, 0);
            SendMessageW(hwnd, WM_KEYDOWN, 'W', 0);
            SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
            /* Cmd+V translated to Ctrl+V. */
            SendMessageW(hwnd, WM_KEYDOWN, VK_LWIN, 0);
            SendMessageW(hwnd, WM_KEYDOWN, 'V', 0);
            SendMessageW(hwnd, WM_KEYUP, 'V', 0);
            SendMessageW(hwnd, WM_KEYUP, VK_LWIN, 0);
            DestroyWindow(hwnd);
            input_ok = (fx_shift_up == 1 && fx_w_up == 1 && fx_cmd_v_ctrl == 1) ? 1 : 0;
        } else {
            input_ok = 0;
        }
    }
    strcpy(name + 1, "result.txt");
    int length = input_ok >= 0
        ? snprintf(body, sizeof body, "prompt=%d other=%d guard=%d input=%d\n", prompt, other, guard, input_ok)
        : (console_ok >= 0
            ? snprintf(body, sizeof body, "prompt=%d other=%d guard=%d console=%d\n", prompt, other, guard, console_ok)
            : snprintf(body, sizeof body, "prompt=%d other=%d guard=%d\n", prompt, other, guard));
    HANDLE output = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return 2;
    DWORD written;
    WriteFile(output, body, (DWORD)length, &written, NULL);
    CloseHandle(output);
    return 0;
}
