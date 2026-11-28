/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Wine SuspendThread heap & loader lock guard for PreLoader / LeviLamina / mods.
 *
 * Under Wine, hook installers (PreLoader's Detours, LeviLamina's ThreadSuspender,
 * LeviOptimize, arke, etc.) enumerate and call SuspendThread() on every other
 * thread in the process, then allocate memory (e.g. std::vector::push_back in
 * LeviLamina::ThreadSuspender) or query module/memory state while those threads
 * remain suspended. When RTX / D3DMetal / MetalFX / ResourceLoadManager are
 * concurrently active during 97% startup, suspending a thread that holds a Win32
 * heap lock or ntdll's loader lock causes an immediate permanent deadlock.
 *
 * Strictly Wine-only (completely inert on native Windows):
 *   1. Patches the SuspendThread IAT in PreLoader.dll, LeviLamina.dll, and every
 *      mod DLL under the game directory — both already loaded during DllMain and
 *      loaded later via LdrRegisterDllNotification.
 *   2. Before calling the real SuspendThread(), acquires ntdll's loader lock
 *      (try-lock with brief yields to avoid ABBA inversion against already-
 *      suspended threads) and locks all process heaps (GetProcessHeaps +
 *      GetProcessHeap via RtlLockHeap), then releases them immediately after
 *      SuspendThread() returns.
 */
#ifndef LEVI_CLIENT_HEAP_GUARD_H
#define LEVI_CLIENT_HEAP_GUARD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

static int levi_heap_guard_wcs_ieq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    size_t i = 0;
    while (a[i] && b[i]) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb + (L'a' - L'A'));
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == L'\0' && b[i] == L'\0';
}

static int levi_heap_guard_wcs_iprefix(const wchar_t *str, const wchar_t *prefix, size_t prefix_len) {
    if (!str || !prefix || prefix_len == 0) return 0;
    for (size_t i = 0; i < prefix_len; ++i) {
        if (!str[i]) return 0;
        wchar_t ca = str[i];
        wchar_t cb = prefix[i];
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb + (L'a' - L'A'));
        if (ca == L'/') ca = L'\\';
        if (cb == L'/') cb = L'\\';
        if (ca != cb) return 0;
    }
    return 1;
}

static int levi_heap_guard_is_target_module(const wchar_t *full_path,
                                            const wchar_t *base_name,
                                            const wchar_t *game_dir,
                                            size_t game_dir_len) {
    const wchar_t *name = base_name;
    if ((!name || !name[0]) && full_path && full_path[0]) {
        name = full_path;
        for (const wchar_t *p = full_path; *p; ++p) {
            if (*p == L'\\' || *p == L'/') name = p + 1;
        }
    }
    if (name && (levi_heap_guard_wcs_ieq(name, L"PreLoader.dll") ||
                 levi_heap_guard_wcs_ieq(name, L"LeviLamina.dll"))) {
        return 1;
    }
    if (full_path && game_dir && game_dir_len > 0 &&
        levi_heap_guard_wcs_iprefix(full_path, game_dir, game_dir_len)) {
        /* Never patch our own DLL or the main Minecraft executable. */
        if (name && (levi_heap_guard_wcs_ieq(name, L"LeviWineBridge.dll") ||
                     levi_heap_guard_wcs_ieq(name, L"Minecraft.Windows.exe"))) {
            return 0;
        }
        return 1;
    }
    return 0;
}

#ifndef LEVI_HEAP_GUARD_PURE_TEST

#include <stdio.h>
#include <tlhelp32.h>

typedef DWORD (WINAPI *LeviSuspendThreadFn)(HANDLE);
typedef BOOLEAN (WINAPI *LeviLockHeapFn)(PVOID);
typedef LONG (NTAPI *LeviLdrLockLoaderLockFn)(ULONG flags, ULONG *disposition, ULONG_PTR *cookie);
typedef LONG (NTAPI *LeviLdrUnlockLoaderLockFn)(ULONG flags, ULONG_PTR cookie);

#define LEVI_LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY 0x00000002u
#define LEVI_LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCKED 1u
#define LEVI_HEAP_GUARD_MAX_HEAPS 64u

typedef struct LeviHgUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} LeviHgUnicodeString;

typedef struct LeviHgLdrDllLoadedData {
    ULONG Flags;
    const LeviHgUnicodeString *FullDllName;
    const LeviHgUnicodeString *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LeviHgLdrDllLoadedData;

typedef union LeviHgLdrDllNotificationData {
    LeviHgLdrDllLoadedData Loaded;
    LeviHgLdrDllLoadedData Unloaded;
} LeviHgLdrDllNotificationData;

typedef VOID (CALLBACK *LeviHgLdrDllNotificationFn)(
    ULONG reason,
    const LeviHgLdrDllNotificationData *data,
    PVOID context);

typedef LONG (NTAPI *LeviHgLdrRegisterDllNotificationFn)(
    ULONG flags,
    LeviHgLdrDllNotificationFn fn,
    PVOID context,
    PVOID *cookie);

static LeviSuspendThreadFn volatile g_hg_orig_suspend = NULL;
static LeviLockHeapFn g_hg_lock_heap = NULL;
static LeviLockHeapFn g_hg_unlock_heap = NULL;
static LeviLdrLockLoaderLockFn g_hg_ldr_lock = NULL;
static LeviLdrUnlockLoaderLockFn g_hg_ldr_unlock = NULL;
static HMODULE g_hg_self_module = NULL;
static WCHAR g_hg_game_dir[MAX_PATH];
static size_t g_hg_game_dir_len = 0;
static volatile LONG g_hg_patched_count = 0;
static volatile LONG g_hg_started = 0;
static PVOID g_hg_ldr_cookie = NULL;

/* A frozen Wine thread must not own any process heap lock or ntdll's loader
 * lock needed by PreLoader / LeviLamina / mod hook installers.
 * Lock ordering follows Windows/Wine's global hierarchy:
 *   LdrLockLoaderLock (try-lock with brief yields) -> RtlLockHeap (all heaps) -> SuspendThread. */
static DWORD WINAPI suspend_without_heap_owner(HANDLE thread) {
    LeviSuspendThreadFn orig =
        (LeviSuspendThreadFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_hg_orig_suspend, NULL, NULL);
    if (!orig) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        orig = k32 ? (LeviSuspendThreadFn)(void *)GetProcAddress(k32, "SuspendThread") : NULL;
        if (!orig) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return (DWORD)-1;
        }
    }

    int ldr_locked = 0;
    ULONG_PTR ldr_cookie = 0;
    if (g_hg_ldr_lock && g_hg_ldr_unlock) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            ULONG disp = 0;
            LONG status = g_hg_ldr_lock(LEVI_LDR_LOCK_LOADER_LOCK_FLAG_TRY_ONLY, &disp, &ldr_cookie);
            if (status == 0 && disp == LEVI_LDR_LOCK_LOADER_LOCK_DISPOSITION_LOCKED) {
                ldr_locked = 1;
                break;
            }
            SwitchToThread();
        }
    }

    HANDLE heaps[LEVI_HEAP_GUARD_MAX_HEAPS];
    BOOLEAN locked[LEVI_HEAP_GUARD_MAX_HEAPS];
    DWORD count = 0;
    HANDLE default_heap = GetProcessHeap();
    if (g_hg_lock_heap && g_hg_unlock_heap) {
        DWORD n = GetProcessHeaps(LEVI_HEAP_GUARD_MAX_HEAPS, heaps);
        if (n > LEVI_HEAP_GUARD_MAX_HEAPS) n = LEVI_HEAP_GUARD_MAX_HEAPS;
        int has_default = 0;
        for (DWORD i = 0; i < n; ++i) {
            if (!heaps[i]) continue;
            if (heaps[i] == default_heap) has_default = 1;
            heaps[count++] = heaps[i];
        }
        if (default_heap && !has_default && count < LEVI_HEAP_GUARD_MAX_HEAPS) {
            heaps[count++] = default_heap;
        }
        for (DWORD i = 0; i < count; ++i) {
            locked[i] = g_hg_lock_heap(heaps[i]);
        }
    }

    DWORD result = orig(thread);
    DWORD error = GetLastError();

    for (DWORD i = count; i > 0; --i) {
        if (locked[i - 1]) g_hg_unlock_heap(heaps[i - 1]);
    }
    if (ldr_locked) {
        (void)g_hg_ldr_unlock(0, ldr_cookie);
    }

    SetLastError(error);
    return result;
}

static int heap_guard_patch_module(HMODULE mod) {
    if (!mod || mod == g_hg_self_module) return 0;
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return 0;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        IMAGE_THUNK_DATA64 *lookup = (IMAGE_THUNK_DATA64 *)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *name = (IMAGE_IMPORT_BY_NAME *)(base + lookup->u1.AddressOfData);
            if (strcmp((const char *)name->Name, "SuspendThread") != 0) continue;

            PVOID current = (PVOID)(ULONG_PTR)iat->u1.Function;
            if (!current || current == (PVOID)&suspend_without_heap_owner) continue;

            HMODULE pinned = NULL;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)&suspend_without_heap_owner, &pinned)) {
                return patched;
            }
            (void)InterlockedCompareExchangePointer((PVOID volatile *)&g_hg_orig_suspend, current, NULL);

            DWORD protection = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &protection)) continue;
            (void)InterlockedExchangePointer((PVOID volatile *)&iat->u1.Function, (PVOID)&suspend_without_heap_owner);
            VirtualProtect(&iat->u1.Function, sizeof(void *), protection, &protection);
            InterlockedIncrement(&g_hg_patched_count);
            patched++;
        }
    }
    return patched;
}

static void heap_guard_patch_loaded_modules(void) {
    HMODULE preloader = GetModuleHandleW(L"PreLoader.dll");
    if (preloader) (void)heap_guard_patch_module(preloader);
    HMODULE lam = GetModuleHandleW(L"LeviLamina.dll");
    if (lam) (void)heap_guard_patch_module(lam);

    if (g_hg_game_dir_len == 0) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me)) {
        do {
            if (!me.hModule || me.hModule == g_hg_self_module) continue;
            if (!levi_heap_guard_is_target_module(me.szExePath, me.szModule,
                                                  g_hg_game_dir, g_hg_game_dir_len)) {
                continue;
            }
            (void)heap_guard_patch_module(me.hModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static void heap_guard_copy_unicode(const LeviHgUnicodeString *us, wchar_t *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!us || !us->Buffer || us->Length == 0) return;
    size_t chars = us->Length / sizeof(wchar_t);
    if (chars >= cap) chars = cap - 1;
    memcpy(out, us->Buffer, chars * sizeof(wchar_t));
    out[chars] = L'\0';
}

static VOID CALLBACK heap_guard_dll_notification(ULONG reason,
                                                 const LeviHgLdrDllNotificationData *data,
                                                 PVOID context) {
    (void)context;
    if (reason != 1u || !data || !data->Loaded.DllBase) return;
    wchar_t full_buf[MAX_PATH];
    wchar_t base_buf[MAX_PATH];
    heap_guard_copy_unicode(data->Loaded.FullDllName, full_buf, MAX_PATH);
    heap_guard_copy_unicode(data->Loaded.BaseDllName, base_buf, MAX_PATH);
    if (!levi_heap_guard_is_target_module(full_buf, base_buf, g_hg_game_dir, g_hg_game_dir_len)) {
        return;
    }
    (void)heap_guard_patch_module((HMODULE)data->Loaded.DllBase);
}

static int start_heap_guard(HMODULE selfModule) {
    if (InterlockedCompareExchange(&g_hg_started, 1, 0) != 0) return 1;
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    g_hg_lock_heap = (LeviLockHeapFn)(void *)GetProcAddress(ntdll, "RtlLockHeap");
    g_hg_unlock_heap = (LeviLockHeapFn)(void *)GetProcAddress(ntdll, "RtlUnlockHeap");
    g_hg_ldr_lock = (LeviLdrLockLoaderLockFn)(void *)GetProcAddress(ntdll, "LdrLockLoaderLock");
    g_hg_ldr_unlock = (LeviLdrUnlockLoaderLockFn)(void *)GetProcAddress(ntdll, "LdrUnlockLoaderLock");
    if (!g_hg_lock_heap || !g_hg_unlock_heap) return 0;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        PVOID real_suspend = (PVOID)(void *)GetProcAddress(k32, "SuspendThread");
        if (real_suspend) {
            (void)InterlockedCompareExchangePointer((PVOID volatile *)&g_hg_orig_suspend, real_suspend, NULL);
        }
    }

    g_hg_self_module = selfModule;
    HMODULE exe = GetModuleHandleW(NULL);
    DWORD len = exe ? GetModuleFileNameW(exe, g_hg_game_dir, MAX_PATH) : 0;
    if (len > 0 && len < MAX_PATH) {
        WCHAR *slash = wcsrchr(g_hg_game_dir, L'\\');
        if (slash) {
            slash[1] = L'\0';
            g_hg_game_dir_len = (size_t)(slash + 1 - g_hg_game_dir);
        }
    }

    heap_guard_patch_loaded_modules();

    LeviHgLdrRegisterDllNotificationFn reg_fn =
        (LeviHgLdrRegisterDllNotificationFn)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (reg_fn) {
        (void)reg_fn(0, heap_guard_dll_notification, NULL, &g_hg_ldr_cookie);
    }

    char buf[160];
    snprintf(buf, sizeof buf,
             "[LeviWineBridge] Wine SuspendThread heap/loader guard ready (patched=%ld)\n",
             (long)InterlockedCompareExchange(&g_hg_patched_count, 0, 0));
    OutputDebugStringA(buf);
    fputs(buf, stderr);
    fflush(stderr);
    return 1;
}

static void install_heap_guard(void) {
    heap_guard_patch_loaded_modules();
}

#endif /* LEVI_HEAP_GUARD_PURE_TEST */
#endif /* LEVI_CLIENT_HEAP_GUARD_H */
