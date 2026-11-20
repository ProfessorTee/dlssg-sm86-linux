// dlssg-sm86-linux: LD_PRELOAD shim that lets the DLSS Frame Generation snippet run on RTX 30 (Ampere) under native
// Linux Vulkan. Stage 1: lift the NGX core's snippet arch gate (NGXValidateSnippetMetaData: "snippet is using newer GPU
// arch 190 > 170").
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

static int g_verbose = 1;

void shim_log(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[dlssg-sm86] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// Byte pattern search with 0x100 = wildcard.
static uint8_t *find_pattern(uint8_t *start, size_t len, const uint16_t *pat, size_t plen, int *count) {
    uint8_t *hit = NULL;
    *count = 0;
    for (size_t i = 0; i + plen <= len; i++) {
        size_t j = 0;
        while (j < plen && (pat[j] == 0x100 || start[i + j] == pat[j])) j++;
        if (j == plen) {
            if (!hit) hit = start + i;
            (*count)++;
        }
    }
    return hit;
}

static int patch_bytes(uint8_t *at, const uint8_t *bytes, size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)at & ~(uintptr_t)(pg - 1);
    size_t span = ((uintptr_t)at + n - a + pg - 1) & ~(size_t)(pg - 1);
    if (mprotect((void *)a, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    memcpy(at, bytes, n);
    mprotect((void *)a, span, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)at + n);
    return 0;
}

// Executable PT_LOAD segment of a loaded object.
struct seg { const char *want; uint8_t *base; size_t len; char path[512]; };

static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz;
    struct seg *s = data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, s->want)) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) {
            s->base = (uint8_t *)(info->dlpi_addr + p->p_vaddr);
            s->len = p->p_memsz;
            snprintf(s->path, sizeof s->path, "%s", info->dlpi_name);
            return 1;
        }
    }
    return 0;
}

static int find_text(const char *name, struct seg *s) {
    memset(s, 0, sizeof *s);
    s->want = name;
    return dl_iterate_phdr(phdr_cb, s) == 1;
}

static int g_core_patched;

// libnvidia-ngx.so (r620): mov r8d,[rbx+0x10] ; mov r9d,[rax+arch] ; cmp r8d,r9d ; jg <reject>
// r8d = snippet's minimum arch, r9d = GPU arch. The jg is replaced with NOPs.
static void patch_core(void) {
    if (g_core_patched) return;
    struct seg s;
    if (!find_text("libnvidia-ngx.so", &s)) return;
    static const uint16_t pat[] = {0x44, 0x8b, 0x43, 0x10, 0x44, 0x8b, 0x88, 0x100, 0x100, 0x00, 0x00,
                                   0x45, 0x39, 0xc8, 0x0f, 0x8f};
    int n;
    uint8_t *hit = find_pattern(s.base, s.len, pat, sizeof pat / sizeof pat[0], &n);
    g_core_patched = 1;
    if (n != 1) {
        shim_log("core %s: arch gate pattern found %d times, not patched", s.path, n);
        return;
    }
    static const uint8_t nop6[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    if (patch_bytes(hit + 14, nop6, 6)) {
        shim_log("core: mprotect failed");
        return;
    }
    shim_log("core %s: snippet arch gate lifted at +0x%lx", s.path, (unsigned long)(hit + 14 - s.base));

    // Per-feature minimum arch (feature table +0x28) against the GPU's: in the capability pass ("<Feature>.Available",
    // jge <available>) and in NVSDK_NGX_CreateFeature_Validate ("feature is not supported on this device", jl <reject>).
    // ponytail: lifts the arch minimum for every feature, not only dlssg; a feature that really needs newer hardware
    // then fails inside its own snippet instead of here.
    static const struct { const char *what; uint16_t pat[16]; size_t len; size_t at; uint8_t rep[6]; size_t rlen; } sites[] = {
        {"available", {0x41, 0x8b, 0x4c, 0x24, 0x28, 0x39, 0x48, 0x04, 0x0f, 0x8d}, 10, 8, {0x90, 0xe9}, 2},
        {"available", {0x41, 0x8b, 0x74, 0x24, 0x28, 0x39, 0x70, 0x04, 0x0f, 0x8d}, 10, 8, {0x90, 0xe9}, 2},
        {"validate", {0x8b, 0x84, 0x05, 0x88, 0x8c, 0x00, 0x00, 0x41, 0x39, 0x40, 0x04, 0x0f, 0x8c}, 13, 11,
         {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, 6},
        {"validate", {0x41, 0x39, 0x40, 0x04, 0x7c}, 5, 4, {0x66, 0x90}, 2},
    };
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        uint8_t *h = find_pattern(s.base, s.len, sites[i].pat, sites[i].len, &n);
        if (i == 3) {
            // short-jl form: only the one preceded by mov eax,[rbp+rax+0x8c88]
            static const uint16_t full[] = {0x8b, 0x84, 0x05, 0x88, 0x8c, 0x00, 0x00, 0x41, 0x39, 0x40, 0x04, 0x7c};
            h = find_pattern(s.base, s.len, full, 12, &n);
            if (h) h += 7;
        }
        if (n != 1 || !h) { shim_log("core: %s site %zu found %d times, skipped", sites[i].what, i, n); continue; }
        patch_bytes(h + sites[i].at, sites[i].rep, sites[i].rlen);
        shim_log("core: feature arch minimum lifted (%s) at +0x%lx", sites[i].what, (unsigned long)(h - s.base));
    }
}

void shim_after_dlopen(const char *file);
void shim_after_dlopen_h(void *h, const char *file);
void *real_dlsym_pub(void *h, const char *name);
void *vk_wrap_dlsym(const char *name, void *p);

void *dlopen(const char *file, int mode) {
    static void *(*real)(const char *, int);
    if (!real) real = dlsym(RTLD_NEXT, "dlopen");
    void *h = real(file, mode);
    if (h && file) { shim_after_dlopen(file); shim_after_dlopen_h(h, file); }
    return h;
}

// libnvidia-ngx-dlssg.so (310.9.16) NVSDK_NGX_VULKAN_GetFeatureRequirements:
//   mov dword [rsp+8], 0xb (FrameGeneration) ; mov dword [rsp+0xc], 0x190 (minimum arch: Ada)
// The minimum becomes 0x160 (Turing).
static void patch_dlssg(void *handle, const char *fdpath) {
    void *fn = real_dlsym_pub(handle, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    if (!fn) { shim_log("dlssg %s: GetFeatureRequirements not exported", fdpath); return; }
    static const uint16_t pat[] = {0xc7, 0x44, 0x24, 0x08, 0x0b, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x0c, 0x90, 0x01, 0x00, 0x00};
    int n;
    uint8_t *hit = find_pattern(fn, 64, pat, sizeof pat / sizeof pat[0], &n);
    if (n != 1) { shim_log("dlssg: requirement pattern not found"); return; }
    static const uint8_t turing[] = {0x60, 0x01};
    if (patch_bytes(hit + 12, turing, 2)) { shim_log("dlssg: mprotect failed"); return; }
    shim_log("dlssg: minimum arch 0x190 -> 0x160 in GetFeatureRequirements");

    // SetGPUArch(this, arch): test r12d,r12d ; je <query> ; mov [rbp+0x390],r12d  (m_gpuArch)
    // becomes mov dword [rbp+0x390], <spoof> ; nop2 - the snippet then picks its Ada (sm_89) kernel set, which the
    // CU-module hook swaps for Ampere builds.
    struct seg t;
    if (!find_text("/proc/self/fd/", &t)) return;
    static const uint16_t pat2[] = {0x45, 0x85, 0xe4, 0x74, 0x100, 0x44, 0x89, 0xa5, 0x90, 0x03, 0x00, 0x00};
    Dl_info di;
    if (!dladdr(fn, &di)) return;
    // search the whole executable segment of this snippet (the one fn lives in)
    uint8_t *base = di.dli_fbase;
    size_t span = 0x90000;
    hit = find_pattern(base, span, pat2, sizeof pat2 / sizeof pat2[0], &n);
    if (n != 1) { shim_log("dlssg: SetGPUArch pattern found %d times", n); return; }
    unsigned arch = 0x190;
    const char *e = getenv("DLSSG_SM86_SPOOF_ARCH");
    if (e) arch = (unsigned)strtoul(e, NULL, 0);
    uint8_t mov[12] = {0xc7, 0x85, 0x90, 0x03, 0x00, 0x00, 0, 0, 0, 0, 0x66, 0x90};
    memcpy(mov + 6, &arch, 4);
    if (patch_bytes(hit, mov, sizeof mov)) { shim_log("dlssg: mprotect failed"); return; }
    shim_log("dlssg: m_gpuArch spoofed to 0x%x", arch);
}

void shim_after_dlopen_h(void *h, const char *file) {
    if (strncmp(file, "/proc/self/fd/", 14)) return;
    char link[512] = "";
    ssize_t n = readlink(file, link, sizeof link - 1);
    if (n > 0) link[n] = 0;
    if (strstr(link, "libnvidia-ngx-dlssg.so")) patch_dlssg(h, file);
}

void shim_after_dlopen(const char *file) {
    if (strstr(file, "libnvidia-ngx.so")) patch_core();
    if (getenv("DLSSG_SM86_TRACE")) {
        char link[512] = "";
        if (!strncmp(file, "/proc/self/fd/", 14)) {
            ssize_t n = readlink(file, link, sizeof link - 1);
            if (n > 0) link[n] = 0;
        }
        shim_log("dlopen %s %s", file, link);
    }
}

typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn real_dlsym(void) {
    static dlsym_fn r;
    if (!r) r = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!r) r = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    return r;
}

void *real_dlsym_pub(void *h, const char *name) { return real_dlsym()(h, name); }

// NVSDK_NGX_Parameter (nvsdk_ngx_defs.h): Set(ull,float,double,uint,int,d3d11,d3d12,void*), Get(... same), Reset.
typedef struct ngx_param { void **vt; } ngx_param;
typedef void (*set_ui_fn)(ngx_param *, const char *, unsigned int);
typedef int (*get_ui_fn)(ngx_param *, const char *, unsigned int *);
#define NGX_SET_UI 3
#define NGX_GET_UI 11

static void fix_caps(ngx_param *p) {
    if (!p) return;
    unsigned int avail = 0, init = 0;
    ((get_ui_fn)p->vt[NGX_GET_UI])(p, "FrameGeneration.Available", &avail);
    ((get_ui_fn)p->vt[NGX_GET_UI])(p, "FrameGeneration.FeatureInitResult", &init);
    shim_log("caps: FrameGeneration.Available=%u FeatureInitResult=0x%x", avail, init);
    if (!avail && init == 1) {
        ((set_ui_fn)p->vt[NGX_SET_UI])(p, "FrameGeneration.Available", 1);
        shim_log("caps: FrameGeneration.Available forced to 1");
    }
}

typedef int (*getcaps_fn)(ngx_param **);
static getcaps_fn real_getcaps;
static int my_getcaps(ngx_param **out) {
    int r = real_getcaps(out);
    if (r == 1 && out) fix_caps(*out);
    return r;
}

void *dlsym(void *h, const char *name) {
    void *p = real_dlsym()(h, name);
    if (p && !strcmp(name, "NVSDK_NGX_VULKAN_GetCapabilityParameters")) {
        real_getcaps = p;
        shim_log("hooked %s", name);
        return (void *)my_getcaps;
    }
    if (p && !strncmp(name, "vkGet", 5)) p = vk_wrap_dlsym(name, p);
    if (getenv("DLSSG_SM86_TRACE") && (!strncmp(name, "nvml", 4) || !strncmp(name, "vk", 2) || !strncmp(name, "Nv", 2) || !strncmp(name, "nvapi", 5))) {
        Dl_info di = {0};
        dladdr(__builtin_return_address(0), &di);
        shim_log("dlsym %s from %s", name, di.dli_fname ? di.dli_fname : "?");
    }
    return p;
}

__attribute__((constructor)) static void shim_init(void) {
    const char *v = getenv("DLSSG_SM86_QUIET");
    g_verbose = !(v && *v == '1');
    shim_log("loaded");
    patch_core(); // in case the core is already mapped
}
