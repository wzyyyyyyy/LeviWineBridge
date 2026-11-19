/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LEVI_CLIENT_CONSOLE_BRIDGE_H
#define LEVI_CLIENT_CONSOLE_BRIDGE_H

#include <tlhelp32.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

typedef BOOL (WINAPI *GetConsoleModeFn)(HANDLE, LPDWORD);
typedef BOOL (WINAPI *SetConsoleModeFn)(HANDLE, DWORD);
typedef BOOL (WINAPI *WriteConsoleWFn)(HANDLE, const VOID *, DWORD, LPDWORD, LPVOID);
typedef BOOL (WINAPI *WriteConsoleAFn)(HANDLE, const VOID *, DWORD, LPDWORD, LPVOID);
typedef BOOL (WINAPI *ReadConsoleWFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
typedef BOOL (WINAPI *ReadConsoleAFn)(HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
typedef BOOL (WINAPI *GetConsoleSBIFn)(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO);
typedef BOOL (WINAPI *SetConsoleAttrFn)(HANDLE, WORD);
typedef DWORD (WINAPI *GetFileTypeFn)(HANDLE);

typedef int (__cdecl *CrtOpenOsfHandleFn)(intptr_t, int);
typedef int (__cdecl *CrtDup2Fn)(int, int);
typedef int (__cdecl *CrtCloseFn)(int);
typedef void *(__cdecl *CrtAcrtIobFuncFn)(unsigned);
typedef int (__cdecl *CrtSetvbufFn)(void *, char *, int, size_t);

static char console_addr[64];
static char console_token[128];
static unsigned short console_port;
static HMODULE console_self_module;
static HANDLE console_out_read;
static HANDLE console_out_write;
static HANDLE console_in_read;
static HANDLE console_in_write;
static HANDLE console_orig_out;
static HANDLE console_orig_err;
static HANDLE console_orig_in;
static HANDLE console_extra_out[4];
static HANDLE console_extra_in[2];
static volatile LONG console_ucrt_bound;
static volatile LONG console_msvcrt_bound;

static GetConsoleModeFn real_get_console_mode;
static SetConsoleModeFn real_set_console_mode;
static WriteConsoleWFn real_write_console_w;
static WriteConsoleAFn real_write_console_a;
static ReadConsoleWFn real_read_console_w;
static ReadConsoleAFn real_read_console_a;
static GetConsoleSBIFn real_get_console_sbi;
static SetConsoleAttrFn real_set_console_attr;
static GetFileTypeFn real_get_file_type;

static int console_is_out(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    if (h == console_out_write) return 1;
    if (console_orig_out && console_orig_out != INVALID_HANDLE_VALUE && h == console_orig_out) return 1;
    if (console_orig_err && console_orig_err != INVALID_HANDLE_VALUE && h == console_orig_err) return 1;
    for (size_t i = 0; i < sizeof console_extra_out / sizeof console_extra_out[0]; ++i) {
        if (console_extra_out[i] && h == console_extra_out[i]) return 1;
    }
    return 0;
}

static int console_is_in(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    if (h == console_in_read) return 1;
    if (console_orig_in && console_orig_in != INVALID_HANDLE_VALUE && h == console_orig_in) return 1;
    for (size_t i = 0; i < sizeof console_extra_in / sizeof console_extra_in[0]; ++i) {
        if (console_extra_in[i] && h == console_extra_in[i]) return 1;
    }
    return 0;
}

static void console_write_pipe(const char *data, DWORD len) {
    DWORD off = 0;
    while (off < len) {
        DWORD written = 0;
        if (!WriteFile(console_out_write, data + off, len - off, &written, NULL) || written == 0) break;
        off += written;
    }
}

static BOOL WINAPI bridge_get_console_mode(HANDLE h, LPDWORD mode) {
    if (console_is_out(h)) {
        if (mode) *mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return TRUE;
    }
    if (console_is_in(h)) {
        if (mode) *mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
        return TRUE;
    }
    return real_get_console_mode ? real_get_console_mode(h, mode) : FALSE;
}

static BOOL WINAPI bridge_set_console_mode(HANDLE h, DWORD mode) {
    if (console_is_out(h) || console_is_in(h)) return TRUE;
    return real_set_console_mode ? real_set_console_mode(h, mode) : FALSE;
}

static BOOL WINAPI bridge_write_console_w(HANDLE h, const VOID *buf, DWORD count, LPDWORD written, LPVOID reserved) {
    if (!console_is_out(h)) {
        return real_write_console_w ? real_write_console_w(h, buf, count, written, reserved) : FALSE;
    }
    if (count == 0) {
        if (written) *written = 0;
        return TRUE;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, (const WCHAR *)buf, (int)count, NULL, 0, NULL, NULL);
    if (needed > 0) {
        char stack_buf[1024];
        char *out = needed <= (int)sizeof stack_buf ? stack_buf : (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)needed);
        if (out) {
            int converted = WideCharToMultiByte(CP_UTF8, 0, (const WCHAR *)buf, (int)count, out, needed, NULL, NULL);
            if (converted > 0) console_write_pipe(out, (DWORD)converted);
            if (out != stack_buf) HeapFree(GetProcessHeap(), 0, out);
        }
    }
    if (written) *written = count;
    return TRUE;
}

static BOOL WINAPI bridge_write_console_a(HANDLE h, const VOID *buf, DWORD count, LPDWORD written, LPVOID reserved) {
    if (!console_is_out(h)) {
        return real_write_console_a ? real_write_console_a(h, buf, count, written, reserved) : FALSE;
    }
    if (count > 0 && buf) console_write_pipe((const char *)buf, count);
    if (written) *written = count;
    return TRUE;
}

static DWORD console_read_line_utf8(char *out, DWORD cap) {
    DWORD len = 0;
    while (len + 2 < cap) {
        char ch = 0;
        DWORD got = 0;
        if (!ReadFile(console_in_read, &ch, 1, &got, NULL) || got == 0) break;
        if (ch == '\n') {
            if (len == 0 || out[len - 1] != '\r') out[len++] = '\r';
            out[len++] = '\n';
            break;
        }
        out[len++] = ch;
    }
    return len;
}

static BOOL WINAPI bridge_read_console_a(HANDLE h, LPVOID buf, DWORD count, LPDWORD read_out, LPVOID control) {
    if (!console_is_in(h)) {
        return real_read_console_a ? real_read_console_a(h, buf, count, read_out, control) : FALSE;
    }
    if (!buf || count == 0) {
        if (read_out) *read_out = 0;
        return TRUE;
    }
    DWORD len = console_read_line_utf8((char *)buf, count);
    if (read_out) *read_out = len;
    return len > 0 ? TRUE : FALSE;
}

static BOOL WINAPI bridge_read_console_w(HANDLE h, LPVOID buf, DWORD count, LPDWORD read_out, LPVOID control) {
    if (!console_is_in(h)) {
        return real_read_console_w ? real_read_console_w(h, buf, count, read_out, control) : FALSE;
    }
    if (!buf || count == 0) {
        if (read_out) *read_out = 0;
        return TRUE;
    }
    char line[4096];
    DWORD len = console_read_line_utf8(line, (DWORD)sizeof line);
    if (len == 0) {
        if (read_out) *read_out = 0;
        return FALSE;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, line, (int)len, (WCHAR *)buf, (int)count);
    if (wlen <= 0) {
        if (read_out) *read_out = 0;
        return FALSE;
    }
    if (read_out) *read_out = (DWORD)wlen;
    return TRUE;
}

static BOOL WINAPI bridge_get_console_sbi(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info) {
    if (!console_is_out(h)) {
        return real_get_console_sbi ? real_get_console_sbi(h, info) : FALSE;
    }
    if (!info) return FALSE;
    memset(info, 0, sizeof *info);
    info->dwSize.X = 120;
    info->dwSize.Y = 30;
    info->wAttributes = 7;
    info->srWindow.Right = 119;
    info->srWindow.Bottom = 29;
    info->dwMaximumWindowSize.X = 120;
    info->dwMaximumWindowSize.Y = 30;
    return TRUE;
}

static BOOL WINAPI bridge_set_console_attr(HANDLE h, WORD attr) {
    static const int fg_map[16] = {
        30, 34, 32, 36, 31, 35, 33, 37,
        90, 94, 92, 96, 91, 95, 93, 97
    };
    static const int bg_map[16] = {
        40, 44, 42, 46, 41, 45, 43, 47,
        100, 104, 102, 106, 101, 105, 103, 107
    };
    if (!console_is_out(h)) {
        return real_set_console_attr ? real_set_console_attr(h, attr) : FALSE;
    }
    char seq[32];
    int fg = fg_map[attr & 0x0F];
    int bg_idx = (attr >> 4) & 0x0F;
    int len = bg_idx ? snprintf(seq, sizeof seq, "\x1b[0;%d;%dm", fg, bg_map[bg_idx])
                     : snprintf(seq, sizeof seq, "\x1b[0;%dm", fg);
    if (len > 0 && len < (int)sizeof seq) console_write_pipe(seq, (DWORD)len);
    return TRUE;
}

static DWORD WINAPI bridge_get_file_type(HANDLE h) {
    if (console_is_out(h) || console_is_in(h)) return FILE_TYPE_CHAR;
    return real_get_file_type ? real_get_file_type(h) : FILE_TYPE_UNKNOWN;
}

static int console_skip_module(HMODULE mod, const WCHAR *name) {
    if (!mod || mod == console_self_module) return 1;
    if (!name) return 1;
    static const WCHAR *const skipped[] = {
        L"ntdll.dll", L"kernel32.dll", L"kernelbase.dll",
        L"ws2_32.dll", L"winhttp.dll", L"ucrtbase.dll", L"msvcrt.dll",
        L"user32.dll", L"gdi32.dll", L"advapi32.dll", L"rpcrt4.dll",
        L"sechost.dll", L"ole32.dll", L"combase.dll", L"shell32.dll"
    };
    for (size_t i = 0; i < sizeof skipped / sizeof skipped[0]; ++i) {
        if (!_wcsicmp(name, skipped[i])) return 1;
    }
    return 0;
}

static void console_patch_module_iat(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        IMAGE_THUNK_DATA64 *lookup = (IMAGE_THUNK_DATA64 *)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            const char *sym = (const char *)imp->Name;
            PVOID replacement = NULL;
            if (!strcmp(sym, "GetConsoleMode")) replacement = (PVOID)&bridge_get_console_mode;
            else if (!strcmp(sym, "SetConsoleMode")) replacement = (PVOID)&bridge_set_console_mode;
            else if (!strcmp(sym, "WriteConsoleW")) replacement = (PVOID)&bridge_write_console_w;
            else if (!strcmp(sym, "WriteConsoleA")) replacement = (PVOID)&bridge_write_console_a;
            else if (!strcmp(sym, "ReadConsoleW")) replacement = (PVOID)&bridge_read_console_w;
            else if (!strcmp(sym, "ReadConsoleA")) replacement = (PVOID)&bridge_read_console_a;
            else if (!strcmp(sym, "GetConsoleScreenBufferInfo")) replacement = (PVOID)&bridge_get_console_sbi;
            else if (!strcmp(sym, "SetConsoleTextAttribute")) replacement = (PVOID)&bridge_set_console_attr;
            else if (!strcmp(sym, "GetFileType")) replacement = (PVOID)&bridge_get_file_type;
            if (!replacement || (PVOID)(ULONG_PTR)iat->u1.Function == replacement) continue;
            DWORD protection;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protection)) continue;
            InterlockedExchangePointer((PVOID volatile *)&iat->u1.Function, replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void *), protection, &protection);
        }
    }
}

static void console_track_handle(HANDLE *list, size_t count, HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    for (size_t i = 0; i < count; ++i) {
        if (list[i] == h) return;
        if (!list[i]) {
            list[i] = h;
            return;
        }
    }
}

static void console_rebind_crt(const WCHAR *dll_name, volatile LONG *bound_flag) {
    if (InterlockedCompareExchange(bound_flag, 0, 0) != 0) return;
    HMODULE crt = GetModuleHandleW(dll_name);
    if (!crt) return;
    CrtOpenOsfHandleFn open_osf = (CrtOpenOsfHandleFn)(void *)GetProcAddress(crt, "_open_osfhandle");
    CrtDup2Fn dup2_fn = (CrtDup2Fn)(void *)GetProcAddress(crt, "_dup2");
    CrtCloseFn close_fn = (CrtCloseFn)(void *)GetProcAddress(crt, "_close");
    if (!open_osf || !dup2_fn || !close_fn) return;
    if (InterlockedCompareExchange(bound_flag, 1, 0) != 0) return;
    HANDLE proc = GetCurrentProcess();
    HANDLE dup = NULL;
    if (DuplicateHandle(proc, console_in_read, proc, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        int fd = open_osf((intptr_t)dup, 0);
        if (fd >= 0) {
            dup2_fn(fd, 0);
            if (fd != 0) close_fn(fd);
            console_track_handle(console_extra_in, sizeof console_extra_in / sizeof console_extra_in[0], GetStdHandle(STD_INPUT_HANDLE));
        } else {
            CloseHandle(dup);
        }
    }
    for (int target = 1; target <= 2; ++target) {
        dup = NULL;
        if (DuplicateHandle(proc, console_out_write, proc, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            int fd = open_osf((intptr_t)dup, 0);
            if (fd >= 0) {
                dup2_fn(fd, target);
                if (fd != target) close_fn(fd);
                console_track_handle(console_extra_out, sizeof console_extra_out / sizeof console_extra_out[0],
                                     GetStdHandle(target == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE));
            } else {
                CloseHandle(dup);
            }
        }
    }
    /* Restore canonical std handles after CRT _dup2 (which mutates Win32 std handles). */
    SetStdHandle(STD_OUTPUT_HANDLE, console_out_write);
    SetStdHandle(STD_ERROR_HANDLE, console_out_write);
    SetStdHandle(STD_INPUT_HANDLE, console_in_read);
    CrtAcrtIobFuncFn iob_fn = (CrtAcrtIobFuncFn)(void *)GetProcAddress(crt, "__acrt_iob_func");
    CrtSetvbufFn setvbuf_fn = (CrtSetvbufFn)(void *)GetProcAddress(crt, "setvbuf");
    if (iob_fn && setvbuf_fn) {
        void *out_stream = iob_fn(1);
        void *err_stream = iob_fn(2);
        if (out_stream) setvbuf_fn(out_stream, NULL, _IONBF, 0);
        if (err_stream) setvbuf_fn(err_stream, NULL, _IONBF, 0);
    }
}

static void console_scan_modules(void) {
    console_rebind_crt(L"ucrtbase.dll", &console_ucrt_bound);
    console_rebind_crt(L"msvcrt.dll", &console_msvcrt_bound);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof entry;
    if (Module32FirstW(snap, &entry)) {
        do {
            if (!console_skip_module(entry.hModule, entry.szModule)) {
                console_patch_module_iat(entry.hModule);
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static DWORD WINAPI console_watch_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 150; ++i) {
        Sleep(200);
        console_scan_modules();
    }
    return 0;
}

static int console_send_all(SOCKET sock, const char *data, int len) {
    int off = 0;
    while (off < len) {
        int sent = send(sock, data + off, len - off, 0);
        if (sent <= 0) return 0;
        off += sent;
    }
    return 1;
}

static SOCKET console_connect_socket(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return INVALID_SOCKET;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(console_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (int attempt = 0; attempt < 10; ++attempt) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return INVALID_SOCKET;
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
        DWORD timeout = 3000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        if (connect(sock, (struct sockaddr *)&sa, sizeof sa) == 0) {
            char hello[160];
            int hlen = snprintf(hello, sizeof hello, "GAME %s\n", console_token);
            char reply[16];
            int rlen = 0;
            if (hlen > 0 && hlen < (int)sizeof hello && console_send_all(sock, hello, hlen)) {
                while (rlen < (int)sizeof reply - 1) {
                    int n = recv(sock, reply + rlen, 1, 0);
                    if (n <= 0) break;
                    rlen += n;
                    if (reply[rlen - 1] == '\n') break;
                }
                reply[rlen] = 0;
                if (!strcmp(reply, "OK\n")) {
                    timeout = 0;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
                    return sock;
                }
            }
        }
        closesocket(sock);
        Sleep(100);
    }
    return INVALID_SOCKET;
}

static DWORD WINAPI console_input_thread(void *arg) {
    SOCKET sock = (SOCKET)(ULONG_PTR)arg;
    char buf[1024];
    for (;;) {
        int n = recv(sock, buf, (int)sizeof buf, 0);
        if (n <= 0) break;
        DWORD off = 0;
        while (off < (DWORD)n) {
            DWORD written = 0;
            if (!WriteFile(console_in_write, buf + off, (DWORD)n - off, &written, NULL) || written == 0) {
                return 0;
            }
            off += written;
        }
    }
    return 0;
}

static DWORD WINAPI console_bridge_thread(void *unused) {
    (void)unused;
    HMODULE pinned = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)&console_bridge_thread, &pinned);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        real_get_console_mode = (GetConsoleModeFn)(void *)GetProcAddress(k32, "GetConsoleMode");
        real_set_console_mode = (SetConsoleModeFn)(void *)GetProcAddress(k32, "SetConsoleMode");
        real_write_console_w = (WriteConsoleWFn)(void *)GetProcAddress(k32, "WriteConsoleW");
        real_write_console_a = (WriteConsoleAFn)(void *)GetProcAddress(k32, "WriteConsoleA");
        real_read_console_w = (ReadConsoleWFn)(void *)GetProcAddress(k32, "ReadConsoleW");
        real_read_console_a = (ReadConsoleAFn)(void *)GetProcAddress(k32, "ReadConsoleA");
        real_get_console_sbi = (GetConsoleSBIFn)(void *)GetProcAddress(k32, "GetConsoleScreenBufferInfo");
        real_set_console_attr = (SetConsoleAttrFn)(void *)GetProcAddress(k32, "SetConsoleTextAttribute");
        real_get_file_type = (GetFileTypeFn)(void *)GetProcAddress(k32, "GetFileType");
    }
    console_scan_modules();
    HANDLE watcher = CreateThread(NULL, 0, console_watch_thread, NULL, 0, NULL);
    if (watcher) CloseHandle(watcher);

    SOCKET sock = console_connect_socket();
    if (sock != INVALID_SOCKET) {
        OutputDebugStringA("LeviWineBridge: console bridge connected\n");
        HANDLE input_thread = CreateThread(NULL, 0, console_input_thread, (void *)(ULONG_PTR)sock, 0, NULL);
        if (input_thread) CloseHandle(input_thread);
    }

    /* Always keep draining the output pipe so the game can never block on a full pipe. */
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(console_out_read, buf, (DWORD)sizeof buf, &n, NULL) || n == 0) break;
        if (sock != INVALID_SOCKET) {
            if (!console_send_all(sock, buf, (int)n)) {
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
        }
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    return 0;
}

static int console_parse_addr(const char *addr, unsigned short *port_out) {
    const char *prefix = "127.0.0.1:";
    size_t plen = strlen(prefix);
    if (!addr || strncmp(addr, prefix, plen) != 0) return 0;
    char *end = NULL;
    unsigned long port = strtoul(addr + plen, &end, 10);
    if (!end || *end != '\0' || port == 0 || port > 65535) return 0;
    *port_out = (unsigned short)port;
    return 1;
}

static int start_console_bridge(HMODULE module) {
    DWORD a = GetEnvironmentVariableA("LEVI_CONSOLE_ADDR", console_addr, sizeof console_addr);
    DWORD b = GetEnvironmentVariableA("LEVI_CONSOLE_TOKEN", console_token, sizeof console_token);
    if (!a || a >= sizeof console_addr || !b || b >= sizeof console_token) return 0;
    if (!console_parse_addr(console_addr, &console_port)) return 0;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&console_out_read, &console_out_write, &sa, 65536)) return 0;
    if (!CreatePipe(&console_in_read, &console_in_write, &sa, 65536)) {
        CloseHandle(console_out_read);
        CloseHandle(console_out_write);
        return 0;
    }
    SetHandleInformation(console_out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(console_in_write, HANDLE_FLAG_INHERIT, 0);

    console_self_module = module;
    console_orig_out = GetStdHandle(STD_OUTPUT_HANDLE);
    console_orig_err = GetStdHandle(STD_ERROR_HANDLE);
    console_orig_in = GetStdHandle(STD_INPUT_HANDLE);

    SetStdHandle(STD_OUTPUT_HANDLE, console_out_write);
    SetStdHandle(STD_ERROR_HANDLE, console_out_write);
    SetStdHandle(STD_INPUT_HANDLE, console_in_read);

    HANDLE thread = CreateThread(NULL, 0, console_bridge_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

#endif
