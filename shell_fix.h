/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LEVI_WINE_BRIDGE_SHELL_FIX_H
#define LEVI_WINE_BRIDGE_SHELL_FIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------------- Pure helpers (unit-testable on host) ---------------- */

static int levi_wascii_tolower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (int)(c - L'A' + L'a') : (int)c;
}

static int levi_wstr_ieq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (levi_wascii_tolower(*a) != levi_wascii_tolower(*b)) return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int levi_wstr_iprefix(const wchar_t *s, const wchar_t *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (levi_wascii_tolower(*s) != levi_wascii_tolower(*prefix)) return 0;
        ++s;
        ++prefix;
    }
    return 1;
}

static int levi_shell_is_open_verb(const wchar_t *verb) {
    if (!verb || !verb[0]) return 1;
    return levi_wstr_ieq(verb, L"open") || levi_wstr_ieq(verb, L"explore");
}

static int levi_shell_is_explorer_exe(const wchar_t *path) {
    if (!path) return 0;
    while (*path == L' ' || *path == L'\t') ++path;
    wchar_t buf[512];
    size_t len = 0;
    if (*path == L'"') {
        ++path;
        while (*path && *path != L'"' && len + 1 < sizeof buf / sizeof buf[0]) {
            buf[len++] = *path++;
        }
    } else {
        while (*path && len + 1 < sizeof buf / sizeof buf[0]) {
            buf[len++] = *path++;
        }
        while (len > 0 && (buf[len - 1] == L' ' || buf[len - 1] == L'\t')) --len;
    }
    buf[len] = 0;
    const wchar_t *base = buf;
    for (const wchar_t *p = buf; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    return levi_wstr_ieq(base, L"explorer") || levi_wstr_ieq(base, L"explorer.exe");
}

static int levi_shell_is_cmd_exe(const wchar_t *path) {
    if (!path) return 0;
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    return levi_wstr_ieq(base, L"cmd") || levi_wstr_ieq(base, L"cmd.exe");
}

static int levi_shell_copy_unquoted(const wchar_t *src, wchar_t *out, size_t out_cap) {
    if (!src || !out || out_cap < 2) return 0;
    while (*src == L' ' || *src == L'\t') ++src;
    if (!*src) return 0;
    size_t len = 0;
    if (*src == L'"') {
        ++src;
        while (*src && *src != L'"') {
            if (len + 1 >= out_cap) return 0;
            out[len++] = *src++;
        }
    } else {
        while (*src) {
            if (len + 1 >= out_cap) return 0;
            out[len++] = *src++;
        }
        while (len > 0 && (out[len - 1] == L' ' || out[len - 1] == L'\t')) --len;
    }
    if (len == 0) return 0;
    out[len] = 0;
    return 1;
}

static int levi_shell_parse_explorer_args(const wchar_t *args, wchar_t *out_path, size_t out_cap, int *out_reveal) {
    if (!args || !out_path || out_cap < 2 || !out_reveal) return 0;
    *out_reveal = 0;
    const wchar_t *p = args;
    for (;;) {
        while (*p == L' ' || *p == L'\t' || *p == L',') ++p;
        if (!*p) return 0;
        if (levi_wstr_iprefix(p, L"/desktop") || levi_wstr_iprefix(p, L"/idlist")) return 0;
        if (levi_wstr_iprefix(p, L"/select,")) {
            p += 8;
            *out_reveal = 1;
            break;
        }
        if (levi_wstr_iprefix(p, L"/select") && (p[7] == L' ' || p[7] == L'\t')) {
            p += 7;
            *out_reveal = 1;
            break;
        }
        if (levi_wstr_iprefix(p, L"/root,")) {
            p += 6;
            continue;
        }
        if (levi_wstr_iprefix(p, L"/separate") && (p[9] == 0 || p[9] == L' ' || p[9] == L'\t' || p[9] == L',')) {
            p += 9;
            continue;
        }
        if ((p[0] == L'/') && (levi_wascii_tolower(p[1]) == 'e' || levi_wascii_tolower(p[1]) == 'n') &&
            (p[2] == 0 || p[2] == L' ' || p[2] == L'\t' || p[2] == L',')) {
            p += 2;
            continue;
        }
        if (p[0] == L'/') return 0;
        break;
    }
    if (!levi_shell_copy_unquoted(p, out_path, out_cap)) return 0;
    if (levi_wstr_iprefix(out_path, L"shell:") || levi_wstr_iprefix(out_path, L"::{")) return 0;
    return 1;
}

static const wchar_t *levi_shell_next_token(const wchar_t *p, wchar_t *tok, size_t tok_cap) {
    if (!p || !tok || tok_cap < 2) return NULL;
    while (*p == L' ' || *p == L'\t') ++p;
    if (!*p) return NULL;
    size_t len = 0;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') {
            if (len + 1 >= tok_cap) return NULL;
            tok[len++] = *p++;
        }
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') {
            if (len + 1 >= tok_cap) return NULL;
            tok[len++] = *p++;
        }
    }
    tok[len] = 0;
    return p;
}

static int levi_shell_has_shell_metachars(const wchar_t *s) {
    for (; s && *s; ++s) {
        if (*s == L'|' || *s == L'&' || *s == L'>' || *s == L'<' || *s == L';') return 1;
    }
    return 0;
}

static int levi_shell_parse_cmdline_explorer(const wchar_t *cmdline, wchar_t *out_path, size_t out_cap, int *out_reveal) {
    if (!cmdline || !out_path || out_cap < 2 || !out_reveal) return 0;
    wchar_t exe[512];
    const wchar_t *rest = levi_shell_next_token(cmdline, exe, sizeof exe / sizeof exe[0]);
    if (!rest) return 0;
    if (levi_shell_is_explorer_exe(exe)) {
        return levi_shell_parse_explorer_args(rest, out_path, out_cap, out_reveal);
    }
    if (levi_shell_is_cmd_exe(exe)) {
        if (levi_shell_has_shell_metachars(rest)) return 0;
        wchar_t flag[64], sub[512];
        const wchar_t *p = levi_shell_next_token(rest, flag, sizeof flag / sizeof flag[0]);
        if (!p || !levi_wstr_ieq(flag, L"/c")) return 0;
        p = levi_shell_next_token(p, sub, sizeof sub / sizeof sub[0]);
        if (!p) return 0;
        if (levi_shell_is_explorer_exe(sub)) {
            return levi_shell_parse_explorer_args(p, out_path, out_cap, out_reveal);
        }
        if (levi_wstr_ieq(sub, L"start")) {
            /* Skip optional window title ("") and /flags before the target path. */
            for (;;) {
                while (*p == L' ' || *p == L'\t') ++p;
                if (!*p) return 0;
                if (p[0] == L'"' && p[1] == L'"' && (p[2] == 0 || p[2] == L' ' || p[2] == L'\t')) {
                    p += 2;
                    continue;
                }
                if (p[0] == L'/') {
                    wchar_t skip[64];
                    p = levi_shell_next_token(p, skip, sizeof skip / sizeof skip[0]);
                    if (!p) return 0;
                    continue;
                }
                break;
            }
            *out_reveal = 0;
            return levi_shell_copy_unquoted(p, out_path, out_cap);
        }
    }
    return 0;
}

static int levi_shell_parse_endpoint(const char *url, unsigned short *port_out, char *path_out, size_t path_cap) {
    const char *prefix = "http://127.0.0.1:";
    size_t plen = strlen(prefix);
    if (!url || strncmp(url, prefix, plen) != 0) return 0;
    char *path = NULL;
    unsigned long port = strtoul(url + plen, &path, 10);
    if (port == 0 || port > 65535 || !path || strncmp(path, "/v1/shell/", 10) != 0) return 0;
    size_t len = strlen(path);
    if (len == 0 || len >= path_cap) return 0;
    *port_out = (unsigned short)port;
    memcpy(path_out, path, len + 1);
    return 1;
}

static int levi_json_append_raw(char *dst, size_t cap, size_t *pos, const char *s) {
    size_t len = strlen(s);
    if (*pos + len >= cap) return 0;
    memcpy(dst + *pos, s, len);
    *pos += len;
    dst[*pos] = 0;
    return 1;
}

static int levi_json_append_escaped(char *dst, size_t cap, size_t *pos, const char *src) {
    if (!src) return 1;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        unsigned char c = *p;
        const char *esc = NULL;
        char ubuf[7];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(ubuf, sizeof ubuf, "\\u%04x", (unsigned)c);
            esc = ubuf;
        }
        if (esc) {
            if (!levi_json_append_raw(dst, cap, pos, esc)) return 0;
        } else {
            if (*pos + 1 >= cap) return 0;
            dst[(*pos)++] = (char)c;
            dst[*pos] = 0;
        }
    }
    return 1;
}

static int levi_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int levi_utf8_encode(uint32_t cp, char *dst, size_t cap, size_t *pos) {
    if (cp <= 0x7F) {
        if (*pos + 1 >= cap) return 0;
        dst[(*pos)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*pos + 2 >= cap) return 0;
        dst[(*pos)++] = (char)(0xC0 | (cp >> 6));
        dst[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*pos + 3 >= cap) return 0;
        dst[(*pos)++] = (char)(0xE0 | (cp >> 12));
        dst[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (*pos + 4 >= cap) return 0;
        dst[(*pos)++] = (char)(0xF0 | (cp >> 18));
        dst[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        return 0;
    }
    dst[*pos] = 0;
    return 1;
}

static const char *levi_json_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static const char *levi_json_read_string(const char *p, char *out, size_t cap) {
    p = levi_json_skip_ws(p);
    if (!p || *p != '"') return NULL;
    ++p;
    size_t pos = 0;
    if (cap == 0) return NULL;
    out[0] = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p) return NULL;
            char ch = 0;
            switch (*p) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                int h0 = levi_hex_digit(p[1]), h1 = levi_hex_digit(p[2]),
                    h2 = levi_hex_digit(p[3]), h3 = levi_hex_digit(p[4]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return NULL;
                uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int l0 = levi_hex_digit(p[2]), l1 = levi_hex_digit(p[3]),
                        l2 = levi_hex_digit(p[4]), l3 = levi_hex_digit(p[5]);
                    if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                        uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                            p += 6;
                        }
                    }
                }
                if (!levi_utf8_encode(cp, out, cap, &pos)) return NULL;
                continue;
            }
            default:
                return NULL;
            }
            if (pos + 1 >= cap) return NULL;
            out[pos++] = ch;
            out[pos] = 0;
            ++p;
        } else {
            if (pos + 1 >= cap) return NULL;
            out[pos++] = *p++;
            out[pos] = 0;
        }
    }
    if (*p != '"') return NULL;
    return p + 1;
}

static int levi_json_parse_dialog_response(const char *json, int *out_cancelled,
                                           char *path_buf, size_t buf_cap,
                                           size_t *offsets, int max_paths, int *out_count) {
    if (!json || !out_cancelled || !path_buf || !offsets || !out_count) return 0;
    *out_cancelled = 1;
    *out_count = 0;
    int saw_cancelled = 0;
    size_t buf_pos = 0;
    const char *p = levi_json_skip_ws(json);
    if (!p || *p != '{') return 0;
    ++p;
    for (;;) {
        p = levi_json_skip_ws(p);
        if (!p || !*p) return 0;
        if (*p == '}') return saw_cancelled;
        char key[64];
        p = levi_json_read_string(p, key, sizeof key);
        if (!p) return 0;
        p = levi_json_skip_ws(p);
        if (*p != ':') return 0;
        p = levi_json_skip_ws(p + 1);
        if (!strcmp(key, "cancelled")) {
            if (!strncmp(p, "true", 4)) {
                *out_cancelled = 1;
                p += 4;
            } else if (!strncmp(p, "false", 5)) {
                *out_cancelled = 0;
                p += 5;
            } else {
                return 0;
            }
            saw_cancelled = 1;
        } else if (!strcmp(key, "paths")) {
            if (*p == 'n' && !strncmp(p, "null", 4)) {
                p += 4;
            } else if (*p == '[') {
                ++p;
                for (;;) {
                    p = levi_json_skip_ws(p);
                    if (!p || !*p) return 0;
                    if (*p == ']') {
                        ++p;
                        break;
                    }
                    if (*out_count >= max_paths || buf_pos + 2 >= buf_cap) return 0;
                    char *dst = path_buf + buf_pos;
                    p = levi_json_read_string(p, dst, buf_cap - buf_pos);
                    if (!p) return 0;
                    offsets[*out_count] = buf_pos;
                    buf_pos += strlen(dst) + 1;
                    (*out_count)++;
                    p = levi_json_skip_ws(p);
                    if (*p == ',') {
                        ++p;
                        continue;
                    }
                    if (*p == ']') {
                        ++p;
                        break;
                    }
                    return 0;
                }
            } else {
                return 0;
            }
        } else {
            return 0;
        }
        p = levi_json_skip_ws(p);
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '}') return saw_cancelled;
        return 0;
    }
}

#ifndef LEVI_SHELL_FIX_PURE_TEST

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

typedef LPSTR (CDECL *WineGetUnixFileNameFn)(LPCWSTR);
typedef LPWSTR (CDECL *WineGetDosFileNameFn)(LPCSTR);
typedef BOOL (WINAPI *SHGetPathFromIDListWFn)(PCIDLIST_ABSOLUTE, LPWSTR);
typedef PIDLIST_ABSOLUTE (WINAPI *ILCombineFn)(PCIDLIST_ABSOLUTE, PCUIDLIST_RELATIVE);
typedef PIDLIST_ABSOLUTE (WINAPI *ILCreateFromPathWFn)(PCWSTR);
typedef void (WINAPI *ILFreeFn)(PIDLIST_RELATIVE);

typedef HINSTANCE (WINAPI *ShellExecuteWFn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
typedef HINSTANCE (WINAPI *ShellExecuteAFn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
typedef BOOL (WINAPI *ShellExecuteExWFn)(SHELLEXECUTEINFOW *);
typedef BOOL (WINAPI *ShellExecuteExAFn)(SHELLEXECUTEINFOA *);
typedef HRESULT (WINAPI *SHOpenFolderAndSelectItemsFn)(PCIDLIST_ABSOLUTE, UINT, PCUITEMID_CHILD_ARRAY, DWORD);
typedef BOOL (WINAPI *CreateProcessWFn)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *CreateProcessAFn)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef UINT (WINAPI *WinExecFn)(LPCSTR, UINT);
typedef BOOL (WINAPI *GetOpenFileNameWFn)(LPOPENFILENAMEW);
typedef BOOL (WINAPI *GetOpenFileNameAFn)(LPOPENFILENAMEA);
typedef BOOL (WINAPI *GetSaveFileNameWFn)(LPOPENFILENAMEW);
typedef BOOL (WINAPI *GetSaveFileNameAFn)(LPOPENFILENAMEA);
typedef PIDLIST_ABSOLUTE (WINAPI *SHBrowseForFolderWFn)(LPBROWSEINFOW);
typedef PIDLIST_ABSOLUTE (WINAPI *SHBrowseForFolderAFn)(LPBROWSEINFOA);

static HMODULE shell_self_module;
static unsigned short shell_bridge_port;
static char shell_bridge_base_path[160];
static char shell_bridge_token[96];
static int shell_bridge_configured;

static WineGetUnixFileNameFn wine_to_unix_fn;
static WineGetDosFileNameFn wine_to_dos_fn;

static ShellExecuteWFn real_shell_execute_w;
static ShellExecuteAFn real_shell_execute_a;
static ShellExecuteExWFn real_shell_execute_ex_w;
static ShellExecuteExAFn real_shell_execute_ex_a;
static SHOpenFolderAndSelectItemsFn real_sh_open_folder;
static CreateProcessWFn real_create_process_w;
static CreateProcessAFn real_create_process_a;
static WinExecFn real_win_exec;
static GetOpenFileNameWFn real_get_open_file_name_w;
static GetOpenFileNameAFn real_get_open_file_name_a;
static GetSaveFileNameWFn real_get_save_file_name_w;
static GetSaveFileNameAFn real_get_save_file_name_a;
static SHBrowseForFolderWFn real_sh_browse_for_folder_w;
static SHBrowseForFolderAFn real_sh_browse_for_folder_a;

static int shell_wide_to_utf8(const WCHAR *w, char *out, int out_cap) {
    if (!w || !out || out_cap <= 0) return 0;
    out[0] = 0;
    if (!w[0]) return 1;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_cap, NULL, NULL);
    return n > 0;
}

static int shell_utf8_to_wide(const char *s, WCHAR *out, int out_cap) {
    if (!s || !out || out_cap <= 0) return 0;
    out[0] = 0;
    if (!s[0]) return 1;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, out_cap);
    return n > 0;
}

static int shell_ansi_to_wide(const char *s, WCHAR *out, int out_cap) {
    if (!s || !out || out_cap <= 0) return 0;
    out[0] = 0;
    if (!s[0]) return 1;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, out, out_cap);
    return n > 0;
}

static int shell_resolve_full_path_w(const WCHAR *file, const WCHAR *dir, WCHAR *out, DWORD out_cap) {
    if (!file || !file[0] || !out || out_cap < 4) return 0;
    /* Ignore URLs and shell namespaces. */
    if (wcsstr(file, L"://") || levi_wstr_iprefix(file, L"mailto:") ||
        levi_wstr_iprefix(file, L"shell:") || levi_wstr_iprefix(file, L"::{")) {
        return 0;
    }
    WCHAR combined[4096];
    const WCHAR *input = file;
    int is_abs = (file[0] == L'\\' || file[0] == L'/' ||
                  (((file[0] >= L'A' && file[0] <= L'Z') || (file[0] >= L'a' && file[0] <= L'z')) && file[1] == L':'));
    if (!is_abs && dir && dir[0]) {
        size_t dlen = wcslen(dir);
        int has_sep = dlen > 0 && (dir[dlen - 1] == L'\\' || dir[dlen - 1] == L'/');
        int n = _snwprintf(combined, sizeof combined / sizeof combined[0],
                           has_sep ? L"%ls%ls" : L"%ls\\%ls", dir, file);
        if (n <= 0 || n >= (int)(sizeof combined / sizeof combined[0])) return 0;
        input = combined;
    }
    DWORD got = GetFullPathNameW(input, out_cap, out, NULL);
    return got > 0 && got < out_cap;
}

static int shell_dos_to_unix_utf8(const WCHAR *dos_path, char *out, size_t out_cap) {
    if (!dos_path || !dos_path[0] || !out || out_cap < 2) return 0;
    out[0] = 0;
    if (dos_path[0] == L'\\' && dos_path[1] == L'\\' && dos_path[2] == L'?' && dos_path[3] == L'\\') {
        dos_path += 4;
    }
    if (wine_to_unix_fn) {
        LPSTR unix_path = wine_to_unix_fn(dos_path);
        if (unix_path) {
            size_t len = strlen(unix_path);
            int ok = len > 0 && len < out_cap;
            if (ok) memcpy(out, unix_path, len + 1);
            HeapFree(GetProcessHeap(), 0, unix_path);
            if (ok) return 1;
        }
    }
    /* Fallback for Z:\... paths if wine_get_unix_file_name is unavailable. */
    if ((dos_path[0] == L'Z' || dos_path[0] == L'z') && dos_path[1] == L':') {
        char utf8[4096];
        if (!shell_wide_to_utf8(dos_path + 2, utf8, (int)sizeof utf8)) return 0;
        size_t len = strlen(utf8);
        if (len == 0 || len + 1 >= out_cap) return 0;
        for (size_t i = 0; i < len; ++i) {
            out[i] = (utf8[i] == '\\') ? '/' : utf8[i];
        }
        out[len] = 0;
        return out[0] == '/';
    }
    return 0;
}

static int shell_unix_to_dos_w(const char *unix_path, WCHAR *out, size_t out_cap) {
    if (!unix_path || !unix_path[0] || !out || out_cap < 4) return 0;
    out[0] = 0;
    if (wine_to_dos_fn) {
        LPWSTR dos = wine_to_dos_fn(unix_path);
        if (dos) {
            size_t len = wcslen(dos);
            int ok = len > 0 && len < out_cap;
            if (ok) memcpy(out, dos, (len + 1) * sizeof(WCHAR));
            HeapFree(GetProcessHeap(), 0, dos);
            if (ok) return 1;
        }
    }
    if (unix_path[0] == '/') {
        out[0] = L'Z';
        out[1] = L':';
        if (!shell_utf8_to_wide(unix_path, out + 2, (int)(out_cap - 2))) return 0;
        for (WCHAR *p = out + 2; *p; ++p) {
            if (*p == L'/') *p = L'\\';
        }
        return 1;
    }
    return 0;
}

static int shell_http_post(const char *action, const char *body,
                           char *resp_buf, size_t resp_cap, DWORD recv_timeout_ms) {
    if (!shell_bridge_configured || !action || !body) return 0;
    char full_path[200];
    int plen = snprintf(full_path, sizeof full_path, "%s/%s", shell_bridge_base_path, action);
    if (plen <= 0 || plen >= (int)sizeof full_path) return 0;

    WCHAR wpath[200], wtoken[96], headers[192];
    if (!shell_utf8_to_wide(full_path, wpath, (int)(sizeof wpath / sizeof wpath[0]))) return 0;
    if (!shell_utf8_to_wide(shell_bridge_token, wtoken, (int)(sizeof wtoken / sizeof wtoken[0]))) return 0;
    _snwprintf(headers, sizeof headers / sizeof headers[0],
               L"Authorization: Bearer %ls\r\nContent-Type: application/json\r\n", wtoken);
    headers[(sizeof headers / sizeof headers[0]) - 1] = 0;

    HINTERNET session = WinHttpOpen(L"LeviWineBridge/0.2", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 1500, 1500, 3000, (int)recv_timeout_ms);
    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)shell_bridge_port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", wpath, NULL, NULL, NULL, 0) : NULL;

    int ok = 0;
    DWORD body_len = (DWORD)strlen(body);
    if (req &&
        WinHttpSendRequest(req, headers, (DWORD)wcslen(headers), (LPVOID)body, body_len, body_len, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, sz = sizeof status;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL) &&
            status == 200) {
            ok = 1;
            if (resp_buf && resp_cap > 0) {
                size_t total = 0;
                resp_buf[0] = 0;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    if (total + avail >= resp_cap) {
                        ok = 0;
                        break;
                    }
                    DWORD got = 0;
                    if (!WinHttpReadData(req, resp_buf + total, avail, &got) || got == 0) break;
                    total += got;
                    resp_buf[total] = 0;
                }
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

static int shell_fallback_winebrowser(const WCHAR *dos_dir) {
    if (!dos_dir || !dos_dir[0] || !real_shell_execute_w) return 0;
    WCHAR quoted[4104];
    int n = _snwprintf(quoted, sizeof quoted / sizeof quoted[0], L"\"%ls\"", dos_dir);
    if (n <= 0 || n >= (int)(sizeof quoted / sizeof quoted[0])) return 0;
    HINSTANCE r = real_shell_execute_w(NULL, L"open", L"winebrowser.exe", quoted, NULL, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

static int shell_open_native(const WCHAR *dos_path, int reveal) {
    if (!dos_path || !dos_path[0]) return 0;
    DWORD attr = GetFileAttributesW(dos_path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (!reveal && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;

    if (shell_bridge_configured) {
        char unix_path[4096];
        char win_utf8[4096];
        int has_unix = shell_dos_to_unix_utf8(dos_path, unix_path, sizeof unix_path);
        int has_win = shell_wide_to_utf8(dos_path, win_utf8, (int)sizeof win_utf8);
        char body[8192];
        size_t pos = 0;
        int built = levi_json_append_raw(body, sizeof body, &pos, "{");
        if (built && has_unix) {
            built = levi_json_append_raw(body, sizeof body, &pos, "\"path\":\"") &&
                    levi_json_append_escaped(body, sizeof body, &pos, unix_path) &&
                    levi_json_append_raw(body, sizeof body, &pos, "\",");
        } else if (built && has_win) {
            built = levi_json_append_raw(body, sizeof body, &pos, "\"windowsPath\":\"") &&
                    levi_json_append_escaped(body, sizeof body, &pos, win_utf8) &&
                    levi_json_append_raw(body, sizeof body, &pos, "\",");
        } else {
            built = 0;
        }
        if (built) {
            built = levi_json_append_raw(body, sizeof body, &pos, reveal ? "\"reveal\":true}" : "\"reveal\":false}");
        }
        if (built && shell_http_post("open", body, NULL, 0, 2500)) {
            return 1;
        }
    }

    if (!reveal) {
        return shell_fallback_winebrowser(dos_path);
    }
    return 0;
}

static int shell_try_handle_shellexec_w(const WCHAR *verb, const WCHAR *file, const WCHAR *params, const WCHAR *dir) {
    if (!file || !file[0]) return 0;
    WCHAR target[4096];
    int reveal = 0;

    if (levi_shell_is_explorer_exe(file)) {
        if (!params || !levi_shell_parse_explorer_args(params, target, sizeof target / sizeof target[0], &reveal)) {
            return 0;
        }
        WCHAR full[4096];
        if (!shell_resolve_full_path_w(target, dir, full, (DWORD)(sizeof full / sizeof full[0]))) return 0;
        return shell_open_native(full, reveal);
    }

    if (!levi_shell_is_open_verb(verb)) return 0;
    if (params && params[0]) return 0;
    WCHAR full[4096];
    if (!shell_resolve_full_path_w(file, dir, full, (DWORD)(sizeof full / sizeof full[0]))) return 0;
    DWORD attr = GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    return shell_open_native(full, 0);
}

/* ---------------- Hooked folder-opening Win32 APIs ---------------- */

static HINSTANCE WINAPI bridge_shell_execute_w(HWND hwnd, LPCWSTR op, LPCWSTR file,
                                               LPCWSTR params, LPCWSTR dir, INT show_cmd) {
    if (shell_try_handle_shellexec_w(op, file, params, dir)) {
        return (HINSTANCE)(INT_PTR)42;
    }
    return real_shell_execute_w ? real_shell_execute_w(hwnd, op, file, params, dir, show_cmd)
                                : (HINSTANCE)(INT_PTR)SE_ERR_FNF;
}

static HINSTANCE WINAPI bridge_shell_execute_a(HWND hwnd, LPCSTR op, LPCSTR file,
                                               LPCSTR params, LPCSTR dir, INT show_cmd) {
    WCHAR wop[64], wfile[4096], wparams[4096], wdir[4096];
    const WCHAR *pop = (op && shell_ansi_to_wide(op, wop, 64)) ? wop : NULL;
    const WCHAR *pfile = (file && shell_ansi_to_wide(file, wfile, 4096)) ? wfile : NULL;
    const WCHAR *pparams = (params && shell_ansi_to_wide(params, wparams, 4096)) ? wparams : NULL;
    const WCHAR *pdir = (dir && shell_ansi_to_wide(dir, wdir, 4096)) ? wdir : NULL;
    if (pfile && shell_try_handle_shellexec_w(pop, pfile, pparams, pdir)) {
        return (HINSTANCE)(INT_PTR)42;
    }
    return real_shell_execute_a ? real_shell_execute_a(hwnd, op, file, params, dir, show_cmd)
                                : (HINSTANCE)(INT_PTR)SE_ERR_FNF;
}

static BOOL WINAPI bridge_shell_execute_ex_w(SHELLEXECUTEINFOW *info) {
    if (info && !(info->fMask & SEE_MASK_IDLIST) && info->lpFile) {
        if (shell_try_handle_shellexec_w(info->lpVerb, info->lpFile, info->lpParameters, info->lpDirectory)) {
            info->hInstApp = (HINSTANCE)(INT_PTR)42;
            if (info->fMask & SEE_MASK_NOCLOSEPROCESS) info->hProcess = NULL;
            return TRUE;
        }
    }
    return real_shell_execute_ex_w ? real_shell_execute_ex_w(info) : FALSE;
}

static BOOL WINAPI bridge_shell_execute_ex_a(SHELLEXECUTEINFOA *info) {
    if (info && !(info->fMask & SEE_MASK_IDLIST) && info->lpFile) {
        WCHAR wop[64], wfile[4096], wparams[4096], wdir[4096];
        const WCHAR *pop = (info->lpVerb && shell_ansi_to_wide(info->lpVerb, wop, 64)) ? wop : NULL;
        const WCHAR *pfile = shell_ansi_to_wide(info->lpFile, wfile, 4096) ? wfile : NULL;
        const WCHAR *pparams = (info->lpParameters && shell_ansi_to_wide(info->lpParameters, wparams, 4096)) ? wparams : NULL;
        const WCHAR *pdir = (info->lpDirectory && shell_ansi_to_wide(info->lpDirectory, wdir, 4096)) ? wdir : NULL;
        if (pfile && shell_try_handle_shellexec_w(pop, pfile, pparams, pdir)) {
            info->hInstApp = (HINSTANCE)(INT_PTR)42;
            if (info->fMask & SEE_MASK_NOCLOSEPROCESS) info->hProcess = NULL;
            return TRUE;
        }
    }
    return real_shell_execute_ex_a ? real_shell_execute_ex_a(info) : FALSE;
}

static HRESULT WINAPI bridge_sh_open_folder(PCIDLIST_ABSOLUTE pidl_folder, UINT cidl,
                                            PCUITEMID_CHILD_ARRAY apidl, DWORD flags) {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    SHGetPathFromIDListWFn get_path = shell32 ? (SHGetPathFromIDListWFn)(void *)GetProcAddress(shell32, "SHGetPathFromIDListW") : NULL;
    if (get_path && pidl_folder) {
        WCHAR target[4096];
        int handled = 0;
        if (cidl > 0 && apidl && apidl[0]) {
            ILCombineFn il_combine = (ILCombineFn)(void *)GetProcAddress(shell32, "ILCombine");
            ILFreeFn il_free = (ILFreeFn)(void *)GetProcAddress(shell32, "ILFree");
            if (il_combine) {
                PIDLIST_ABSOLUTE full_pidl = il_combine(pidl_folder, apidl[0]);
                if (full_pidl) {
                    if (get_path(full_pidl, target)) {
                        handled = shell_open_native(target, 1);
                    }
                    if (il_free) il_free(full_pidl);
                    else {
                        HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
                        typedef void (WINAPI *CoTaskMemFreeFn)(LPVOID);
                        CoTaskMemFreeFn free_fn = ole32 ? (CoTaskMemFreeFn)(void *)GetProcAddress(ole32, "CoTaskMemFree") : NULL;
                        if (free_fn) free_fn(full_pidl);
                    }
                }
            }
        }
        if (!handled && get_path(pidl_folder, target)) {
            handled = shell_open_native(target, 0);
        }
        if (handled) return S_OK;
    }
    return real_sh_open_folder ? real_sh_open_folder(pidl_folder, cidl, apidl, flags) : E_FAIL;
}

static int shell_try_handle_create_process_w(LPCWSTR app_name, LPCWSTR cmdline, LPCWSTR cwd) {
    WCHAR target[4096];
    int reveal = 0;
    int parsed = 0;
    if (app_name && levi_shell_is_explorer_exe(app_name)) {
        if (!cmdline) return 0;
        wchar_t first[512];
        const wchar_t *rest = levi_shell_next_token(cmdline, first, sizeof first / sizeof first[0]);
        if (rest && levi_shell_is_explorer_exe(first)) {
            parsed = levi_shell_parse_explorer_args(rest, target, sizeof target / sizeof target[0], &reveal);
        } else {
            parsed = levi_shell_parse_explorer_args(cmdline, target, sizeof target / sizeof target[0], &reveal);
        }
    } else if (!app_name && cmdline) {
        parsed = levi_shell_parse_cmdline_explorer(cmdline, target, sizeof target / sizeof target[0], &reveal);
    }
    if (!parsed) return 0;
    WCHAR full[4096];
    if (!shell_resolve_full_path_w(target, cwd, full, (DWORD)(sizeof full / sizeof full[0]))) return 0;
    return shell_open_native(full, reveal);
}

static void shell_fill_fake_proc_info(LPPROCESS_INFORMATION pi) {
    if (!pi) return;
    memset(pi, 0, sizeof *pi);
    pi->hProcess = CreateEventW(NULL, TRUE, TRUE, NULL);
    pi->hThread = CreateEventW(NULL, TRUE, TRUE, NULL);
    pi->dwProcessId = GetCurrentProcessId();
    pi->dwThreadId = GetCurrentThreadId();
}

static BOOL WINAPI bridge_create_process_w(LPCWSTR app, LPWSTR cmdline,
                                           LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                           BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
                                           LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    if (shell_try_handle_create_process_w(app, cmdline, cwd)) {
        shell_fill_fake_proc_info(pi);
        return TRUE;
    }
    return real_create_process_w ? real_create_process_w(app, cmdline, pa, ta, inherit, flags, env, cwd, si, pi) : FALSE;
}

static BOOL WINAPI bridge_create_process_a(LPCSTR app, LPSTR cmdline,
                                           LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                           BOOL inherit, DWORD flags, LPVOID env, LPCSTR cwd,
                                           LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    WCHAR wapp[1024], wcmd[4096], wcwd[4096];
    const WCHAR *papp = (app && shell_ansi_to_wide(app, wapp, 1024)) ? wapp : NULL;
    const WCHAR *pcmd = (cmdline && shell_ansi_to_wide(cmdline, wcmd, 4096)) ? wcmd : NULL;
    const WCHAR *pcwd = (cwd && shell_ansi_to_wide(cwd, wcwd, 4096)) ? wcwd : NULL;
    if (shell_try_handle_create_process_w(papp, pcmd, pcwd)) {
        shell_fill_fake_proc_info(pi);
        return TRUE;
    }
    return real_create_process_a ? real_create_process_a(app, cmdline, pa, ta, inherit, flags, env, cwd, si, pi) : FALSE;
}

static UINT WINAPI bridge_win_exec(LPCSTR cmdline, UINT show_cmd) {
    WCHAR wcmd[4096];
    if (cmdline && shell_ansi_to_wide(cmdline, wcmd, 4096) &&
        shell_try_handle_create_process_w(NULL, wcmd, NULL)) {
        return 33;
    }
    return real_win_exec ? real_win_exec(cmdline, show_cmd) : 0;
}

/* ---------------- Hooked file / folder picker Win32 APIs ---------------- */

static int shell_append_filters_json_w(char *body, size_t cap, size_t *pos, const WCHAR *filter) {
    if (!filter || !filter[0]) return 1;
    if (!levi_json_append_raw(body, cap, pos, ",\"filters\":[")) return 0;
    const WCHAR *p = filter;
    int first = 1;
    int count = 0;
    while (*p && count < 32) {
        const WCHAR *name = p;
        p += wcslen(p) + 1;
        if (!*p) break;
        const WCHAR *pattern = p;
        p += wcslen(p) + 1;

        char uname[256], upat[512];
        if (!shell_wide_to_utf8(name, uname, (int)sizeof uname) ||
            !shell_wide_to_utf8(pattern, upat, (int)sizeof upat)) {
            continue;
        }
        if (!first && !levi_json_append_raw(body, cap, pos, ",")) return 0;
        first = 0;
        if (!levi_json_append_raw(body, cap, pos, "{\"name\":\"") ||
            !levi_json_append_escaped(body, cap, pos, uname) ||
            !levi_json_append_raw(body, cap, pos, "\",\"patterns\":[\"") ||
            !levi_json_append_escaped(body, cap, pos, upat) ||
            !levi_json_append_raw(body, cap, pos, "\"]}")) {
            return 0;
        }
        ++count;
    }
    return levi_json_append_raw(body, cap, pos, "]");
}

/* Returns: 1 = user selected, 0 = user cancelled, -1 = bridge unavailable (fallback to Win32). */
static int shell_request_dialog_w(const char *kind, const WCHAR *title, const WCHAR *initial_dir,
                                  const WCHAR *default_name, const WCHAR *default_ext,
                                  const WCHAR *filter, int multi,
                                  char *path_buf, size_t path_buf_cap,
                                  size_t *offsets, int max_paths, int *out_count) {
    if (!shell_bridge_configured) return -1;

    char body[16384];
    size_t pos = 0;
    if (!levi_json_append_raw(body, sizeof body, &pos, "{\"kind\":\"") ||
        !levi_json_append_escaped(body, sizeof body, &pos, kind) ||
        !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
        return -1;
    }
    if (title && title[0]) {
        char utitle[512];
        if (shell_wide_to_utf8(title, utitle, (int)sizeof utitle)) {
            if (!levi_json_append_raw(body, sizeof body, &pos, ",\"title\":\"") ||
                !levi_json_append_escaped(body, sizeof body, &pos, utitle) ||
                !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
                return -1;
            }
        }
    }
    if (initial_dir && initial_dir[0]) {
        WCHAR full_dir[4096];
        const WCHAR *dir_src = shell_resolve_full_path_w(initial_dir, NULL, full_dir, 4096) ? full_dir : initial_dir;
        char udir[4096];
        if (shell_dos_to_unix_utf8(dir_src, udir, sizeof udir)) {
            if (!levi_json_append_raw(body, sizeof body, &pos, ",\"initialDir\":\"") ||
                !levi_json_append_escaped(body, sizeof body, &pos, udir) ||
                !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
                return -1;
            }
        } else if (shell_wide_to_utf8(dir_src, udir, (int)sizeof udir)) {
            if (!levi_json_append_raw(body, sizeof body, &pos, ",\"windowsInitialDir\":\"") ||
                !levi_json_append_escaped(body, sizeof body, &pos, udir) ||
                !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
                return -1;
            }
        }
    }
    if (default_name && default_name[0]) {
        char uname[512];
        if (shell_wide_to_utf8(default_name, uname, (int)sizeof uname)) {
            if (!levi_json_append_raw(body, sizeof body, &pos, ",\"defaultName\":\"") ||
                !levi_json_append_escaped(body, sizeof body, &pos, uname) ||
                !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
                return -1;
            }
        }
    }
    if (default_ext && default_ext[0]) {
        char uext[64];
        if (shell_wide_to_utf8(default_ext, uext, (int)sizeof uext)) {
            if (!levi_json_append_raw(body, sizeof body, &pos, ",\"defaultExt\":\"") ||
                !levi_json_append_escaped(body, sizeof body, &pos, uext) ||
                !levi_json_append_raw(body, sizeof body, &pos, "\"")) {
                return -1;
            }
        }
    }
    if (multi) {
        if (!levi_json_append_raw(body, sizeof body, &pos, ",\"multi\":true")) return -1;
    }
    if (filter && !shell_append_filters_json_w(body, sizeof body, &pos, filter)) return -1;
    if (!levi_json_append_raw(body, sizeof body, &pos, "}")) return -1;

    char *resp = (char *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!resp) return -1;
    int http_ok = shell_http_post("dialog", body, resp, 65536, 0);
    if (!http_ok) {
        HeapFree(GetProcessHeap(), 0, resp);
        return -1;
    }
    int cancelled = 1;
    int parsed = levi_json_parse_dialog_response(resp, &cancelled, path_buf, path_buf_cap,
                                                 offsets, max_paths, out_count);
    HeapFree(GetProcessHeap(), 0, resp);
    if (!parsed) return -1;
    if (cancelled || *out_count <= 0) return 0;
    return 1;
}

static void shell_compute_ofn_offsets_w(const WCHAR *path, WORD *file_off, WORD *ext_off) {
    size_t len = wcslen(path);
    size_t slash = 0;
    for (size_t i = 0; i < len; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') slash = i + 1;
    }
    *file_off = (WORD)slash;
    *ext_off = 0;
    for (size_t i = len; i > slash; --i) {
        if (path[i - 1] == L'.') {
            *ext_off = (WORD)i;
            break;
        }
    }
}

static int shell_handle_ofn_w(LPOPENFILENAMEW ofn, int is_save) {
    if (!shell_bridge_configured || !ofn || !ofn->lpstrFile || ofn->nMaxFile < 4) return -1;
    if (ofn->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) return -1;

    WCHAR init_dir_buf[4096];
    init_dir_buf[0] = 0;
    const WCHAR *init_dir = ofn->lpstrInitialDir;
    const WCHAR *def_name = NULL;
    if (ofn->lpstrFile[0]) {
        const WCHAR *last_sep = NULL;
        for (const WCHAR *p = ofn->lpstrFile; *p; ++p) {
            if (*p == L'\\' || *p == L'/') last_sep = p;
        }
        if (last_sep) {
            if (!init_dir || !init_dir[0]) {
                size_t dlen = (size_t)(last_sep - ofn->lpstrFile);
                if (dlen > 0 && dlen < 4096) {
                    memcpy(init_dir_buf, ofn->lpstrFile, dlen * sizeof(WCHAR));
                    init_dir_buf[dlen] = 0;
                    init_dir = init_dir_buf;
                }
            }
            def_name = last_sep + 1;
        } else {
            def_name = ofn->lpstrFile;
        }
    }

    int multi = (!is_save && (ofn->Flags & OFN_ALLOWMULTISELECT)) ? 1 : 0;
    char *path_buf = (char *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!path_buf) return -1;
    size_t offsets[128];
    int count = 0;
    int rc = shell_request_dialog_w(is_save ? "save" : "open",
                                    ofn->lpstrTitle, init_dir, def_name, ofn->lpstrDefExt,
                                    ofn->lpstrFilter, multi,
                                    path_buf, 65536, offsets, 128, &count);
    if (rc <= 0) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return rc;
    }

    if (count == 1 || !multi) {
        WCHAR dos[4096];
        if (!shell_unix_to_dos_w(path_buf + offsets[0], dos, 4096)) {
            HeapFree(GetProcessHeap(), 0, path_buf);
            return -1;
        }
        size_t dlen = wcslen(dos);
        if (dlen + 1 > ofn->nMaxFile) {
            HeapFree(GetProcessHeap(), 0, path_buf);
            return 0;
        }
        memcpy(ofn->lpstrFile, dos, (dlen + 1) * sizeof(WCHAR));
        shell_compute_ofn_offsets_w(ofn->lpstrFile, &ofn->nFileOffset, &ofn->nFileExtension);
        HeapFree(GetProcessHeap(), 0, path_buf);
        return 1;
    }

    /* Multi-select: format as <dir>\0<name1>\0<name2>\0\0 (OFN_EXPLORER style). */
    WCHAR first_dos[4096];
    if (!shell_unix_to_dos_w(path_buf + offsets[0], first_dos, 4096)) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return -1;
    }
    WCHAR *last_sep = wcsrchr(first_dos, L'\\');
    if (!last_sep) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return -1;
    }
    size_t dir_len = (size_t)(last_sep - first_dos);
    if (dir_len == 2 && first_dos[1] == L':') dir_len = 3; /* Keep C:\ */
    size_t out_pos = 0;
    if (dir_len + 2 >= ofn->nMaxFile) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return 0;
    }
    memcpy(ofn->lpstrFile, first_dos, dir_len * sizeof(WCHAR));
    out_pos = dir_len;
    ofn->lpstrFile[out_pos++] = 0;
    ofn->nFileOffset = (WORD)out_pos;
    ofn->nFileExtension = 0;

    for (int i = 0; i < count; ++i) {
        WCHAR item_dos[4096];
        if (!shell_unix_to_dos_w(path_buf + offsets[i], item_dos, 4096)) continue;
        const WCHAR *base = wcsrchr(item_dos, L'\\');
        base = base ? (base + 1) : item_dos;
        size_t blen = wcslen(base);
        if (out_pos + blen + 2 > ofn->nMaxFile) break;
        memcpy(ofn->lpstrFile + out_pos, base, (blen + 1) * sizeof(WCHAR));
        out_pos += blen + 1;
    }
    ofn->lpstrFile[out_pos] = 0;
    HeapFree(GetProcessHeap(), 0, path_buf);
    return 1;
}

static int shell_handle_ofn_a(LPOPENFILENAMEA ofna, int is_save) {
    if (!shell_bridge_configured || !ofna || !ofna->lpstrFile || ofna->nMaxFile < 4) return -1;
    if (ofna->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) return -1;

    WCHAR wtitle[512], wdir[4096], wext[64];
    WCHAR *wfile = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32768 * sizeof(WCHAR));
    WCHAR *wfilter = NULL;
    if (!wfile) return -1;

    if (ofna->lpstrFile[0]) {
        shell_ansi_to_wide(ofna->lpstrFile, wfile, 4096);
    }
    if (ofna->lpstrFilter && ofna->lpstrFilter[0]) {
        wfilter = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 8192 * sizeof(WCHAR));
        if (wfilter) {
            const char *p = ofna->lpstrFilter;
            size_t wp = 0;
            while (*p && wp + 4 < 8192) {
                int n = MultiByteToWideChar(CP_ACP, 0, p, -1, wfilter + wp, (int)(8191 - wp));
                if (n <= 0) break;
                wp += (size_t)n;
                p += strlen(p) + 1;
            }
            wfilter[wp] = 0;
        }
    }

    OPENFILENAMEW ofnw;
    memset(&ofnw, 0, sizeof ofnw);
    ofnw.lStructSize = sizeof ofnw;
    ofnw.hwndOwner = ofna->hwndOwner;
    ofnw.lpstrFilter = wfilter;
    ofnw.lpstrFile = wfile;
    ofnw.nMaxFile = 32768;
    ofnw.lpstrInitialDir = (ofna->lpstrInitialDir && shell_ansi_to_wide(ofna->lpstrInitialDir, wdir, 4096)) ? wdir : NULL;
    ofnw.lpstrTitle = (ofna->lpstrTitle && shell_ansi_to_wide(ofna->lpstrTitle, wtitle, 512)) ? wtitle : NULL;
    ofnw.lpstrDefExt = (ofna->lpstrDefExt && shell_ansi_to_wide(ofna->lpstrDefExt, wext, 64)) ? wext : NULL;
    ofnw.Flags = ofna->Flags;

    int rc = shell_handle_ofn_w(&ofnw, is_save);
    if (rc == 1) {
        if (ofnw.nFileOffset > 0 && wfile[ofnw.nFileOffset - 1] == 0 && (ofna->Flags & OFN_ALLOWMULTISELECT)) {
            /* Double-NUL multi-select copy to ANSI. */
            size_t src = 0, dst = 0;
            int first = 1;
            while (wfile[src]) {
                int n = WideCharToMultiByte(CP_ACP, 0, wfile + src, -1,
                                            ofna->lpstrFile + dst, (int)(ofna->nMaxFile - dst - 1), NULL, NULL);
                if (n <= 0) break;
                if (first) {
                    ofna->nFileOffset = (WORD)n;
                    ofna->nFileExtension = 0;
                    first = 0;
                }
                dst += (size_t)n;
                src += wcslen(wfile + src) + 1;
            }
            ofna->lpstrFile[dst] = 0;
        } else {
            int n = WideCharToMultiByte(CP_ACP, 0, wfile, -1, ofna->lpstrFile, (int)ofna->nMaxFile, NULL, NULL);
            if (n <= 0) {
                rc = 0;
            } else {
                const char *s = ofna->lpstrFile;
                size_t len = strlen(s), slash = 0;
                for (size_t i = 0; i < len; ++i) {
                    if (s[i] == '\\' || s[i] == '/') slash = i + 1;
                }
                ofna->nFileOffset = (WORD)slash;
                ofna->nFileExtension = 0;
                for (size_t i = len; i > slash; --i) {
                    if (s[i - 1] == '.') {
                        ofna->nFileExtension = (WORD)i;
                        break;
                    }
                }
            }
        }
    }

    if (wfilter) HeapFree(GetProcessHeap(), 0, wfilter);
    HeapFree(GetProcessHeap(), 0, wfile);
    return rc;
}

static BOOL WINAPI bridge_get_open_file_name_w(LPOPENFILENAMEW ofn) {
    int rc = shell_handle_ofn_w(ofn, 0);
    if (rc >= 0) return rc ? TRUE : FALSE;
    return real_get_open_file_name_w ? real_get_open_file_name_w(ofn) : FALSE;
}

static BOOL WINAPI bridge_get_save_file_name_w(LPOPENFILENAMEW ofn) {
    int rc = shell_handle_ofn_w(ofn, 1);
    if (rc >= 0) return rc ? TRUE : FALSE;
    return real_get_save_file_name_w ? real_get_save_file_name_w(ofn) : FALSE;
}

static BOOL WINAPI bridge_get_open_file_name_a(LPOPENFILENAMEA ofn) {
    int rc = shell_handle_ofn_a(ofn, 0);
    if (rc >= 0) return rc ? TRUE : FALSE;
    return real_get_open_file_name_a ? real_get_open_file_name_a(ofn) : FALSE;
}

static BOOL WINAPI bridge_get_save_file_name_a(LPOPENFILENAMEA ofn) {
    int rc = shell_handle_ofn_a(ofn, 1);
    if (rc >= 0) return rc ? TRUE : FALSE;
    return real_get_save_file_name_a ? real_get_save_file_name_a(ofn) : FALSE;
}

static PIDLIST_ABSOLUTE shell_browse_folder_common(const WCHAR *title, PCIDLIST_ABSOLUTE pidl_root,
                                                   WCHAR *out_dos, size_t out_dos_cap, int *out_handled) {
    *out_handled = 0;
    if (!shell_bridge_configured) return NULL;
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    ILCreateFromPathWFn create_pidl = shell32 ? (ILCreateFromPathWFn)(void *)GetProcAddress(shell32, "ILCreateFromPathW") : NULL;
    if (!create_pidl) return NULL;

    WCHAR root_path[4096];
    root_path[0] = 0;
    if (pidl_root) {
        SHGetPathFromIDListWFn get_path = (SHGetPathFromIDListWFn)(void *)GetProcAddress(shell32, "SHGetPathFromIDListW");
        if (get_path) get_path(pidl_root, root_path);
    }

    char *path_buf = (char *)HeapAlloc(GetProcessHeap(), 0, 8192);
    if (!path_buf) return NULL;
    size_t offsets[4];
    int count = 0;
    int rc = shell_request_dialog_w("folder", title, root_path[0] ? root_path : NULL,
                                    NULL, NULL, NULL, 0,
                                    path_buf, 8192, offsets, 4, &count);
    if (rc < 0) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return NULL;
    }
    *out_handled = 1;
    if (rc == 0 || count <= 0) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        return NULL;
    }
    if (!shell_unix_to_dos_w(path_buf + offsets[0], out_dos, out_dos_cap)) {
        HeapFree(GetProcessHeap(), 0, path_buf);
        *out_handled = 0;
        return NULL;
    }
    HeapFree(GetProcessHeap(), 0, path_buf);
    return create_pidl(out_dos);
}

static PIDLIST_ABSOLUTE WINAPI bridge_sh_browse_for_folder_w(LPBROWSEINFOW bi) {
    if (bi) {
        WCHAR dos[4096];
        int handled = 0;
        PIDLIST_ABSOLUTE pidl = shell_browse_folder_common(bi->lpszTitle, bi->pidlRoot, dos, 4096, &handled);
        if (handled) {
            if (pidl && bi->pszDisplayName) {
                const WCHAR *base = wcsrchr(dos, L'\\');
                base = base ? (base + 1) : dos;
                wcsncpy(bi->pszDisplayName, base, MAX_PATH - 1);
                bi->pszDisplayName[MAX_PATH - 1] = 0;
            }
            return pidl;
        }
    }
    return real_sh_browse_for_folder_w ? real_sh_browse_for_folder_w(bi) : NULL;
}

static PIDLIST_ABSOLUTE WINAPI bridge_sh_browse_for_folder_a(LPBROWSEINFOA bi) {
    if (bi) {
        WCHAR wtitle[512], dos[4096];
        const WCHAR *ptitle = (bi->lpszTitle && shell_ansi_to_wide(bi->lpszTitle, wtitle, 512)) ? wtitle : NULL;
        int handled = 0;
        PIDLIST_ABSOLUTE pidl = shell_browse_folder_common(ptitle, bi->pidlRoot, dos, 4096, &handled);
        if (handled) {
            if (pidl && bi->pszDisplayName) {
                const WCHAR *base = wcsrchr(dos, L'\\');
                base = base ? (base + 1) : dos;
                WideCharToMultiByte(CP_ACP, 0, base, -1, bi->pszDisplayName, MAX_PATH, NULL, NULL);
            }
            return pidl;
        }
    }
    return real_sh_browse_for_folder_a ? real_sh_browse_for_folder_a(bi) : NULL;
}

/* ---------------- IAT patching & module watcher ---------------- */

static int shell_skip_module(HMODULE mod, const WCHAR *name) {
    if (!mod || mod == shell_self_module || !name) return 1;
    static const WCHAR *const skipped[] = {
        L"ntdll.dll", L"kernel32.dll", L"kernelbase.dll",
        L"shell32.dll", L"comdlg32.dll", L"shlwapi.dll", L"comctl32.dll",
        L"user32.dll", L"gdi32.dll", L"advapi32.dll", L"rpcrt4.dll",
        L"sechost.dll", L"ole32.dll", L"oleaut32.dll", L"combase.dll",
        L"ws2_32.dll", L"winhttp.dll", L"ucrtbase.dll", L"msvcrt.dll"
    };
    for (size_t i = 0; i < sizeof skipped / sizeof skipped[0]; ++i) {
        if (!_wcsicmp(name, skipped[i])) return 1;
    }
    return 0;
}

static void shell_patch_module_iat(HMODULE mod) {
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
            PVOID volatile *real_slot = NULL;

            if (!strcmp(sym, "ShellExecuteW")) { replacement = (PVOID)&bridge_shell_execute_w; real_slot = (PVOID volatile *)&real_shell_execute_w; }
            else if (!strcmp(sym, "ShellExecuteA")) { replacement = (PVOID)&bridge_shell_execute_a; real_slot = (PVOID volatile *)&real_shell_execute_a; }
            else if (!strcmp(sym, "ShellExecuteExW")) { replacement = (PVOID)&bridge_shell_execute_ex_w; real_slot = (PVOID volatile *)&real_shell_execute_ex_w; }
            else if (!strcmp(sym, "ShellExecuteExA")) { replacement = (PVOID)&bridge_shell_execute_ex_a; real_slot = (PVOID volatile *)&real_shell_execute_ex_a; }
            else if (!strcmp(sym, "SHOpenFolderAndSelectItems")) { replacement = (PVOID)&bridge_sh_open_folder; real_slot = (PVOID volatile *)&real_sh_open_folder; }
            else if (!strcmp(sym, "CreateProcessW")) { replacement = (PVOID)&bridge_create_process_w; real_slot = (PVOID volatile *)&real_create_process_w; }
            else if (!strcmp(sym, "CreateProcessA")) { replacement = (PVOID)&bridge_create_process_a; real_slot = (PVOID volatile *)&real_create_process_a; }
            else if (!strcmp(sym, "WinExec")) { replacement = (PVOID)&bridge_win_exec; real_slot = (PVOID volatile *)&real_win_exec; }
            else if (shell_bridge_configured && !strcmp(sym, "GetOpenFileNameW")) { replacement = (PVOID)&bridge_get_open_file_name_w; real_slot = (PVOID volatile *)&real_get_open_file_name_w; }
            else if (shell_bridge_configured && !strcmp(sym, "GetOpenFileNameA")) { replacement = (PVOID)&bridge_get_open_file_name_a; real_slot = (PVOID volatile *)&real_get_open_file_name_a; }
            else if (shell_bridge_configured && !strcmp(sym, "GetSaveFileNameW")) { replacement = (PVOID)&bridge_get_save_file_name_w; real_slot = (PVOID volatile *)&real_get_save_file_name_w; }
            else if (shell_bridge_configured && !strcmp(sym, "GetSaveFileNameA")) { replacement = (PVOID)&bridge_get_save_file_name_a; real_slot = (PVOID volatile *)&real_get_save_file_name_a; }
            else if (shell_bridge_configured && !strcmp(sym, "SHBrowseForFolderW")) { replacement = (PVOID)&bridge_sh_browse_for_folder_w; real_slot = (PVOID volatile *)&real_sh_browse_for_folder_w; }
            else if (shell_bridge_configured && !strcmp(sym, "SHBrowseForFolderA")) { replacement = (PVOID)&bridge_sh_browse_for_folder_a; real_slot = (PVOID volatile *)&real_sh_browse_for_folder_a; }

            if (!replacement) continue;
            PVOID cur = (PVOID)(ULONG_PTR)iat->u1.Function;
            if (cur == replacement) continue;
            if (real_slot) {
                InterlockedCompareExchangePointer(real_slot, cur, NULL);
            }
            DWORD protection;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protection)) continue;
            InterlockedExchangePointer((PVOID volatile *)&iat->u1.Function, replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void *), protection, &protection);
        }
    }
}

static void shell_scan_modules(void) {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (shell32 && !real_shell_execute_w) {
        PVOID fn = (PVOID)(void *)GetProcAddress(shell32, "ShellExecuteW");
        if (fn) InterlockedCompareExchangePointer((PVOID volatile *)&real_shell_execute_w, fn, NULL);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof entry;
    if (Module32FirstW(snap, &entry)) {
        do {
            if (!shell_skip_module(entry.hModule, entry.szModule)) {
                shell_patch_module_iat(entry.hModule);
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

static DWORD WINAPI shell_watch_thread(void *unused) {
    (void)unused;
    HMODULE pinned = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)&shell_watch_thread, &pinned);
    shell_scan_modules();
    OutputDebugStringA("LeviWineBridge: Wine shell/dialog bridge ready\n");
    for (int i = 0; i < 150; ++i) {
        Sleep(200);
        shell_scan_modules();
    }
    /* Keep catching lazily loaded mod DLLs at low frequency. */
    for (;;) {
        Sleep(2000);
        shell_scan_modules();
    }
    return 0;
}

static int start_shell_fix(HMODULE module) {
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char flag[16];
    DWORD n = GetEnvironmentVariableA("LEVI_WINE_SHELL_FIX", flag, sizeof flag);
    if (n > 0 && n < sizeof flag && !strcmp(flag, "0")) return 0;

    shell_self_module = module;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        wine_to_unix_fn = (WineGetUnixFileNameFn)(void *)GetProcAddress(k32, "wine_get_unix_file_name");
        wine_to_dos_fn = (WineGetDosFileNameFn)(void *)GetProcAddress(k32, "wine_get_dos_file_name");
        real_create_process_w = (CreateProcessWFn)(void *)GetProcAddress(k32, "CreateProcessW");
        real_create_process_a = (CreateProcessAFn)(void *)GetProcAddress(k32, "CreateProcessA");
        real_win_exec = (WinExecFn)(void *)GetProcAddress(k32, "WinExec");
    }

    char url[256];
    DWORD a = GetEnvironmentVariableA("LEVI_SHELL_BRIDGE_URL", url, sizeof url);
    DWORD b = GetEnvironmentVariableA("LEVI_SHELL_BRIDGE_TOKEN", shell_bridge_token, sizeof shell_bridge_token);
    if (a > 0 && a < sizeof url && b > 0 && b < sizeof shell_bridge_token &&
        levi_shell_parse_endpoint(url, &shell_bridge_port, shell_bridge_base_path, sizeof shell_bridge_base_path)) {
        shell_bridge_configured = 1;
    }

    HANDLE thread = CreateThread(NULL, 0, shell_watch_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

#endif /* LEVI_SHELL_FIX_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_SHELL_FIX_H */
