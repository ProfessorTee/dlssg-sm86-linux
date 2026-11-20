// Vulkan side: wraps vkGetInstanceProcAddr / vkGetDeviceProcAddr handed to the game so the CU-module entry points of
// VK_NVX_binary_import (what the DLSS-G snippet loads its kernels with) pass through here.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

void shim_log(const char *fmt, ...);
void sha256(const void *data, size_t len, uint8_t out[32]);

static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateCuModuleNVX real_create_module;
static PFN_vkCreateCuFunctionNVX real_create_function;

// /proc/self/fd/N of a snippet -> the memfd's name (the snippet file).
static const char *caller_name(const char *f) {
    static __thread char buf[256];
    if (!f) return "?";
    if (strncmp(f, "/proc/self/fd/", 14)) return f;
    ssize_t n = readlink(f, buf, sizeof buf - 1);
    if (n <= 0) return f;
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    return slash ? slash + 1 : buf;
}

static const char *blob_kind(const uint8_t *p, size_t n, char *buf, size_t bn) {
    if (n >= 0x34 && !memcmp(p, "\x7f" "ELF", 4)) {
        uint32_t flags;
        memcpy(&flags, p + 0x30, 4);
        snprintf(buf, bn, "cubin e_flags=0x%x", flags);
    } else if (n >= 4 && !memcmp(p, "\x50\xed\x55\xba", 4)) {
        snprintf(buf, bn, "fatbin");
    } else if (n >= 8 && memmem(p, n < 512 ? n : 512, ".target", 7)) {
        const char *t = memmem(p, n < 4096 ? n : 4096, ".target", 7);
        snprintf(buf, bn, "ptx %.16s", t);
    } else {
        snprintf(buf, bn, "unknown %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
    }
    return buf;
}

// Kernel pack: <dir>/<sha256 of the snippet's blob>.cubin (tools/make_pack.py). Dir = $DLSSG_SM86_PACK, else every
// subdirectory of pack/ next to this library.
static char g_pack_root[1024];

static void pack_root_init(void) {
    if (g_pack_root[0]) return;
    const char *e = getenv("DLSSG_SM86_PACK");
    if (e && *e) { snprintf(g_pack_root, sizeof g_pack_root, "%s", e); return; }
    Dl_info di;
    if (dladdr((void *)pack_root_init, &di) && di.dli_fname) {
        snprintf(g_pack_root, sizeof g_pack_root, "%s", di.dli_fname);
        char *slash = strrchr(g_pack_root, '/');
        if (slash) snprintf(slash, sizeof g_pack_root - (slash - g_pack_root), "/pack");
    }
}

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

// Finds <hex>.cubin in the pack root itself or one level below it.
static void *pack_lookup(const char *hex, size_t *len, char *found, size_t fn) {
    pack_root_init();
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.cubin", g_pack_root, hex);
    void *b = read_file(path, len);
    if (b) { snprintf(found, fn, "%s", path); return b; }
    DIR *d = opendir(g_pack_root);
    if (!d) return NULL;
    struct dirent *de;
    while (!b && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s/%s.cubin", g_pack_root, de->d_name, hex);
        b = read_file(path, len);
        if (b) snprintf(found, fn, "%s", path);
    }
    closedir(d);
    return b;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_create_module(VkDevice dev, const VkCuModuleCreateInfoNVX *ci,
                                                       const VkAllocationCallbacks *a, VkCuModuleNVX *out) {
    uint8_t h[32];
    char kind[64], hex[65], found[1400] = "";
    sha256(ci->pData, ci->dataSize, h);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    size_t rlen = 0;
    void *repl = pack_lookup(hex, &rlen, found, sizeof found);
    VkResult r;
    if (repl) {
        VkCuModuleCreateInfoNVX c2 = *ci;
        c2.pData = repl;
        c2.dataSize = rlen;
        r = real_create_module(dev, &c2, a, out);
        free(repl); // the driver copies the image at creation
        shim_log("cu module %.16s swapped for %s (%zu B) -> %d", hex, strrchr(found, '/') + 1, rlen, r);
        if (r == VK_SUCCESS) return r;
    }
    r = real_create_module(dev, ci, a, out);
    Dl_info di = {0};
    dladdr(__builtin_return_address(0), &di);
    shim_log("cu module [%s] %zu B %s sha256 %02x%02x%02x%02x%02x%02x%02x%02x -> %d handle %p", caller_name(di.dli_fname), ci->dataSize,
             blob_kind(ci->pData, ci->dataSize, kind, sizeof kind), h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], r,
             (void *)*out);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_create_function(VkDevice dev, const VkCuFunctionCreateInfoNVX *ci,
                                                         const VkAllocationCallbacks *a, VkCuFunctionNVX *out) {
    VkResult r = real_create_function(dev, ci, a, out);
    Dl_info di = {0};
    dladdr(__builtin_return_address(0), &di);
    shim_log("cu function [%s] %s in module %p -> %d", caller_name(di.dli_fname), ci->pName, (void *)ci->module, r);
    return r;
}

static PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction real) {
    if (!real) return real;
    if (!strcmp(name, "vkCreateCuModuleNVX")) {
        real_create_module = (PFN_vkCreateCuModuleNVX)real;
        return (PFN_vkVoidFunction)my_create_module;
    }
    if (!strcmp(name, "vkCreateCuFunctionNVX")) {
        real_create_function = (PFN_vkCreateCuFunctionNVX)real;
        return (PFN_vkVoidFunction)my_create_function;
    }
    return real;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gdpa(VkDevice dev, const char *name) {
    return wrap(name, real_gdpa(dev, name));
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance inst, const char *name) {
    PFN_vkVoidFunction f = real_gipa(inst, name);
    if (f && !strcmp(name, "vkGetDeviceProcAddr")) {
        real_gdpa = (PFN_vkGetDeviceProcAddr)f;
        return (PFN_vkVoidFunction)my_gdpa;
    }
    return wrap(name, f);
}

// Called from the dlsym hook: returns a replacement for the loader's entry points, or the original pointer.
void *vk_wrap_dlsym(const char *name, void *p) {
    if (!p) return p;
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        if (!real_gipa) real_gipa = (PFN_vkGetInstanceProcAddr)p;
        return (void *)my_gipa;
    }
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        if (!real_gdpa) real_gdpa = (PFN_vkGetDeviceProcAddr)p;
        return (void *)my_gdpa;
    }
    return p;
}
