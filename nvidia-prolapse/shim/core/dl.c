/* dlopen / dlvsym / dladdr1 translations. */
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvglibc.h"

#if defined(__LP64__)
/* Flag bits glibc's dlopen accepts on x86_64 (RTLD_* identical to bionic per
 * gen-constants.py, plus glibc's RTLD_DEEPBIND). */
#define GLIBC_DLOPEN_FLAGS \
    (RTLD_LAZY | RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL | RTLD_NODELETE | NVG_GLIBC_RTLD_DEEPBIND)
#else
/* i386: glibc's own values (bionic LP32 RTLD_NOW/RTLD_GLOBAL differ). */
#define GLIBC_DLOPEN_FLAGS                                                                     \
    (NVG_GLIBC_RTLD_LAZY | NVG_GLIBC_RTLD_NOW | NVG_GLIBC_RTLD_NOLOAD | NVG_GLIBC_RTLD_GLOBAL | \
     NVG_GLIBC_RTLD_NODELETE | NVG_GLIBC_RTLD_DEEPBIND)
#endif

#if !defined(__LP64__)
/* bionic LP32 dlopen flags and pseudo-handles differ from glibc's (glibc_abi.h): map them.
 * RTLD_LOCAL is 0 in both; RTLD_LAZY/RTLD_NOLOAD/RTLD_NODELETE have the same values. */
static int map_dlopen_flags(int g) {
    int b = 0;
    if (g & NVG_GLIBC_RTLD_LAZY) b |= RTLD_LAZY;
    if (g & NVG_GLIBC_RTLD_NOW) b |= RTLD_NOW;
    if (g & NVG_GLIBC_RTLD_NOLOAD) b |= RTLD_NOLOAD;
    if (g & NVG_GLIBC_RTLD_GLOBAL) b |= RTLD_GLOBAL;
    if (g & NVG_GLIBC_RTLD_NODELETE) b |= RTLD_NODELETE;
    return b;
}
#define NVG_GLIBC_HANDLE_DEFAULT ((void *)0)
#define NVG_GLIBC_HANDLE_NEXT ((void *)-1L)

/* glibc handle -> bionic handle (RTLD_NEXT cannot be forwarded: bionic would search relative
 * to libnvglibc, not the caller). */
static void *map_handle(void *h, const char *fn, const char *name) {
    if (h == NVG_GLIBC_HANDLE_DEFAULT) {
        return RTLD_DEFAULT;
    }
    if (h == NVG_GLIBC_HANDLE_NEXT) {
        nvgi_die("%s(RTLD_NEXT, \"%s\"): RTLD_NEXT cannot be forwarded (bionic would search "
                 "relative to libnvglibc, not the caller)", fn, name);
    }
    return h;
}

NVG_EXPORT void *nvg_dlsym(void *handle, const char *name) {
    return dlsym(map_handle(handle, "dlsym", name), name);
}
#endif

/* Note: bionic attributes this call to the core (caller address) when it picks
 * the namespace; in P1 the core, fronts and blobs share one namespace. */
NVG_EXPORT void *nvg_dlopen(const char *name, int flags) {
    if (flags & ~GLIBC_DLOPEN_FLAGS) {
        nvgi_die("dlopen(\"%s\", %#x): flag bits %#x are not glibc dlopen flags",
                 name ? name : "(null)", flags, flags & ~GLIBC_DLOPEN_FLAGS);
    }
    if (flags & NVG_GLIBC_RTLD_DEEPBIND) {
        nvgi_trace("dlopen(\"%s\"): RTLD_DEEPBIND stripped (bionic has no such mode)",
                   name ? name : "(null)");
        flags &= ~NVG_GLIBC_RTLD_DEEPBIND;
    }
#if !defined(__LP64__)
    flags = map_dlopen_flags(flags);
#endif
    /* Every library the driver opens stays loaded for the life of the process (bionic RTLD_NODELETE). libEGL_nvidia
     * dlopens itself and dlcloses that handle from the exit-time destructor it registers with __cxa_atexit (615.78.08:
     * ELF 0x316d0 registers 0x9f950, which reaches its dlclose wrapper 0xb8140); when no other reference is left, bionic
     * unmaps the library under that running destructor and the process dies at exit with SIGSEGV (measured in the
     * guest; a glibc process exits cleanly). While the process runs, the driver never drops its own handles, so this
     * changes nothing before exit. */
    flags |= RTLD_NODELETE;
    void *h = dlopen(name, flags);
    if (h == NULL) {
        /* dlerror() is not called here: it would consume the message the
         * caller is about to read. */
        nvgi_trace("dlopen(\"%s\", %#x) failed (caller sees NULL)", name ? name : "(null)", flags);
    } else {
        nvgi_trace("dlopen(\"%s\", %#x) = %p", name ? name : "(null)", flags, h);
    }
    return h;
}

/* A GLIBC_* version names a glibc-ABI definition. The only ones in this
 * process are the fronts' exports, so resolve the name there (whatever the
 * handle: bionic's same-named definitions have bionic's ABI, e.g. sigaction).
 * Other versions go to bionic's dlvsym. */
NVG_EXPORT void *nvg_dlvsym(void *handle, const char *name, const char *version) {
    if (version != NULL && strncmp(version, "GLIBC_", 6) == 0) {
        void *p = nvgi_front_lookup(name);
        nvgi_trace("dlvsym(%p, \"%s\", \"%s\") = %p%s", handle, name, version, p,
                   p ? "" : " (not exported by any front)");
        return p;
    }
#if defined(__LP64__)
    if (handle == RTLD_NEXT) {
        nvgi_die("dlvsym(RTLD_NEXT, \"%s\", \"%s\"): RTLD_NEXT cannot be forwarded (bionic would "
                 "search relative to libnvglibc, not the caller)", name, version ? version : "");
    }
    return dlvsym(handle, name, version);
#else
    return dlvsym(map_handle(handle, "dlvsym", name), name, version);
#endif
}

/* ---- dladdr1(RTLD_DL_LINKMAP): synthetic glibc link_map per module ---- */

struct lm_node {
    struct nvg_glibc_link_map lm;
    void *fbase;
    struct lm_node *next;
};

static struct lm_node *lm_cache;
static pthread_mutex_t lm_lock = PTHREAD_MUTEX_INITIALIZER;

struct find_ctx {
    uintptr_t addr;
    int found;
    uintptr_t l_addr;
    const char *name;
    void *l_ld;
};

static int find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct find_ctx *c = data;
    int hit = 0;
    void *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && c->addr >= lo && c->addr < lo + ph->p_memsz) {
            hit = 1;
        } else if (ph->p_type == PT_DYNAMIC) {
            dyn = (void *)lo;
        }
    }
    if (!hit) {
        return 0;
    }
    c->found = 1;
    c->l_addr = info->dlpi_addr;
    c->name = info->dlpi_name;
    c->l_ld = dyn;
    return 1;
}

static struct nvg_glibc_link_map *link_map_for(const void *addr, const Dl_info *info) {
    pthread_mutex_lock(&lm_lock);
    struct lm_node *n;
    for (n = lm_cache; n != NULL; n = n->next) {
        if (n->fbase == info->dli_fbase && info->dli_fname != NULL &&
            strcmp(n->lm.l_name, info->dli_fname) == 0) {
            break;
        }
    }
    if (n == NULL) {
        struct find_ctx c = { .addr = (uintptr_t)addr };
        dl_iterate_phdr(find_cb, &c);
        if (!c.found) {
            pthread_mutex_unlock(&lm_lock);
            nvgi_die("dladdr1(%p): dladdr found \"%s\" but dl_iterate_phdr has no module "
                     "containing the address", addr, info->dli_fname ? info->dli_fname : "?");
        }
        n = calloc(1, sizeof(*n));
        char *name = strdup(info->dli_fname ? info->dli_fname : (c.name ? c.name : ""));
        if (n == NULL || name == NULL) {
            pthread_mutex_unlock(&lm_lock);
            nvgi_die("dladdr1(%p): out of memory", addr);
        }
        n->lm.l_addr = c.l_addr;
        n->lm.l_name = name;
        n->lm.l_ld = c.l_ld;
        n->lm.l_next = NULL; /* no chain: only l_addr/l_name/l_ld are provided */
        n->lm.l_prev = NULL;
        n->fbase = info->dli_fbase;
        n->next = lm_cache;
        lm_cache = n;
    }
    pthread_mutex_unlock(&lm_lock);
    return &n->lm;
}

NVG_EXPORT int nvg_dladdr1(const void *addr, Dl_info *info, void **extra, int flags) {
    int r = dladdr(addr, info);
    if (r == 0) {
        return 0;
    }
    switch (flags) {
    case 0:
        return r;
    case NVG_GLIBC_RTLD_DL_LINKMAP:
        *extra = link_map_for(addr, info);
        return r;
    case NVG_GLIBC_RTLD_DL_SYMENT:
        nvgi_die("dladdr1(%p, RTLD_DL_SYMENT): not implemented (no ElfW(Sym) is available "
                 "from bionic)", addr);
    default:
        nvgi_die("dladdr1(%p, flags %d): unknown flags", addr, flags);
    }
}
