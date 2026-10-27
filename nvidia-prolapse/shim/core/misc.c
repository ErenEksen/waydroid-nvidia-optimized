/* sysconf / nl_langinfo id translation, mallinfo, glibc identity, gettext stubs. */
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvglibc.h"
#include "constants.h"

static const struct nvg_cmap *cmap_find(const struct nvg_cmap *t, size_t n, int glibc) {
    for (size_t i = 0; i < n; i++) {
        if (t[i].glibc == glibc) {
            return &t[i];
        }
    }
    return NULL;
}

NVG_EXPORT long nvg_sysconf(int name) {
    const struct nvg_cmap *e = cmap_find(nvg_sc_map, NVG_SC_MAP_N, name);
    if (e == NULL) {
        nvgi_die("sysconf(%d): glibc _SC_ id with no bionic equivalent (add it to bionic's "
                 "table or translate it in core/misc.c)", name);
    }
    return sysconf(e->bionic);
}

NVG_EXPORT char *nvg___nl_langinfo_l(int item, locale_t loc) {
    const struct nvg_cmap *e = cmap_find(nvg_nl_item_map, NVG_NL_ITEM_MAP_N, item);
    if (e == NULL) {
        nvgi_die("nl_langinfo_l(%#x): glibc nl_item (category %d, index %d) with no bionic "
                 "equivalent", item, item >> 16, item & 0xffff);
    }
    return nl_langinfo_l(e->bionic, loc);
}

#if defined(__LP64__) /* i386: bionic's 10 x size_t is glibc's 10 x int (identical, dispositions-x86.tsv) */
NVG_EXPORT struct nvg_glibc_mallinfo nvg_mallinfo(void) {
    struct mallinfo m = mallinfo();
    /* glibc's fields are int; it truncates the same way. */
    struct nvg_glibc_mallinfo r = {
        (int)m.arena, (int)m.ordblks, (int)m.smblks, (int)m.hblks, (int)m.hblkhd,
        (int)m.usmblks, (int)m.fsmblks, (int)m.uordblks, (int)m.fordblks, (int)m.keepcost,
    };
    return r;
}
#endif

/* gnu_get_libc_version(): eglcore parses "major.minor" and keeps a "glibc older than 2.34"
 * flag (abi-contract.md §6). The value must be the host glibc the blobs were validated on;
 * there is no built-in default. Sources, in order:
 *   1. $NVGLIBC_GLIBC_VERSION (explicit, for probes and tests);
 *   2. the file "host-glibc-version" next to libnvglibc.so, written by whatever installs the
 *      shims from the host (an app process has no environment we control, and the shims live
 *      in a directory the installer owns anyway).
 * Either missing or malformed aborts with the remedy. */
NVG_EXPORT const char *nvg_gnu_get_libc_version(void);

static const char *read_sidecar_version(char *buf, size_t n, char *path, size_t pathn) {
    Dl_info di;
    path[0] = '\0';
    if (dladdr((const void *)&nvg_gnu_get_libc_version, &di) == 0 || di.dli_fname == NULL) {
        return NULL;
    }
    const char *slash = strrchr(di.dli_fname, '/');
    size_t dirlen = slash ? (size_t)(slash - di.dli_fname) + 1 : 0;
    if (dirlen + sizeof("host-glibc-version") > pathn) {
        return NULL;
    }
    memcpy(path, di.dli_fname, dirlen);
    memcpy(path + dirlen, "host-glibc-version", sizeof("host-glibc-version"));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) {
        return NULL;
    }
    buf[r] = '\0';
    char *nl = strpbrk(buf, "\r\n");
    if (nl) {
        *nl = '\0';
    }
    return buf;
}

NVG_EXPORT const char *nvg_gnu_get_libc_version(void) {
    static const char *v;
    static char file_buf[32];
    const char *cur = __atomic_load_n(&v, __ATOMIC_ACQUIRE);
    if (cur != NULL) {
        return cur;
    }
    char path[512];
    const char *src = "NVGLIBC_GLIBC_VERSION";
    const char *e = getenv("NVGLIBC_GLIBC_VERSION");
    if (e == NULL || *e == '\0') {
        char tmp[sizeof(file_buf)];
        e = read_sidecar_version(tmp, sizeof(tmp), path, sizeof(path));
        if (e == NULL) {
            nvgi_die("gnu_get_libc_version: NVGLIBC_GLIBC_VERSION is not set and %s is missing or "
                     "empty; the installer must write the host glibc version (e.g. 2.44, from "
                     "`getconf GNU_LIBC_VERSION`) to that file next to libnvglibc.so",
                     path[0] ? path : "<dir of libnvglibc.so>/host-glibc-version");
        }
        src = path;
        /* Publish through a static buffer: the returned pointer must stay valid forever. */
        memcpy(file_buf, tmp, sizeof(file_buf));
        e = file_buf;
    }
    const char *p = e;
    if (!isdigit((unsigned char)*p)) {
        goto bad;
    }
    while (isdigit((unsigned char)*p)) p++;
    if (*p++ != '.' || !isdigit((unsigned char)*p)) {
        goto bad;
    }
    while (isdigit((unsigned char)*p)) p++;
    if (*p != '\0' && *p != '.') {
        goto bad;
    }
    nvgi_trace("gnu_get_libc_version: \"%s\" from %s", e, src);
    __atomic_store_n(&v, e, __ATOMIC_RELEASE);
    return e;
bad:
    nvgi_die("gnu_get_libc_version: \"%s\" (from %s) is not <major>.<minor>", e, src);
}

/* Statically linked libstdc++ locale glue; no message catalogs exist here. */
NVG_EXPORT char *nvg_dgettext(const char *domain, const char *msgid) {
    (void)domain;
    return (char *)msgid;
}

/* gettext(msgid) is dgettext(NULL, msgid): with no catalogs it returns msgid (glibc intl/gettext.c). */
NVG_EXPORT char *nvg_gettext(const char *msgid) {
    return (char *)msgid;
}

NVG_EXPORT char *nvg_bind_textdomain_codeset(const char *domain, const char *codeset) {
    (void)domain;
    (void)codeset;
    return NULL;
}

/* glibc newlocale() (locale/newlocale.c) special-cases category_mask == 1 << LC_ALL as
 * "every category"; libstdc++'s locale::facet::_S_create_c_locale calls exactly
 * __newlocale(1 << LC_ALL, name, base). Bionic rejects any bit outside LC_ALL_MASK with
 * EINVAL, which surfaces as an uncaught std::runtime_error inside the blob. The individual
 * LC_*_MASK bits are identical (gen-constants.py), so only this value needs mapping. */
NVG_EXPORT locale_t nvg___newlocale(int category_mask, const char *name, locale_t base) {
    if (category_mask == (1 << LC_ALL)) {
        category_mask = LC_ALL_MASK;
    }
    locale_t l = newlocale(category_mask, name, base);
    if (l == NULL) {
        nvgi_trace("__newlocale(%#x, \"%s\") failed: bionic newlocale returned NULL (errno %d)",
                   category_mask, name ? name : "(null)", errno);
    }
    return l;
}
