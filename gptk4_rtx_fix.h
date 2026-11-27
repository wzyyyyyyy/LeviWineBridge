/* SPDX-License-Identifier: GPL-3.0-only
 *
 * GPTK 4 (D3DMetal) Ray Tracing (DXR) heap storage-mode fix for Wine.
 *
 * Apple's D3DMetal GPTK 4 beta 3 maps D3D12_HEAP_TYPE_CUSTOM heaps to
 * MTLStorageModeShared even when CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE,
 * while D3D12AccelerationStructure::GetAccelerationStructure hardcodes
 * MTLResourceStorageModePrivate when sub-allocating acceleration-structure
 * header buffers from a placed heap, triggering a fatal Metal assertion:
 *   "The requested storage mode (MTLStorageModePrivate) is not compatible
 *    with the heap's mode (MTLStorageModeShared)"
 *
 * Activated strictly under Wine when D3DMetal DXR is enabled (D3DM_SUPPORT_DXR=1
 * with D3DMETAL_RUNTIME_DIR set, or LEVI_GPTK4_RTX_FIX=1; opt out via
 * LEVI_GPTK4_RTX_FIX=0). Completely inert on native Windows.
 *
 * Without modifying any signed Apple/MetalSharp binaries or executable pages,
 * this hook intercepts d3d12.dll's writable gGFXTDispatch D3D12CreateDevice slot,
 * patches the returned ID3D12Device COM vtable slots to:
 *   1. Normalize D3D12_HEAP_TYPE_CUSTOM + D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE
 *      to D3D12_HEAP_TYPE_DEFAULT in CreateHeap / CreateHeap1 /
 *      CreateCommittedResource / CreateCommittedResource1 / CreateCommittedResource2
 *      so GPU-only heaps use MTLStorageModePrivate natively.
 *   2. As a safety net in CreatePlacedResource / CreatePlacedResource1, redirect
 *      any RAYTRACING_ACCELERATION_STRUCTURE resource placed on a non-DEFAULT heap
 *      to a committed D3D12_HEAP_TYPE_DEFAULT resource.
 */
#ifndef LEVI_WINE_BRIDGE_GPTK4_RTX_FIX_H
#define LEVI_WINE_BRIDGE_GPTK4_RTX_FIX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define LEVI_D3D12_HEAP_TYPE_DEFAULT                                 1u
#define LEVI_D3D12_HEAP_TYPE_UPLOAD                                  2u
#define LEVI_D3D12_HEAP_TYPE_READBACK                                3u
#define LEVI_D3D12_HEAP_TYPE_CUSTOM                                  4u
#define LEVI_D3D12_CPU_PAGE_PROPERTY_UNKNOWN                         0u
#define LEVI_D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE                   1u
#define LEVI_D3D12_MEMORY_POOL_UNKNOWN                               0u
#define LEVI_D3D12_HEAP_FLAG_NONE                                    0u
#define LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE  0x400000u

typedef struct LeviD3D12HeapProperties {
    uint32_t Type;
    uint32_t CPUPageProperty;
    uint32_t MemoryPoolPreference;
    uint32_t CreationNodeMask;
    uint32_t VisibleNodeMask;
} LeviD3D12HeapProperties;

typedef struct LeviD3D12HeapDesc {
    uint64_t SizeInBytes;
    LeviD3D12HeapProperties Properties;
    uint64_t Alignment;
    uint32_t Flags;
    uint32_t _pad;
} LeviD3D12HeapDesc;

static int levi_rtx_ascii_ieq(const char *a, const char *b) {
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

static int levi_rtx_should_activate(const char *override_env,
                                    const char *dxr_env,
                                    const char *runtime_dir) {
    if (override_env && override_env[0]) {
        if (levi_rtx_ascii_ieq(override_env, "0") ||
            levi_rtx_ascii_ieq(override_env, "false") ||
            levi_rtx_ascii_ieq(override_env, "off")) {
            return 0;
        }
        if (levi_rtx_ascii_ieq(override_env, "1") ||
            levi_rtx_ascii_ieq(override_env, "true") ||
            levi_rtx_ascii_ieq(override_env, "on")) {
            return 1;
        }
    }
    if (!dxr_env || strcmp(dxr_env, "1") != 0) return 0;
    if (!runtime_dir || !runtime_dir[0]) return 0;
    return 1;
}

static int levi_rtx_is_d3d12_dll(const wchar_t *name) {
    if (!name || !name[0]) return 0;
    const wchar_t *base = name;
    for (const wchar_t *p = name; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    static const wchar_t target[] = L"d3d12.dll";
    size_t i = 0;
    while (base[i] && target[i]) {
        wchar_t ca = base[i];
        wchar_t cb = target[i];
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (ca != cb) return 0;
        i++;
    }
    return base[i] == L'\0' && target[i] == L'\0';
}

static int levi_rtx_normalize_heap_props(const LeviD3D12HeapProperties *in,
                                         LeviD3D12HeapProperties *out) {
    if (!in || !out) return 0;
    if (in->Type == LEVI_D3D12_HEAP_TYPE_CUSTOM &&
        in->CPUPageProperty == LEVI_D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE) {
        *out = *in;
        out->Type = LEVI_D3D12_HEAP_TYPE_DEFAULT;
        out->CPUPageProperty = LEVI_D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        out->MemoryPoolPreference = LEVI_D3D12_MEMORY_POOL_UNKNOWN;
        return 1;
    }
    return 0;
}

static int levi_rtx_normalize_heap_desc(const LeviD3D12HeapDesc *in,
                                        LeviD3D12HeapDesc *out) {
    if (!in || !out) return 0;
    if (!levi_rtx_normalize_heap_props(&in->Properties, &out->Properties)) return 0;
    out->SizeInBytes = in->SizeInBytes;
    out->Alignment = in->Alignment;
    out->Flags = in->Flags;
    out->_pad = 0;
    return 1;
}

static int levi_rtx_needs_placed_as_fallback(uint32_t initial_state,
                                             const LeviD3D12HeapDesc *heap_desc) {
    if ((initial_state & LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) == 0) {
        return 0;
    }
    if (!heap_desc) return 0;
    return heap_desc->Properties.Type != LEVI_D3D12_HEAP_TYPE_DEFAULT;
}

static int levi_rtx_has_jmp_rax(const uint8_t *code, size_t start, size_t limit) {
    for (size_t j = start; j + 2 <= limit; ++j) {
        if (code[j] == 0xff && code[j + 1] == 0xe0) return 1;
    }
    return 0;
}

/* Scans Apple's d3d12.dll D3D12CreateDevice export for its gGFXTDispatch slot:
 *   GPTK 3: 48 8b 05 <disp32> ; 48 ff e0 (or ff e0)
 *   GPTK 4: 48 8b 05 <disp32> ; 48 85 c0 ; 74 <rel8> (or 0f 84 <rel32>) ... jmp rax
 * Skipping EnsureDXGIInitialized's pCreateDXGIFactory2 load (which uses jne + call rax). */
static int levi_find_d3d12_create_device_slot(const uint8_t *code, size_t max_len,
                                              int32_t *out_disp, size_t *out_next_offset) {
    if (!code || max_len < 9 || !out_disp || !out_next_offset) return 0;
    for (size_t i = 0; i + 9 <= max_len; ++i) {
        if (code[i] != 0x48 || code[i + 1] != 0x8b || code[i + 2] != 0x05) continue;
        int matched = 0;
        /* Case A: direct tail jump (GPTK 3). */
        if ((i + 10 <= max_len && code[i + 7] == 0x48 && code[i + 8] == 0xff && code[i + 9] == 0xe0) ||
            (code[i + 7] == 0xff && code[i + 8] == 0xe0)) {
            matched = 1;
        }
        /* Case B: null-checked tail jump (GPTK 4). */
        if (!matched && i + 12 <= max_len &&
            code[i + 7] == 0x48 && code[i + 8] == 0x85 && code[i + 9] == 0xc0) {
            size_t after_je = 0;
            if (code[i + 10] == 0x74) {
                after_je = i + 12;
            } else if (i + 16 <= max_len && code[i + 10] == 0x0f && code[i + 11] == 0x84) {
                after_je = i + 16;
            }
            if (after_je > 0) {
                size_t scan_end = after_je + 48;
                if (scan_end > max_len) scan_end = max_len;
                if (levi_rtx_has_jmp_rax(code, after_je, scan_end)) {
                    matched = 1;
                }
            }
        }
        if (matched) {
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

#ifndef LEVI_GPTK4_RTX_FIX_PURE_TEST

#include <d3d12.h>
#include <stdio.h>

_Static_assert(sizeof(LeviD3D12HeapProperties) == sizeof(D3D12_HEAP_PROPERTIES),
               "LeviD3D12HeapProperties size mismatch");
_Static_assert(sizeof(LeviD3D12HeapDesc) == sizeof(D3D12_HEAP_DESC),
               "LeviD3D12HeapDesc size mismatch");
_Static_assert(offsetof(ID3D12HeapVtbl, GetDesc) == 8u * sizeof(void *),
               "ID3D12Heap::GetDesc vtable slot mismatch");
_Static_assert(offsetof(ID3D12DeviceVtbl, CreateCommittedResource) == 27u * sizeof(void *),
               "ID3D12Device::CreateCommittedResource vtable slot mismatch");
_Static_assert(offsetof(ID3D12DeviceVtbl, CreateHeap) == 28u * sizeof(void *),
               "ID3D12Device::CreateHeap vtable slot mismatch");
_Static_assert(offsetof(ID3D12DeviceVtbl, CreatePlacedResource) == 29u * sizeof(void *),
               "ID3D12Device::CreatePlacedResource vtable slot mismatch");
_Static_assert(offsetof(ID3D12Device4Vtbl, CreateCommittedResource1) == 53u * sizeof(void *),
               "ID3D12Device4::CreateCommittedResource1 vtable slot mismatch");
_Static_assert(offsetof(ID3D12Device4Vtbl, CreateHeap1) == 54u * sizeof(void *),
               "ID3D12Device4::CreateHeap1 vtable slot mismatch");
_Static_assert(offsetof(ID3D12Device8Vtbl, CreateCommittedResource2) == 69u * sizeof(void *),
               "ID3D12Device8::CreateCommittedResource2 vtable slot mismatch");
_Static_assert(offsetof(ID3D12Device8Vtbl, CreatePlacedResource1) == 70u * sizeof(void *),
               "ID3D12Device8::CreatePlacedResource1 vtable slot mismatch");

static const GUID LEVI_IID_ID3D12Device = {
    0x189819f1u, 0x1db6u, 0x4b57u, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 }
};
static const GUID LEVI_IID_ID3D12Device4 = {
    0xe865df17u, 0xa9eeu, 0x46f9u, { 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5 }
};
static const GUID LEVI_IID_ID3D12Device8 = {
    0x9218e6bbu, 0xf944u, 0x4f7eu, { 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3 }
};

typedef HRESULT (WINAPI *LeviD3D12CreateDeviceFn)(
    IUnknown *pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void **ppDevice);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreateCommittedResourceFn)(
    ID3D12Device *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreateHeapFn)(
    ID3D12Device *self,
    const D3D12_HEAP_DESC *desc,
    REFIID riid,
    void **heap);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreatePlacedResourceFn)(
    ID3D12Device *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreateCommittedResource1Fn)(
    ID3D12Device4 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreateHeap1Fn)(
    ID3D12Device4 *self,
    const D3D12_HEAP_DESC *desc,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **heap);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreateCommittedResource2Fn)(
    ID3D12Device8 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource);

typedef HRESULT (STDMETHODCALLTYPE *LeviCreatePlacedResource1Fn)(
    ID3D12Device8 *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource);

typedef struct LeviRtxUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} LeviRtxUnicodeString;

typedef struct LeviRtxLdrDllLoadedData {
    ULONG Flags;
    const LeviRtxUnicodeString *FullDllName;
    const LeviRtxUnicodeString *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LeviRtxLdrDllLoadedData;

typedef union LeviRtxLdrDllNotificationData {
    LeviRtxLdrDllLoadedData Loaded;
    LeviRtxLdrDllLoadedData Unloaded;
} LeviRtxLdrDllNotificationData;

typedef VOID (CALLBACK *LeviRtxLdrDllNotificationFn)(
    ULONG reason,
    const LeviRtxLdrDllNotificationData *data,
    PVOID context);

typedef LONG (NTAPI *LeviRtxLdrRegisterDllNotificationFn)(
    ULONG flags,
    LeviRtxLdrDllNotificationFn fn,
    PVOID context,
    PVOID *cookie);

static volatile LONG g_rtx_d3d12_hooked = 0;
static volatile LONG g_rtx_watcher_started = 0;
static volatile LONG g_rtx_logged_vtable = 0;
static volatile LONG g_rtx_logged_heap_norm = 0;
static volatile LONG g_rtx_logged_committed_norm = 0;
static volatile LONG g_rtx_logged_placed_fallback = 0;
static PVOID g_rtx_ldr_cookie = NULL;

static LeviD3D12CreateDeviceFn volatile g_orig_d3d12_create_device = NULL;
static LeviCreateCommittedResourceFn volatile g_orig_create_committed = NULL;
static LeviCreateHeapFn volatile g_orig_create_heap = NULL;
static LeviCreatePlacedResourceFn volatile g_orig_create_placed = NULL;
static LeviCreateCommittedResource1Fn volatile g_orig_create_committed1 = NULL;
static LeviCreateHeap1Fn volatile g_orig_create_heap1 = NULL;
static LeviCreateCommittedResource2Fn volatile g_orig_create_committed2 = NULL;
static LeviCreatePlacedResource1Fn volatile g_orig_create_placed1 = NULL;

static void rtx_log_line(const char *msg) {
    OutputDebugStringA(msg);
    fputs(msg, stderr);
    fflush(stderr);
}

static int rtx_patch_pointer_slot(PVOID volatile *slot,
                                  PVOID replacement,
                                  PVOID volatile *saved_orig) {
    if (!slot || !replacement || !saved_orig) return 0;
    PVOID current = InterlockedCompareExchangePointer(slot, NULL, NULL);
    if (!current) return 0;
    if (current == replacement) return 1;

    HMODULE pinned = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)replacement, &pinned)) {
        return 0;
    }

    /* Record the original target before swapping the slot so concurrent callers
     * never observe a NULL saved_orig pointer. */
    (void)InterlockedCompareExchangePointer(saved_orig, current, NULL);

    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)slot, sizeof(PVOID), PAGE_READWRITE, &old_prot)) return 0;
    PVOID prev = InterlockedCompareExchangePointer(slot, replacement, current);
    VirtualProtect((LPVOID)slot, sizeof(PVOID), old_prot, &old_prot);
    return prev == current || prev == replacement;
}

static HRESULT STDMETHODCALLTYPE bridge_create_committed_resource(
    ID3D12Device *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource) {
    LeviCreateCommittedResourceFn orig =
        (LeviCreateCommittedResourceFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_committed, NULL, NULL);
    if (!orig) return E_FAIL;

    LeviD3D12HeapProperties fixed;
    if (heap_properties &&
        levi_rtx_normalize_heap_props((const LeviD3D12HeapProperties *)heap_properties, &fixed)) {
        if (InterlockedCompareExchange(&g_rtx_logged_committed_norm, 1, 0) == 0) {
            rtx_log_line("[LeviWineBridge] GPTK 4 RTX normalized committed CUSTOM+NOT_AVAILABLE heap -> DEFAULT\n");
        }
        return orig(self, (const D3D12_HEAP_PROPERTIES *)&fixed, heap_flags, desc,
                    initial_state, optimized_clear_value, riid, resource);
    }
    return orig(self, heap_properties, heap_flags, desc,
                initial_state, optimized_clear_value, riid, resource);
}

static HRESULT STDMETHODCALLTYPE bridge_create_committed_resource1(
    ID3D12Device4 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource) {
    LeviCreateCommittedResource1Fn orig =
        (LeviCreateCommittedResource1Fn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_committed1, NULL, NULL);
    if (!orig) return E_FAIL;

    LeviD3D12HeapProperties fixed;
    if (heap_properties &&
        levi_rtx_normalize_heap_props((const LeviD3D12HeapProperties *)heap_properties, &fixed)) {
        if (InterlockedCompareExchange(&g_rtx_logged_committed_norm, 1, 0) == 0) {
            rtx_log_line("[LeviWineBridge] GPTK 4 RTX normalized committed CUSTOM+NOT_AVAILABLE heap -> DEFAULT\n");
        }
        return orig(self, (const D3D12_HEAP_PROPERTIES *)&fixed, heap_flags, desc,
                    initial_state, optimized_clear_value, protected_session, riid, resource);
    }
    return orig(self, heap_properties, heap_flags, desc,
                initial_state, optimized_clear_value, protected_session, riid, resource);
}

static HRESULT STDMETHODCALLTYPE bridge_create_committed_resource2(
    ID3D12Device8 *self,
    const D3D12_HEAP_PROPERTIES *heap_properties,
    D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **resource) {
    LeviCreateCommittedResource2Fn orig =
        (LeviCreateCommittedResource2Fn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_committed2, NULL, NULL);
    if (!orig) return E_FAIL;

    LeviD3D12HeapProperties fixed;
    if (heap_properties &&
        levi_rtx_normalize_heap_props((const LeviD3D12HeapProperties *)heap_properties, &fixed)) {
        if (InterlockedCompareExchange(&g_rtx_logged_committed_norm, 1, 0) == 0) {
            rtx_log_line("[LeviWineBridge] GPTK 4 RTX normalized committed CUSTOM+NOT_AVAILABLE heap -> DEFAULT\n");
        }
        return orig(self, (const D3D12_HEAP_PROPERTIES *)&fixed, heap_flags, desc,
                    initial_state, optimized_clear_value, protected_session, riid, resource);
    }
    return orig(self, heap_properties, heap_flags, desc,
                initial_state, optimized_clear_value, protected_session, riid, resource);
}

static HRESULT STDMETHODCALLTYPE bridge_create_heap(
    ID3D12Device *self,
    const D3D12_HEAP_DESC *desc,
    REFIID riid,
    void **heap) {
    LeviCreateHeapFn orig =
        (LeviCreateHeapFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_heap, NULL, NULL);
    if (!orig) return E_FAIL;

    LeviD3D12HeapDesc fixed;
    if (desc && levi_rtx_normalize_heap_desc((const LeviD3D12HeapDesc *)desc, &fixed)) {
        if (InterlockedCompareExchange(&g_rtx_logged_heap_norm, 1, 0) == 0) {
            char buf[192];
            snprintf(buf, sizeof buf,
                     "[LeviWineBridge] GPTK 4 RTX normalized CUSTOM+NOT_AVAILABLE heap -> DEFAULT (size=%llu)\n",
                     (unsigned long long)fixed.SizeInBytes);
            rtx_log_line(buf);
        }
        return orig(self, (const D3D12_HEAP_DESC *)&fixed, riid, heap);
    }
    return orig(self, desc, riid, heap);
}

static HRESULT STDMETHODCALLTYPE bridge_create_heap1(
    ID3D12Device4 *self,
    const D3D12_HEAP_DESC *desc,
    ID3D12ProtectedResourceSession *protected_session,
    REFIID riid,
    void **heap) {
    LeviCreateHeap1Fn orig =
        (LeviCreateHeap1Fn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_heap1, NULL, NULL);
    if (!orig) return E_FAIL;

    LeviD3D12HeapDesc fixed;
    if (desc && levi_rtx_normalize_heap_desc((const LeviD3D12HeapDesc *)desc, &fixed)) {
        if (InterlockedCompareExchange(&g_rtx_logged_heap_norm, 1, 0) == 0) {
            char buf[192];
            snprintf(buf, sizeof buf,
                     "[LeviWineBridge] GPTK 4 RTX normalized CUSTOM+NOT_AVAILABLE heap -> DEFAULT (size=%llu)\n",
                     (unsigned long long)fixed.SizeInBytes);
            rtx_log_line(buf);
        }
        return orig(self, (const D3D12_HEAP_DESC *)&fixed, protected_session, riid, heap);
    }
    return orig(self, desc, protected_session, riid, heap);
}

static HRESULT STDMETHODCALLTYPE bridge_create_placed_resource(
    ID3D12Device *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource) {
    LeviCreatePlacedResourceFn orig =
        (LeviCreatePlacedResourceFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_placed, NULL, NULL);
    if (!orig) return E_FAIL;

    if (heap && heap->lpVtbl && heap->lpVtbl->GetDesc &&
        ((uint32_t)initial_state & LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        D3D12_HEAP_DESC hdesc;
        memset(&hdesc, 0, sizeof hdesc);
        (void)heap->lpVtbl->GetDesc(heap, &hdesc);
        if (levi_rtx_needs_placed_as_fallback((uint32_t)initial_state, (const LeviD3D12HeapDesc *)&hdesc)) {
            LeviCreateCommittedResourceFn committed =
                (LeviCreateCommittedResourceFn)InterlockedCompareExchangePointer(
                    (PVOID volatile *)&g_orig_create_committed, NULL, NULL);
            if (committed) {
                D3D12_HEAP_PROPERTIES default_props;
                memset(&default_props, 0, sizeof default_props);
                default_props.Type = D3D12_HEAP_TYPE_DEFAULT;
                default_props.CreationNodeMask = hdesc.Properties.CreationNodeMask;
                default_props.VisibleNodeMask = hdesc.Properties.VisibleNodeMask;
                if (InterlockedCompareExchange(&g_rtx_logged_placed_fallback, 1, 0) == 0) {
                    char buf[192];
                    snprintf(buf, sizeof buf,
                             "[LeviWineBridge] GPTK 4 RTX redirected non-DEFAULT placed AS (heapType=%u) -> committed DEFAULT\n",
                             (unsigned int)hdesc.Properties.Type);
                    rtx_log_line(buf);
                }
                return committed(self, &default_props, D3D12_HEAP_FLAG_NONE, desc,
                                 initial_state, optimized_clear_value, riid, resource);
            }
        }
    }
    return orig(self, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource);
}

static HRESULT STDMETHODCALLTYPE bridge_create_placed_resource1(
    ID3D12Device8 *self,
    ID3D12Heap *heap,
    UINT64 heap_offset,
    const D3D12_RESOURCE_DESC1 *desc,
    D3D12_RESOURCE_STATES initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value,
    REFIID riid,
    void **resource) {
    LeviCreatePlacedResource1Fn orig =
        (LeviCreatePlacedResource1Fn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_create_placed1, NULL, NULL);
    if (!orig) return E_FAIL;

    if (heap && heap->lpVtbl && heap->lpVtbl->GetDesc &&
        ((uint32_t)initial_state & LEVI_D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) != 0) {
        D3D12_HEAP_DESC hdesc;
        memset(&hdesc, 0, sizeof hdesc);
        (void)heap->lpVtbl->GetDesc(heap, &hdesc);
        if (levi_rtx_needs_placed_as_fallback((uint32_t)initial_state, (const LeviD3D12HeapDesc *)&hdesc)) {
            LeviCreateCommittedResource2Fn committed2 =
                (LeviCreateCommittedResource2Fn)InterlockedCompareExchangePointer(
                    (PVOID volatile *)&g_orig_create_committed2, NULL, NULL);
            if (committed2) {
                D3D12_HEAP_PROPERTIES default_props;
                memset(&default_props, 0, sizeof default_props);
                default_props.Type = D3D12_HEAP_TYPE_DEFAULT;
                default_props.CreationNodeMask = hdesc.Properties.CreationNodeMask;
                default_props.VisibleNodeMask = hdesc.Properties.VisibleNodeMask;
                if (InterlockedCompareExchange(&g_rtx_logged_placed_fallback, 1, 0) == 0) {
                    char buf[192];
                    snprintf(buf, sizeof buf,
                             "[LeviWineBridge] GPTK 4 RTX redirected non-DEFAULT placed AS (heapType=%u) -> committed DEFAULT\n",
                             (unsigned int)hdesc.Properties.Type);
                    rtx_log_line(buf);
                }
                return committed2(self, &default_props, D3D12_HEAP_FLAG_NONE, desc,
                                  initial_state, optimized_clear_value, NULL, riid, resource);
            }
        }
    }
    return orig(self, heap, heap_offset, desc, initial_state, optimized_clear_value, riid, resource);
}

static void rtx_hook_device_vtable(IUnknown *obj) {
    if (!obj || !obj->lpVtbl || !obj->lpVtbl->QueryInterface) return;

    unsigned long hooked_mask = 0;
    ID3D12Device *dev = NULL;
    if (SUCCEEDED(obj->lpVtbl->QueryInterface(obj, &LEVI_IID_ID3D12Device, (void **)&dev)) && dev) {
        PVOID volatile *vtbl = *(PVOID volatile **)dev;
        if (vtbl) {
            if (rtx_patch_pointer_slot(&vtbl[27], (PVOID)&bridge_create_committed_resource,
                                       (PVOID volatile *)&g_orig_create_committed)) {
                hooked_mask |= 1ul << 0;
            }
            if (rtx_patch_pointer_slot(&vtbl[28], (PVOID)&bridge_create_heap,
                                       (PVOID volatile *)&g_orig_create_heap)) {
                hooked_mask |= 1ul << 1;
            }
            if (rtx_patch_pointer_slot(&vtbl[29], (PVOID)&bridge_create_placed_resource,
                                       (PVOID volatile *)&g_orig_create_placed)) {
                hooked_mask |= 1ul << 2;
            }
        }
        dev->lpVtbl->Release(dev);
    }

    ID3D12Device4 *dev4 = NULL;
    if (SUCCEEDED(obj->lpVtbl->QueryInterface(obj, &LEVI_IID_ID3D12Device4, (void **)&dev4)) && dev4) {
        PVOID volatile *vtbl4 = *(PVOID volatile **)dev4;
        if (vtbl4) {
            if (rtx_patch_pointer_slot(&vtbl4[53], (PVOID)&bridge_create_committed_resource1,
                                       (PVOID volatile *)&g_orig_create_committed1)) {
                hooked_mask |= 1ul << 3;
            }
            if (rtx_patch_pointer_slot(&vtbl4[54], (PVOID)&bridge_create_heap1,
                                       (PVOID volatile *)&g_orig_create_heap1)) {
                hooked_mask |= 1ul << 4;
            }
        }
        dev4->lpVtbl->Release(dev4);
    }

    ID3D12Device8 *dev8 = NULL;
    if (SUCCEEDED(obj->lpVtbl->QueryInterface(obj, &LEVI_IID_ID3D12Device8, (void **)&dev8)) && dev8) {
        PVOID volatile *vtbl8 = *(PVOID volatile **)dev8;
        if (vtbl8) {
            if (rtx_patch_pointer_slot(&vtbl8[69], (PVOID)&bridge_create_committed_resource2,
                                       (PVOID volatile *)&g_orig_create_committed2)) {
                hooked_mask |= 1ul << 5;
            }
            if (rtx_patch_pointer_slot(&vtbl8[70], (PVOID)&bridge_create_placed_resource1,
                                       (PVOID volatile *)&g_orig_create_placed1)) {
                hooked_mask |= 1ul << 6;
            }
        }
        dev8->lpVtbl->Release(dev8);
    }

    if (hooked_mask && InterlockedCompareExchange(&g_rtx_logged_vtable, 1, 0) == 0) {
        char buf[160];
        snprintf(buf, sizeof buf,
                 "[LeviWineBridge] GPTK 4 RTX device vtable hooked (slots=0x%lx)\n",
                 hooked_mask);
        rtx_log_line(buf);
    }
}

static HRESULT WINAPI bridge_d3d12_create_device(
    IUnknown *pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void **ppDevice) {
    LeviD3D12CreateDeviceFn orig =
        (LeviD3D12CreateDeviceFn)InterlockedCompareExchangePointer(
            (PVOID volatile *)&g_orig_d3d12_create_device, NULL, NULL);
    if (!orig) return E_FAIL;

    HRESULT hr = orig(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        rtx_hook_device_vtable((IUnknown *)(*ppDevice));
    }
    return hr;
}

static int rtx_try_hook_d3d12(HMODULE mod) {
    if (!mod) return 0;
    if (InterlockedCompareExchange(&g_rtx_d3d12_hooked, 0, 0) != 0) return 1;
    FARPROC proc = GetProcAddress(mod, "D3D12CreateDevice");
    if (!proc) return 0;
    const uint8_t *code = (const uint8_t *)(void *)proc;
    int32_t disp = 0;
    size_t next_off = 0;
    if (!levi_find_d3d12_create_device_slot(code, 160, &disp, &next_off)) return 0;
    PVOID volatile *slot = (PVOID volatile *)(code + next_off + (ptrdiff_t)disp);
    if (!rtx_patch_pointer_slot(slot, (PVOID)&bridge_d3d12_create_device,
                                (PVOID volatile *)&g_orig_d3d12_create_device)) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_rtx_d3d12_hooked, 1, 0) == 0) {
        rtx_log_line("[LeviWineBridge] GPTK 4 RTX heap hook armed\n");
    }
    return 1;
}

static DWORD WINAPI rtx_hook_watcher_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 1200; ++i) {
        HMODULE mod = GetModuleHandleW(L"d3d12.dll");
        if (mod && rtx_try_hook_d3d12(mod)) return 0;
        Sleep(25);
    }
    return 0;
}

static void rtx_ensure_watcher(void) {
    if (InterlockedCompareExchange(&g_rtx_watcher_started, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, rtx_hook_watcher_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

static VOID CALLBACK rtx_dll_notification(ULONG reason,
                                          const LeviRtxLdrDllNotificationData *data,
                                          PVOID context) {
    (void)context;
    if (reason != 1u || !data) return;
    const LeviRtxUnicodeString *base_name = data->Loaded.BaseDllName;
    const LeviRtxUnicodeString *full_name = data->Loaded.FullDllName;
    const wchar_t *probe = (base_name && base_name->Buffer) ? base_name->Buffer
                         : (full_name && full_name->Buffer) ? full_name->Buffer : NULL;
    if (!probe || !levi_rtx_is_d3d12_dll(probe)) return;
    HMODULE mod = (HMODULE)data->Loaded.DllBase;
    if (!rtx_try_hook_d3d12(mod)) {
        rtx_ensure_watcher();
    }
}

static int start_gptk4_rtx_fix(HMODULE selfModule) {
    (void)selfModule;
    /* Strictly Wine-only: completely inert on native Windows. */
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return 0;

    char override_buf[16] = {0};
    char dxr_buf[16] = {0};
    char runtime_dir[512] = {0};
    DWORD n_ov = GetEnvironmentVariableA("LEVI_GPTK4_RTX_FIX", override_buf, sizeof override_buf);
    DWORD n_dxr = GetEnvironmentVariableA("D3DM_SUPPORT_DXR", dxr_buf, sizeof dxr_buf);
    DWORD n_dir = GetEnvironmentVariableA("D3DMETAL_RUNTIME_DIR", runtime_dir, sizeof runtime_dir);

    if (!levi_rtx_should_activate(n_ov && n_ov < sizeof override_buf ? override_buf : NULL,
                                  n_dxr && n_dxr < sizeof dxr_buf ? dxr_buf : NULL,
                                  n_dir && n_dir < sizeof runtime_dir ? runtime_dir : NULL)) {
        return 0;
    }

    HMODULE existing = GetModuleHandleW(L"d3d12.dll");
    if (existing && rtx_try_hook_d3d12(existing)) {
        return 1;
    }

    LeviRtxLdrRegisterDllNotificationFn reg_fn =
        (LeviRtxLdrRegisterDllNotificationFn)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (reg_fn && reg_fn(0, rtx_dll_notification, NULL, &g_rtx_ldr_cookie) == 0) {
        if (existing) rtx_ensure_watcher();
        return 1;
    }

    rtx_ensure_watcher();
    return 1;
}

#endif /* LEVI_GPTK4_RTX_FIX_PURE_TEST */
#endif /* LEVI_WINE_BRIDGE_GPTK4_RTX_FIX_H */
