#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
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
    int shell_ok = -1;
    if (GetEnvironmentVariableA("LEVI_SHELL_BRIDGE_TOKEN", environment, sizeof environment)) {
        WCHAR mod_dir[MAX_PATH];
        GetModuleFileNameW(NULL, mod_dir, MAX_PATH);
        WCHAR *wslash = wcsrchr(mod_dir, L'\\');
        if (wslash) *wslash = 0;

        WCHAR shader_dir[MAX_PATH];
        _snwprintf(shader_dir, MAX_PATH, L"%ls\\123\x4E2D\x6587\\shaderpacks", mod_dir);
        WCHAR parent_dir[MAX_PATH];
        _snwprintf(parent_dir, MAX_PATH, L"%ls\\123\x4E2D\x6587", mod_dir);
        CreateDirectoryW(parent_dir, NULL);
        CreateDirectoryW(shader_dir, NULL);

        WCHAR sample_file[MAX_PATH];
        _snwprintf(sample_file, MAX_PATH, L"%ls\\pack.zip", shader_dir);
        HANDLE sf = CreateFileW(sample_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (sf != INVALID_HANDLE_VALUE) CloseHandle(sf);

        /* 1. ShellExecuteW on directory (exact Arke pattern) */
        int se_w = (INT_PTR)ShellExecuteW(NULL, L"open", shader_dir, NULL, NULL, SW_SHOWNORMAL) > 32;

        /* 2. ShellExecuteA on ASCII directory (exact Dear ImGui OpenInShell pattern) */
        char mod_dir_a[MAX_PATH];
        GetModuleFileNameA(NULL, mod_dir_a, MAX_PATH);
        char *aslash = strrchr(mod_dir_a, '\\');
        if (aslash) *aslash = 0;
        int se_a = (INT_PTR)ShellExecuteA(NULL, "open", mod_dir_a, NULL, NULL, SW_SHOWDEFAULT) > 32;

        /* 3. ShellExecuteExW explorer.exe /select,"<file>" */
        WCHAR sel_params[MAX_PATH + 32];
        _snwprintf(sel_params, MAX_PATH + 32, L"/select,\"%ls\"", sample_file);
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof sei);
        sei.cbSize = sizeof sei;
        sei.lpVerb = L"open";
        sei.lpFile = L"explorer.exe";
        sei.lpParameters = sel_params;
        sei.nShow = SW_SHOWNORMAL;
        int see_w = ShellExecuteExW(&sei) ? 1 : 0;

        /* 4. CreateProcessW explorer.exe "<dir>" */
        WCHAR cp_cmd[MAX_PATH + 64];
        _snwprintf(cp_cmd, MAX_PATH + 64, L"explorer.exe \"%ls\"", shader_dir);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        int cp_w = CreateProcessW(NULL, cp_cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) ? 1 : 0;
        if (cp_w) {
            WaitForSingleObject(pi.hProcess, 1000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        /* 5. GetOpenFileNameW (exact Arke pattern) */
        WCHAR open_buf[1024];
        open_buf[0] = 0;
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFilter = L"Shader Config (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = open_buf;
        ofn.nMaxFile = 1024;
        ofn.lpstrInitialDir = shader_dir;
        ofn.lpstrDefExt = L"txt";
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        int ofn_open = GetOpenFileNameW(&ofn) && wcsstr(open_buf, L"picked.txt") != NULL && ofn.nFileOffset > 0 && ofn.nFileExtension > ofn.nFileOffset;

        /* 6. GetSaveFileNameW cancelled */
        WCHAR save_buf[1024];
        wcscpy(save_buf, L"out.txt");
        OPENFILENAMEW sfn;
        memset(&sfn, 0, sizeof sfn);
        sfn.lStructSize = sizeof sfn;
        sfn.lpstrFile = save_buf;
        sfn.nMaxFile = 1024;
        sfn.lpstrInitialDir = shader_dir;
        sfn.lpstrDefExt = L"txt";
        sfn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
        int ofn_save_cancel = GetSaveFileNameW(&sfn) == FALSE;

        /* 7. SHBrowseForFolderW */
        WCHAR disp[MAX_PATH];
        disp[0] = 0;
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.pszDisplayName = disp;
        bi.lpszTitle = L"Pick folder";
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        WCHAR picked_folder[MAX_PATH];
        picked_folder[0] = 0;
        int browse_ok = 0;
        if (pidl) {
            browse_ok = SHGetPathFromIDListW(pidl, picked_folder) && wcsstr(picked_folder, L"shaderpacks") != NULL;
            ILFree(pidl);
        }

        shell_ok = (se_w && se_a && see_w && cp_w && ofn_open && ofn_save_cancel && browse_ok) ? 1 : 0;
    }
    strcpy(name + 1, "result.txt");
    int length = shell_ok >= 0
        ? snprintf(body, sizeof body, "prompt=%d other=%d guard=%d shell=%d\n", prompt, other, guard, shell_ok)
        : (input_ok >= 0
            ? snprintf(body, sizeof body, "prompt=%d other=%d guard=%d input=%d\n", prompt, other, guard, input_ok)
            : (console_ok >= 0
                ? snprintf(body, sizeof body, "prompt=%d other=%d guard=%d console=%d\n", prompt, other, guard, console_ok)
                : snprintf(body, sizeof body, "prompt=%d other=%d guard=%d\n", prompt, other, guard)));
    HANDLE output = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) return 2;
    DWORD written;
    WriteFile(output, body, (DWORD)length, &written, NULL);
    CloseHandle(output);
    return 0;
}
