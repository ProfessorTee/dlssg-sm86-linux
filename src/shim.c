// dlssg-sm86-linux: LD_PRELOAD shim that runs NVIDIA DLSS Frame Generation (incl. multi-frame) on RTX 20/30 in native
// Linux Vulkan games. It patches the arch gates of the NGX core (libnvidia-ngx.so) and of the DLSS-G snippet in memory
// (files on disk stay untouched, signatures are checked before the patches) and swaps the snippet's sm_89 kernels for
// Ampere builds at vkCreateCuModuleNVX (vk.c).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

void patch_core(void);
void patch_dlssg(void *handle);
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

void shim_after_dlopen_h(void *h, const char *file) {
    if (strncmp(file, "/proc/self/fd/", 14)) return;
    char link[512] = "";
    ssize_t n = readlink(file, link, sizeof link - 1);
    if (n > 0) link[n] = 0;
    if (strstr(link, "libnvidia-ngx-dlssg.so")) patch_dlssg(h);
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

// Capability parameters as the game sees them (diagnostic; FrameGeneration.Available is forced only if the core
// patches above missed on a newer driver while the snippet still initialised).
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
    unsigned int mfmax = 0;
    ((get_ui_fn)p->vt[NGX_GET_UI])(p, "DLSSG.MultiFrameCountMax", &mfmax);
    shim_log("caps: FrameGeneration.Available=%u FeatureInitResult=0x%x DLSSG.MultiFrameCountMax=%u", avail, init, mfmax);
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
