// In-memory patches of the NGX core and the DLSS-G snippet.
//
// Sites are found from anchors that survive rebuilds: the log string an error path prints (-> the rip-relative lea that
// loads it -> the one conditional jump into that error block), an exported function, or a parameter name next to the
// arch switch it guards. A site is patched only when exactly one candidate is found; otherwise it is logged and left
// alone, so a changed driver or runtime degrades to "not unlocked" instead of a crash.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void shim_log(const char *fmt, ...);

#define W 0x100 // wildcard byte in a pattern

// ---------------------------------------------------------------- image access

struct image {
    uintptr_t load;
    uint8_t *text; size_t text_len;               // executable PT_LOAD
    struct { uint8_t *p; size_t n; } seg[8]; int nseg; // every PT_LOAD
    char path[512];
};

struct find_ctx { const char *name; uintptr_t load; struct image *img; };

static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz;
    struct find_ctx *c = data;
    if (c->name ? (!info->dlpi_name || !strstr(info->dlpi_name, c->name)) : info->dlpi_addr != c->load) return 0;
    struct image *im = c->img;
    memset(im, 0, sizeof *im);
    im->load = info->dlpi_addr;
    snprintf(im->path, sizeof im->path, "%s", info->dlpi_name ? info->dlpi_name : "");
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD) continue;
        uint8_t *a = (uint8_t *)(info->dlpi_addr + p->p_vaddr);
        if (im->nseg < 8) { im->seg[im->nseg].p = a; im->seg[im->nseg].n = p->p_memsz; im->nseg++; }
        if ((p->p_flags & PF_X) && !im->text) { im->text = a; im->text_len = p->p_memsz; }
    }
    return im->text != NULL;
}

static int image_by_name(const char *name, struct image *im) {
    struct find_ctx c = {name, 0, im};
    return dl_iterate_phdr(phdr_cb, &c) == 1;
}

static int image_by_load(uintptr_t load, struct image *im) {
    struct find_ctx c = {NULL, load, im};
    return dl_iterate_phdr(phdr_cb, &c) == 1;
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

static const uint8_t NOP2[] = {0x66, 0x90};
static const uint8_t NOP6[] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};

// ---------------------------------------------------------------- search

// Pattern (W = any byte) in [p, p+n); first hit, *count = number of hits.
static uint8_t *find_pattern(uint8_t *p, size_t n, const uint16_t *pat, size_t plen, int *count) {
    uint8_t *hit = NULL;
    *count = 0;
    for (size_t i = 0; i + plen <= n; i++) {
        size_t j = 0;
        while (j < plen && (pat[j] == W || p[i + j] == pat[j])) j++;
        if (j == plen) { if (!hit) hit = p + i; (*count)++; }
    }
    return hit;
}
#define PAT(p, n, pat, cnt) find_pattern((p), (n), (pat), sizeof(pat) / sizeof((pat)[0]), (cnt))

// Clamp a window [a, b) to the text segment.
static void window(const struct image *im, uint8_t *a, uint8_t *b, uint8_t **lo, size_t *n) {
    uint8_t *t0 = im->text, *t1 = im->text + im->text_len;
    if (a < t0) a = t0;
    if (b > t1) b = t1;
    *lo = a;
    *n = b > a ? (size_t)(b - a) : 0;
}

// Address of a NUL-terminated string in any segment of the image.
static uint8_t *find_string(const struct image *im, const char *s) {
    size_t n = strlen(s) + 1;
    for (int i = 0; i < im->nseg; i++) {
        uint8_t *h = memmem(im->seg[i].p, im->seg[i].n, s, n);
        if (h && (h == im->seg[i].p || h[-1] == 0)) return h;
    }
    return NULL;
}

// lea r64, [rip+disp32] (REX.W 8d /r, mod=00 rm=101) loading target. Fills out[], returns the count.
static int lea_xrefs(const struct image *im, const uint8_t *target, uint8_t **out, int max) {
    int k = 0;
    for (size_t i = 0; i + 7 <= im->text_len; i++) {
        uint8_t *q = im->text + i;
        if ((q[0] & 0xfb) != 0x48 || q[1] != 0x8d || (q[2] & 0xc7) != 0x05) continue;
        int32_t d;
        memcpy(&d, q + 3, 4);
        if (q + 7 + d == target) { if (k < max) out[k] = q; k++; }
    }
    return k;
}

// The single jcc (rel8 or rel32) in [lo, lo+n) whose target lies in [t0, t1]. *len = its length.
static uint8_t *jcc_into(uint8_t *lo, size_t n, const uint8_t *t0, const uint8_t *t1, int *len, int *count) {
    uint8_t *hit = NULL;
    *count = 0;
    for (size_t i = 0; i + 2 <= n; i++) {
        uint8_t *q = lo + i, *to;
        int l;
        if ((q[0] & 0xf0) == 0x70) { l = 2; to = q + 2 + (int8_t)q[1]; }
        else if (i + 6 <= n && q[0] == 0x0f && (q[1] & 0xf0) == 0x80) { int32_t d; memcpy(&d, q + 2, 4); l = 6; to = q + 6 + d; }
        else continue;
        if (to >= t0 && to <= t1) { if (!hit) { hit = q; *len = l; } (*count)++; }
    }
    return hit;
}

// Removes the conditional jump that leads into the error block printing `msg`, for every lea of `msg`.
static int unjump_error(const struct image *im, const char *what, const char *msg, size_t before) {
    uint8_t *s = find_string(im, msg), *leas[8];
    if (!s) { shim_log("%s: string \"%s\" not found", what, msg); return 0; }
    int nl = lea_xrefs(im, s, leas, 8), done = 0;
    for (int i = 0; i < nl && i < 8; i++) {
        uint8_t *lo;
        size_t n;
        int len = 0, cnt;
        window(im, leas[i] - before, leas[i], &lo, &n);
        // the error block starts a few instructions before its lea (xor eax,eax / mov ... / lea)
        uint8_t *j = jcc_into(lo, n, leas[i] - 24, leas[i], &len, &cnt);
        if (cnt != 1) { shim_log("%s: %d jumps into the error block at +0x%lx, skipped", what, cnt, (unsigned long)(leas[i] - im->text)); continue; }
        patch_bytes(j, len == 2 ? NOP2 : NOP6, len);
        shim_log("%s: lifted at +0x%lx", what, (unsigned long)(j - im->text));
        done++;
    }
    return done;
}

// ---------------------------------------------------------------- NGX core (libnvidia-ngx.so)

static int g_core_done;

void patch_core(void) {
    struct image im;
    if (g_core_done || !image_by_name("libnvidia-ngx.so", &im)) return;
    g_core_done = 1;
    shim_log("core %s", im.path);

    // NGXValidateSnippetMetaData: "snippet is using newer GPU arch %X > %X" (cmp min,gpu ; jg <it>)
    unjump_error(&im, "core: snippet arch gate", "error: snippet is using newer GPU arch %X > %X", 0x1000);
    // NVSDK_NGX_CreateFeature_Validate: per-feature minimum arch (cmp [dev+4],min ; jl <it>), two copies
    unjump_error(&im, "core: CreateFeature arch minimum", "error: feature is not supported on this device", 0x400);

    // Capability pass ("<Feature>.Available"): mov e?x,[r12+min] ; cmp [rax+4],e?x ; jge <available>  ->  jmp.
    // No string of its own; the compare has the same shape as the one in Validate.
    // ponytail: these lift the arch minimum for every feature, not only dlssg; a feature that really needs newer
    // hardware then fails inside its own snippet instead of here.
    static const uint16_t av1[] = {0x41, 0x8b, 0x4c, 0x24, W, 0x39, 0x48, 0x04, 0x0f, 0x8d};
    static const uint16_t av2[] = {0x41, 0x8b, 0x74, 0x24, W, 0x39, 0x70, 0x04, 0x0f, 0x8d};
    const uint16_t *av[] = {av1, av2};
    for (int i = 0; i < 2; i++) {
        int n;
        uint8_t *h = find_pattern(im.text, im.text_len, av[i], 10, &n);
        if (n != 1) { shim_log("core: Available compare %d found %d times, skipped", i, n); continue; }
        static const uint8_t jmp[] = {0x90, 0xe9}; // nop ; jmp rel32 (same rel32, same end)
        patch_bytes(h + 8, jmp, 2);
        shim_log("core: Available arch minimum lifted at +0x%lx", (unsigned long)(h - im.text));
    }
}

// ---------------------------------------------------------------- DLSS-G snippet (libnvidia-ngx-dlssg.so)

void *real_dlsym_pub(void *h, const char *name);

void patch_dlssg(void *handle) {
    void *req = real_dlsym_pub(handle, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    Dl_info di;
    struct image im;
    if (!req || !dladdr(req, &di) || !image_by_load((uintptr_t)di.dli_fbase, &im)) {
        shim_log("dlssg: snippet image not found");
        return;
    }
    int n;

    // GetFeatureRequirements: mov dword [rsp+a], 0xb (FrameGeneration) ; mov dword [rsp+b], 0x190 (min arch) -> 0x160
    static const uint16_t rq[] = {0xc7, 0x44, 0x24, W, 0x0b, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, W, 0x90, 0x01, 0x00, 0x00};
    uint8_t *h = PAT(req, 64, rq, &n);
    if (n == 1) {
        static const uint8_t turing[] = {0x60, 0x01};
        patch_bytes(h + 12, turing, 2);
        shim_log("dlssg: GetFeatureRequirements minimum 0x190 -> 0x160");
    } else {
        shim_log("dlssg: GetFeatureRequirements pattern found %d times, skipped", n);
    }

    // SetGPUArch: test r12d,r12d ; je <query> ; mov [rbp+field],r12d ; lea rcx,"m_gpuArch = 0x%x"
    // -> mov dword [rbp+field], <spoof> ; nop2. Ada makes the snippet pick its sm_89 kernels (swapped in vk.c).
    uint8_t *s = find_string(&im, "m_gpuArch = 0x%x"), *leas[4];
    int nl = s ? lea_xrefs(&im, s, leas, 4) : 0;
    uint8_t *lo;
    size_t wn;
    static const uint16_t ga[] = {0x45, 0x85, 0xe4, 0x74, W, 0x44, 0x89, 0xa5, W, W, W, W};
    h = NULL;
    if (nl == 1) {
        window(&im, leas[0] - 0x20, leas[0], &lo, &wn);
        h = PAT(lo, wn, ga, &n);
    }
    if (h && n == 1) {
        unsigned arch = 0x190;
        const char *e = getenv("DLSSG_SM86_SPOOF_ARCH");
        if (e) arch = (unsigned)strtoul(e, NULL, 0);
        uint8_t mov[12] = {0xc7, 0x85, 0, 0, 0, 0, 0, 0, 0, 0, 0x66, 0x90};
        memcpy(mov + 2, h + 8, 4); // the field's disp32
        memcpy(mov + 6, &arch, 4);
        patch_bytes(h, mov, sizeof mov);
        shim_log("dlssg: m_gpuArch spoofed to 0x%x", arch);
    } else {
        shim_log("dlssg: SetGPUArch not found (string xrefs %d), skipped", nl);
    }

    const char *mfg = getenv("DLSSG_SM86_MFG");
    if (mfg && *mfg == '0') return;
    int mfg_ok = 0;

    // DLSSG.MultiFrameCountMax, set right after an arch switch:
    //   cmp eax,0x170 ; je <one> ; jbe ; cmp eax,0x190 ; je ; jbe ; cmp eax,0x1a0 ; je ; cmp eax,0x1b0 ; jne <..> ; <multi>
    // je <one> for 0x170 is pointed at <multi>. With no device given it takes the highest arch of all GPUs instead:
    //   cmp r12d,0x1af ; jbe <one>  ->  nop.
    s = find_string(&im, "DLSSG.MultiFrameCountMax");
    nl = s ? lea_xrefs(&im, s, leas, 4) : 0;
    static const uint16_t sw[] = {0x3d, 0x70, 0x01, 0x00, 0x00, 0x74, W, 0x0f, 0x86, W, W, W, W, 0x3d, 0x90, 0x01, 0x00,
                                  0x00, 0x74, W, 0x76, W, 0x3d, 0xa0, 0x01, 0x00, 0x00, 0x74, W, 0x3d, 0xb0, 0x01, 0x00,
                                  0x00, 0x0f, 0x85};
    static const uint16_t all[] = {0x41, 0x81, 0xfc, 0xaf, 0x01, 0x00, 0x00, 0x0f, 0x86};
    for (int i = 0; i < nl && i < 4; i++) {
        window(&im, leas[i] - 0x200, leas[i] + 0x200, &lo, &wn);
        h = PAT(lo, wn, sw, &n);
        if (h && n == 1) {
            uint8_t *multi = h + sizeof sw / sizeof sw[0] + 4;
            long rel = multi - (h + 7);
            if (rel >= -128 && rel <= 127) {
                uint8_t r8 = (uint8_t)rel;
                patch_bytes(h + 6, &r8, 1);
                mfg_ok |= 1;
            }
        }
        h = PAT(lo, wn, all, &n);
        if (h && n == 1) { patch_bytes(h + 7, NOP6, 6); mfg_ok |= 2; }
    }

    // mfgSupported (per feature) = device arch > 0x1af: cmp eax,0x1af ; mov byte [rbx+x],0 ; mov eax,1 ; seta [rbx+y].
    // Without it Evaluate refuses a frame count > 1 (0xbad00005). No string nearby: unique-pattern heuristic.
    static const uint16_t fl[] = {0x3d, 0xaf, 0x01, 0x00, 0x00, 0xc6, 0x43, W, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00,
                                  0x0f, 0x97, 0x43, W};
    h = PAT(im.text, im.text_len, fl, &n);
    if (n == 1) {
        static const uint8_t amp = 0x6f; // 0x1af -> 0x16f
        patch_bytes(h + 1, &amp, 1);
        mfg_ok |= 4;
    }
    if (mfg_ok == 7) shim_log("dlssg: multi-frame generation allowed on this arch");
    else shim_log("dlssg: multi-frame patches incomplete (mask %d of 7): DLSS-G stays at 2x", mfg_ok);
}
