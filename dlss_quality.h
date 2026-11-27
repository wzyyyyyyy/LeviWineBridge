/* SPDX-License-Identifier: GPL-3.0-only
 *
 * GPTK (D3DMetal 3/4) DLSS quality preset override for Minecraft Bedrock under Wine.
 *
 * Activated only under Wine when LEVI_GPTK_DLSS_QUALITY is set to one of:
 *   ultra_performance | performance | balanced | dlaa
 * Completely inert on native Windows, when unset, or when set to quality / auto.
 *
 * Apple's nvngx-on-metalfx.dll dispatches all NVSDK_NGX_D3D12_* exports through
 * writable slots in gGFXTDispatch (loaded via `mov rax, [rip+disp32]; test rax, rax`).
 * Without modifying any signed files or executable code pages, this hook swaps the
 * dispatch slots for GetCapabilityParameters, GetParameters, and CreateFeature to
 * intercept DLSSOptimalSettingsCallback and replace Bedrock's hardcoded Quality (2)
 * PerfQualityValue with the configured preset before Apple's callback computes the
 * internal MetalFX Temporal render resolution.
 */
#ifndef LEVI_WINE_BRIDGE_DLSS_QUALITY_H
#define LEVI_WINE_BRIDGE_DLSS_QUALITY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* NVIDIA NGX SDK NVSDK_NGX_PerfQuality_Value enum values. */
#define LEVI_NGX_PERF_QUALITY_PERFORMANCE       0
#define LEVI_NGX_PERF_QUALITY_BALANCED          1
#define LEVI_NGX_PERF_QUALITY_QUALITY           2
#define LEVI_NGX_PERF_QUALITY_ULTRA_PERFORMANCE 3
#define LEVI_NGX_PERF_QUALITY_ULTRA_QUALITY     4
#define LEVI_NGX_PERF_QUALITY_DLAA              5

#define LEVI_NGX_FEATURE_SUPER_SAMPLING 1u
#define LEVI_NGX_RESULT_SUCCESS         1u

static int levi_ascii_ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Returns an NVSDK_NGX_PerfQuality_Value enum when override should activate,
 * or -1 when unset, "auto", "quality" (Bedrock's native default), or invalid. */
static int levi_parse_gptk_dlss_quality(const char *value) {
    if (!value || !value[0]) return -1;
    while (*value == ' ' || *value == '\t') value++;
    char buf[32];
    size_t len = 0;
    while (value[len] && value[len] != ' ' && value[len] != '\t' &&
           value[len] != '\r' && value[len] != '\n' && len + 1 < sizeof buf) {
        buf[len] = value[len];
        len++;
    }
    buf[len] = '\0';
    if (levi_ascii_ieq(buf, "ultra_performance")) return LEVI_NGX_PERF_QUALITY_ULTRA_PERFORMANCE;
    if (levi_ascii_ieq(buf, "performance")) return LEVI_NGX_PERF_QUALITY_PERFORMANCE;
    if (levi_ascii_ieq(buf, "balanced")) return LEVI_NGX_PERF_QUALITY_BALANCED;
    if (levi_ascii_ieq(buf, "dlaa")) return LEVI_NGX_PERF_QUALITY_DLAA;
    return -1;
}

static const char *levi_dlss_quality_name(int quality) {
    switch (quality) {
    case LEVI_NGX_PERF_QUALITY_ULTRA_PERFORMANCE:
        return "ultra_performance";
    case LEVI_NGX_PERF_QUALITY_PERFORMANCE:
        return "performance";
    case LEVI_NGX_PERF_QUALITY_BALANCED:
        return "balanced";
    case LEVI_NGX_PERF_QUALITY_QUALITY:
        return "quality";
    case LEVI_NGX_PERF_QUALITY_DLAA:
        return "dlaa";
    default:
        return "unknown";
    }
}

static int levi_wide_ieq(const wchar_t *a, const wchar_t *b) {
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

/* Under MetalSharp + D3DMetal, Wine redirects nvngx.dll / _nvngx.dll directly to
 * Apple's nvngx-on-metalfx.dll while keeping the PEB module name as nvngx.dll. */
static int levi_dlss_is_target_dll(const wchar_t *name) {
    if (!name || !name[0]) return 0;
    const wchar_t *base = name;
    for (const wchar_t *p = name; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    return levi_wide_ieq(base, L"nvngx-on-metalfx.dll") ||
           levi_wide_ieq(base, L"nvngx.dll") ||
           levi_wide_ieq(base, L"_nvngx.dll");
}

/* Scans the start of an Apple nvngx-on-metalfx.dll export stub for:
 *   48 8b 05 <disp32>   mov rax, [rip + disp32]
 *   48 85 c0            test rax, rax
 * Matches both GPTK 3 (offset 0) and GPTK 4 beta 3 (after prologue push/sub). */
static int levi_find_rip_mov_rax_slot(const uint8_t *code, size_t max_len,
                                      int32_t *out_disp, size_t *out_next_offset) {
    if (!code || max_len < 10 || !out_disp || !out_next_offset) return 0;
    for (size_t i = 0; i + 10 <= max_len; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8b && code[i + 2] == 0x05 &&
            code[i + 7] == 0x48 && code[i + 8] == 0x85 && code[i + 9] == 0xc0) {
            uint32_t raw = (uint32_t)code[i + 3] |
                           ((uint32_t)code[i + 4] << 8) |
                           ((uint32_t)code[i + 5] << 16) |
                           ((uint32_t)code[i + 6] << 24);
            *out_disp = (int32_t)raw;
            *out_next_offset = i + 7;
            return 1;
        }
    }
    return 0;
}

typedef struct LeviNgxParameter LeviNgxParameter;

/* Matches MSVC / Apple D3DMetal NVSDK_NGX_Parameter virtual table layout. */
typedef struct LeviNgxParameterVtbl {
    void (*SetVoidPointer)(LeviNgxParameter *self, const char *name, void *value);
    void (*SetD3D12Resource)(LeviNgxParameter *self, const char *name, void *value);
    void (*SetD3D11Resource)(LeviNgxParameter *self, const char *name, void *value);
    void (*SetInt)(LeviNgxParameter *self, const char *name, int value);
    void (*SetUInt)(LeviNgxParameter *self, const char *name, unsigned int value);
    void (*SetDouble)(LeviNgxParameter *self, const char *name, double value);
    void (*SetFloat)(LeviNgxParameter *self, const char *name, float value);
    void (*SetULL)(LeviNgxParameter *self, const char *name, unsigned long long value);
    uint32_t (*GetVoidPointer)(const LeviNgxParameter *self, const char *name, void **out);
    uint32_t (*GetD3D12Resource)(const LeviNgxParameter *self, const char *name, void **out);
    uint32_t (*GetD3D11Resource)(const LeviNgxParameter *self, const char *name, void **out);
    uint32_t (*GetInt)(const LeviNgxParameter *self, const char *name, int *out);
    uint32_t (*GetUInt)(const LeviNgxParameter *self, const char *name, unsigned int *out);
    uint32_t (*GetDouble)(const LeviNgxParameter *self, const char *name, double *out);
    uint32_t (*GetFloat)(const LeviNgxParameter *self, const char *name, float *out);
    uint32_t (*GetULL)(const LeviNgxParameter *self, const char *name, unsigned long long *out);
    void (*Reset)(LeviNgxParameter *self);
} LeviNgxParameterVtbl;

struct LeviNgxParameter {
    const LeviNgxParameterVtbl *lpVtbl;
};

typedef uint32_t (*LeviNgxOptimalSettingsCb)(LeviNgxParameter *params);

static int levi_ngx_succeeded(uint32_t result) {
    return result == LEVI_NGX_RESULT_SUCCESS || (result & 0xFFF00000u) == 0x00100000u;
}

static int levi_ngx_wrap_optimal_callback(LeviNgxParameter *params,
                                          LeviNgxOptimalSettingsCb wrapper_cb,
                                          LeviNgxOptimalSettingsCb *saved_orig_cb) {
    if (!params || !params->lpVtbl || !params->lpVtbl->GetVoidPointer ||
        !params->lpVtbl->SetVoidPointer || !wrapper_cb || !saved_orig_cb) {
        return 0;
    }
    void *existing = NULL;
    uint32_t res = params->lpVtbl->GetVoidPointer(params, "DLSSOptimalSettingsCallback", &existing);
    if (!levi_ngx_succeeded(res) || !existing) return 0;
    if (existing == (void *)wrapper_cb) return 1;
    *saved_orig_cb = (LeviNgxOptimalSettingsCb)existing;
    params->lpVtbl->SetVoidPointer(params, "DLSSOptimalSettingsCallback", (void *)wrapper_cb);
    return 1;
}

static void levi_ngx_apply_perf_quality(LeviNgxParameter *params, int target_quality) {
    if (!params || !params->lpVtbl || target_quality < 0) return;
    if (params->lpVtbl->SetInt) {
        params->lpVtbl->SetInt(params, "PerfQualityValue", target_quality);
    }
    if (params->lpVtbl->SetUInt) {
        params->lpVtbl->SetUInt(params, "PerfQualityValue", (unsigned int)target_quality);
    }
}

#ifndef LEVI_DLSS_QUALITY_PURE_TEST

#include <stdio.h>

typedef uint32_t (*LeviNgxGetParamsFn)(LeviNgxParameter **out_params);
typedef uint32_t (*LeviNgxCreateFeatureFn)(void *cmd_list, uint32_t feature_id,
                                           LeviNgxParameter *params, void **out_handle);

typedef struct LeviUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} LeviUnicodeString;

typedef struct LeviLdrDllLoadedNotificationData {
    ULONG Flags;
    const LeviUnicodeString *FullDllName;
    const LeviUnicodeString *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LeviLdrDllLoadedNotificationData;

typedef union LeviLdrDllNotificationData {
    LeviLdrDllLoadedNotificationData Loaded;
    LeviLdrDllLoadedNotificationData Unloaded;
} LeviLdrDllNotificationData;

typedef VOID (CALLBACK *LeviLdrDllNotificationFn)(ULONG reason,
                                                  const LeviLdrDllNotificationData *data,
                                                  PVOID context);
typedef LONG (NTAPI *LeviLdrRegisterDllNotificationFn)(ULONG flags,
                                                     LeviLdrDllNotificationFn fn,
                                                     PVOID context,
                                                     PVOID *cookie);

#define LEVI_LDR_DLL_NOTIFICATION_REASON_LOADED 1u

static int g_dlss_target_quality = -1;
static volatile LONG g_dlss_hooked_mask = 0;
static volatile LONG g_dlss_logged_active = 0;
static volatile LONG g_dlss_watcher_started = 0;
static PVOID g_dlss_ldr_cookie = NULL;

static LeviNgxGetParamsFn volatile g_orig_get_cap_params = NULL;
static LeviNgxGetParamsFn volatile g_orig_get_params = NULL;
static LeviNgxGetParamsFn volatile g_orig_alloc_params = NULL;
static LeviNgxCreateFeatureFn volatile g_orig_create_feature = NULL;
static LeviNgxOptimalSettingsCb volatile g_orig_optimal_cb = NULL;

static void dlss_log_line(const char *msg) {
    OutputDebugStringA(msg);
    fputs(msg, stderr);
    fflush(stderr);
}

static uint32_t levi_dlss_optimal_settings_callback(LeviNgxParameter *params) {
    LeviNgxOptimalSettingsCb orig =
        (LeviNgxOptimalSettingsCb)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_optimal_cb, NULL, NULL);
    if (!orig) return 0xBAD00000u;
    levi_ngx_apply_perf_quality(params, g_dlss_target_quality);
    uint32_t res = orig(params);
    if (InterlockedCompareExchange(&g_dlss_logged_active, 1, 0) == 0) {
        unsigned int out_w = 0, out_h = 0;
        if (params && params->lpVtbl && params->lpVtbl->GetUInt) {
            (void)params->lpVtbl->GetUInt(params, "OutWidth", &out_w);
            (void)params->lpVtbl->GetUInt(params, "OutHeight", &out_h);
        }
        char buf[192];
        snprintf(buf, sizeof buf,
                 "[LeviWineBridge] GPTK DLSS quality override active: %s (PerfQualityValue=%d, render=%ux%u)\n",
                 levi_dlss_quality_name(g_dlss_target_quality), g_dlss_target_quality, out_w, out_h);
        dlss_log_line(buf);
    }
    return res;
}

static void dlss_wrap_params_object(LeviNgxParameter *params) {
    LeviNgxOptimalSettingsCb orig = NULL;
    if (levi_ngx_wrap_optimal_callback(params, levi_dlss_optimal_settings_callback, &orig) && orig) {
        InterlockedExchangePointer((PVOID volatile *)&g_orig_optimal_cb, (PVOID)orig);
    }
}

static uint32_t bridge_ngx_get_cap_params(LeviNgxParameter **out_params) {
    LeviNgxGetParamsFn orig =
        (LeviNgxGetParamsFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_get_cap_params, NULL, NULL);
    if (!orig) return 0xBAD00000u;
    uint32_t res = orig(out_params);
    if (levi_ngx_succeeded(res) && out_params && *out_params) {
        dlss_wrap_params_object(*out_params);
    }
    return res;
}

static uint32_t bridge_ngx_get_params(LeviNgxParameter **out_params) {
    LeviNgxGetParamsFn orig =
        (LeviNgxGetParamsFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_get_params, NULL, NULL);
    if (!orig) return 0xBAD00000u;
    uint32_t res = orig(out_params);
    if (levi_ngx_succeeded(res) && out_params && *out_params) {
        dlss_wrap_params_object(*out_params);
    }
    return res;
}

static uint32_t bridge_ngx_alloc_params(LeviNgxParameter **out_params) {
    LeviNgxGetParamsFn orig =
        (LeviNgxGetParamsFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_alloc_params, NULL, NULL);
    if (!orig) return 0xBAD00000u;
    uint32_t res = orig(out_params);
    if (levi_ngx_succeeded(res) && out_params && *out_params) {
        dlss_wrap_params_object(*out_params);
    }
    return res;
}

static uint32_t bridge_ngx_create_feature(void *cmd_list, uint32_t feature_id,
                                          LeviNgxParameter *params, void **out_handle) {
    LeviNgxCreateFeatureFn orig =
        (LeviNgxCreateFeatureFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_feature, NULL, NULL);
    if (!orig) return 0xBAD00000u;
    if (feature_id == LEVI_NGX_FEATURE_SUPER_SAMPLING && params) {
        levi_ngx_apply_perf_quality(params, g_dlss_target_quality);
    }
    return orig(cmd_list, feature_id, params, out_handle);
}

static int dlss_hook_dispatch_slot(HMODULE mod, const char *export_name,
                                   PVOID replacement, PVOID volatile *saved_orig,
                                   LONG mask_bit) {
    if (InterlockedCompareExchange(&g_dlss_hooked_mask, 0, 0) & mask_bit) return 1;
    FARPROC proc = GetProcAddress(mod, export_name);
    if (!proc) return 0;
    const uint8_t *code = (const uint8_t *)(void *)proc;
    int32_t disp = 0;
    size_t next_off = 0;
    if (!levi_find_rip_mov_rax_slot(code, 32, &disp, &next_off)) return 0;
    PVOID volatile *slot = (PVOID volatile *)(code + next_off + (ptrdiff_t)disp);
    PVOID current = InterlockedCompareExchangePointer(slot, NULL, NULL);
    if (!current) return 0;
    if (current == replacement) {
        InterlockedOr(&g_dlss_hooked_mask, mask_bit);
        return 1;
    }
    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)replacement, &pinned)) {
        return 0;
    }
    (void)InterlockedCompareExchangePointer(saved_orig, current, NULL);
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)slot, sizeof(PVOID), PAGE_READWRITE, &old_prot)) return 0;
    PVOID prev = InterlockedCompareExchangePointer(slot, replacement, current);
    VirtualProtect((LPVOID)slot, sizeof(PVOID), old_prot, &old_prot);
    if (prev == current || prev == replacement) {
        InterlockedOr(&g_dlss_hooked_mask, mask_bit);
        return 1;
    }
    return 0;
}

static HMODULE dlss_find_target_module(void) {
    static const wchar_t *const candidates[] = {
        L"nvngx-on-metalfx.dll",
        L"nvngx.dll",
        L"_nvngx.dll",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        HMODULE mod = GetModuleHandleW(candidates[i]);
        if (mod) return mod;
    }
    return NULL;
}

static int dlss_try_hook_module(HMODULE mod) {
    if (!mod) return 0;
    int ok = 0;
    ok += dlss_hook_dispatch_slot(mod, "NVSDK_NGX_D3D12_GetCapabilityParameters",
                                  (PVOID)&bridge_ngx_get_cap_params,
                                  (PVOID volatile *)&g_orig_get_cap_params, 1);
    ok += dlss_hook_dispatch_slot(mod, "NVSDK_NGX_D3D12_GetParameters",
                                  (PVOID)&bridge_ngx_get_params,
                                  (PVOID volatile *)&g_orig_get_params, 2);
    ok += dlss_hook_dispatch_slot(mod, "NVSDK_NGX_D3D12_CreateFeature",
                                  (PVOID)&bridge_ngx_create_feature,
                                  (PVOID volatile *)&g_orig_create_feature, 4);
    (void)dlss_hook_dispatch_slot(mod, "NVSDK_NGX_D3D12_AllocateParameters",
                                  (PVOID)&bridge_ngx_alloc_params,
                                  (PVOID volatile *)&g_orig_alloc_params, 8);
    return ok == 3;
}

static DWORD WINAPI dlss_hook_watcher_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 600; ++i) {
        HMODULE mod = dlss_find_target_module();
        if (mod && dlss_try_hook_module(mod)) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "[LeviWineBridge] GPTK DLSS quality hook armed: %s (PerfQualityValue=%d)\n",
                     levi_dlss_quality_name(g_dlss_target_quality), g_dlss_target_quality);
            dlss_log_line(buf);
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

static void dlss_ensure_watcher(void) {
    if (InterlockedCompareExchange(&g_dlss_watcher_started, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, dlss_hook_watcher_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

static int dlss_unicode_matches_target(const LeviUnicodeString *us) {
    if (!us || !us->Buffer || us->Length == 0) return 0;
    size_t chars = us->Length / sizeof(wchar_t);
    if (chars >= 260) chars = 259;
    wchar_t tmp[260];
    memcpy(tmp, us->Buffer, chars * sizeof(wchar_t));
    tmp[chars] = L'\0';
    return levi_dlss_is_target_dll(tmp);
}

static VOID CALLBACK dlss_dll_notification(ULONG reason,
                                           const LeviLdrDllNotificationData *data,
                                           PVOID context) {
    (void)context;
    if (reason != LEVI_LDR_DLL_NOTIFICATION_REASON_LOADED || !data) return;
    if (!dlss_unicode_matches_target(data->Loaded.BaseDllName) &&
        !dlss_unicode_matches_target(data->Loaded.FullDllName)) {
        return;
    }
    HMODULE mod = (HMODULE)data->Loaded.DllBase;
    if (dlss_try_hook_module(mod)) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "[LeviWineBridge] GPTK DLSS quality hook armed: %s (PerfQualityValue=%d)\n",
                 levi_dlss_quality_name(g_dlss_target_quality), g_dlss_target_quality);
        dlss_log_line(buf);
    } else {
        /* If DllMain has not yet populated gGFXTDispatch, finish hooking asynchronously. */
        dlss_ensure_watcher();
    }
}

static int start_dlss_quality(HMODULE selfModule) {
    (void)selfModule;
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char raw[32];
    DWORD n = GetEnvironmentVariableA("LEVI_GPTK_DLSS_QUALITY", raw, sizeof raw);
    if (n == 0 || n >= sizeof raw) return 0;

    int quality = levi_parse_gptk_dlss_quality(raw);
    if (quality < 0) return 0;
    g_dlss_target_quality = quality;

    HMODULE existing = dlss_find_target_module();
    if (existing && dlss_try_hook_module(existing)) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "[LeviWineBridge] GPTK DLSS quality hook armed: %s (PerfQualityValue=%d)\n",
                 levi_dlss_quality_name(g_dlss_target_quality), g_dlss_target_quality);
        dlss_log_line(buf);
        return 1;
    }

    LeviLdrRegisterDllNotificationFn reg_fn =
        (LeviLdrRegisterDllNotificationFn)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (reg_fn && reg_fn(0, dlss_dll_notification, NULL, &g_dlss_ldr_cookie) == 0) {
        if (existing) dlss_ensure_watcher();
        return 1;
    }

    /* Fallback if LdrRegisterDllNotification is unavailable. */
    dlss_ensure_watcher();
    return 1;
}

#endif /* LEVI_DLSS_QUALITY_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_DLSS_QUALITY_H */
