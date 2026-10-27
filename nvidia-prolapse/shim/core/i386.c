/* Translators only the i386 closure needs (DESIGN.md "32-bit x86"). On i386 glibc's
 * non-LFS struct dirent / struct statfs and its struct passwd differ from bionic's LP32
 * ones (sizes and offsets measured by tools/abi-check.c with gcc -m32 and asserted in
 * glibc_abi.h); x86_64 has none of these differences, so this file is built for x86 only. */
#if !defined(__i386__)
#error "core/i386.c is the i386 translator set"
#endif

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>

#include "nvglibc.h"

/* ---- directories ----
 * glibc's i386 readdir() returns a 268-byte struct dirent with 32-bit d_ino/d_off, valid until
 * the next readdir() on the same DIR. bionic's LP32 struct dirent is the dirent64 layout. Each
 * DIR gets its own glibc entry (made on first readdir, freed by closedir), so interleaved
 * streams never share a buffer. glibc's rule for values that do not fit is EOVERFLOW
 * (sysdeps/unix/sysv/linux/readdir.c, the !__OFF_T_MATCHES_OFF64_T path). */

struct dir_buf {
    DIR *dir;
    struct nvg_glibc_dirent ent;
    struct dir_buf *next;
};

static struct dir_buf *dir_bufs;
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;

static struct nvg_glibc_dirent *dir_entry_for(DIR *d) {
    pthread_mutex_lock(&dir_lock);
    struct dir_buf *b;
    for (b = dir_bufs; b != NULL; b = b->next) {
        if (b->dir == d) {
            break;
        }
    }
    if (b == NULL) {
        b = calloc(1, sizeof(*b));
        if (b == NULL) {
            pthread_mutex_unlock(&dir_lock);
            nvgi_die("readdir: out of memory for a glibc dirent buffer");
        }
        b->dir = d;
        b->next = dir_bufs;
        dir_bufs = b;
    }
    pthread_mutex_unlock(&dir_lock);
    return &b->ent;
}

/* 0 on success, -1 with errno = EOVERFLOW */
static int to_glibc_dirent(const struct dirent *b, struct nvg_glibc_dirent *g) {
    if (b->d_ino > UINT32_MAX || b->d_off > INT32_MAX || b->d_off < INT32_MIN) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t n = strnlen(b->d_name, sizeof(g->d_name) - 1);
    g->d_ino = (uint32_t)b->d_ino;
    g->d_off = (int32_t)b->d_off;
    g->d_type = b->d_type;
    memcpy(g->d_name, b->d_name, n);
    g->d_name[n] = '\0';
    /* like glibc: the record length of the converted entry, rounded to 4 */
    g->d_reclen = (uint16_t)((offsetof(struct nvg_glibc_dirent, d_name) + n + 1 + 3) & ~3u);
    return 0;
}

NVG_EXPORT struct nvg_glibc_dirent *nvg_readdir(DIR *d) {
    struct dirent *b = readdir(d);
    if (b == NULL) {
        return NULL;
    }
    struct nvg_glibc_dirent *g = dir_entry_for(d);
    if (to_glibc_dirent(b, g) != 0) {
        nvgi_trace("readdir(%p): \"%s\" ino %llu off %lld does not fit glibc's i386 dirent: "
                   "EOVERFLOW", (void *)d, b->d_name, (unsigned long long)b->d_ino,
                   (long long)b->d_off);
        return NULL;
    }
    return g;
}

NVG_EXPORT int nvg_closedir(DIR *d) {
    pthread_mutex_lock(&dir_lock);
    for (struct dir_buf **p = &dir_bufs; *p != NULL; p = &(*p)->next) {
        if ((*p)->dir == d) {
            struct dir_buf *b = *p;
            *p = b->next;
            free(b);
            break;
        }
    }
    pthread_mutex_unlock(&dir_lock);
    return closedir(d);
}

NVG_EXPORT int nvg_alphasort(const struct nvg_glibc_dirent **a, const struct nvg_glibc_dirent **b) {
    return strcoll((*a)->d_name, (*b)->d_name);
}

typedef int (*nvg_dirent_filter)(const struct nvg_glibc_dirent *);
typedef int (*nvg_dirent_compar)(const struct nvg_glibc_dirent **, const struct nvg_glibc_dirent **);

/* glibc scandir(): entries are malloc'd one by one, the array too; the caller frees them with
 * free(), which is bionic's here as everywhere in the process. filter/compar are the blob's own
 * functions on glibc dirents (alphasort is ours, above). */
NVG_EXPORT int nvg_scandir(const char *path, struct nvg_glibc_dirent ***out, nvg_dirent_filter filter,
                           nvg_dirent_compar compar) {
    DIR *d = opendir(path);
    if (d == NULL) {
        return -1;
    }
    struct nvg_glibc_dirent **v = NULL;
    size_t n = 0, cap = 0;
    int saved = 0;
    struct dirent *b;
    errno = 0;
    while ((b = readdir(d)) != NULL) {
        struct nvg_glibc_dirent tmp;
        if (to_glibc_dirent(b, &tmp) != 0) {
            saved = EOVERFLOW;
            goto fail;
        }
        if (filter != NULL && !filter(&tmp)) {
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            struct nvg_glibc_dirent **nv = realloc(v, nc * sizeof(*v));
            if (nv == NULL) {
                saved = ENOMEM;
                goto fail;
            }
            v = nv;
            cap = nc;
        }
        v[n] = malloc(tmp.d_reclen);
        if (v[n] == NULL) {
            saved = ENOMEM;
            goto fail;
        }
        memcpy(v[n], &tmp, tmp.d_reclen);
        n++;
    }
    if (errno != 0) {
        saved = errno;
        goto fail;
    }
    closedir(d);
    if (compar != NULL && n > 1) {
        qsort(v, n, sizeof(*v), (int (*)(const void *, const void *))compar);
    }
    *out = v;
    return (int)n;
fail:
    for (size_t i = 0; i < n; i++) {
        free(v[i]);
    }
    free(v);
    closedir(d);
    errno = saved;
    return -1;
}

/* ---- statfs ----
 * glibc i386 statfs() is the kernel's 32-bit statfs, whose compat rule is: EOVERFLOW when
 * f_blocks/f_bfree/f_bavail/f_bsize/f_frsize need more than 32 bits, or f_files/f_ffree do
 * (an all-ones 64-bit value is reported as all-ones 32-bit) (fs/statfs.c put_compat_statfs). */
static int fits_files(uint64_t v) {
    return v <= UINT32_MAX || v == UINT64_MAX;
}

static int statfs_out(const struct statfs64 *bp, struct nvg_glibc_statfs *g) {
    const struct statfs64 b = *bp;
    if (((uint64_t)b.f_blocks | b.f_bfree | b.f_bavail | (uint64_t)b.f_bsize | (uint64_t)b.f_frsize) >>
            32 || !fits_files(b.f_files) || !fits_files(b.f_ffree)) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->f_type = (int32_t)b.f_type;
    g->f_bsize = (int32_t)b.f_bsize;
    g->f_blocks = (uint32_t)b.f_blocks;
    g->f_bfree = (uint32_t)b.f_bfree;
    g->f_bavail = (uint32_t)b.f_bavail;
    g->f_files = (uint32_t)b.f_files;
    g->f_ffree = (uint32_t)b.f_ffree;
    memcpy(g->f_fsid, &b.f_fsid, sizeof(g->f_fsid));
    g->f_namelen = (int32_t)b.f_namelen;
    g->f_frsize = (int32_t)b.f_frsize;
    g->f_flags = (int32_t)b.f_flags;
    return 0;
}

NVG_EXPORT int nvg_statfs(const char *path, struct nvg_glibc_statfs *g) {
    struct statfs64 b;
    if (statfs64(path, &b) != 0) {
        return -1;
    }
    return statfs_out(&b, g);
}

/* fstatfs() is the same 32-bit compat struct and rule on an open fd (fs/statfs.c compat_fstatfs). */
NVG_EXPORT int nvg_fstatfs(int fd, struct nvg_glibc_statfs *g) {
    struct statfs64 b;
    if (fstatfs64(fd, &b) != 0) {
        return -1;
    }
    return statfs_out(&b, g);
}

/* ---- passwd ----
 * bionic's LP32 struct passwd has no pw_gecos (pwd.h: "#define pw_gecos pw_passwd"), so
 * pw_dir/pw_shell sit 4 bytes earlier than in glibc's. bionic fills a local struct, the
 * strings live in the caller's buffer either way; glibc's struct gets the same pointers, and
 * pw_gecos the value bionic's own alias gives it (pw_passwd). */
static int passwd_out(int r, const struct passwd *b, struct passwd *bres, struct nvg_glibc_passwd *g,
                      struct nvg_glibc_passwd **result) {
    if (r != 0 || bres == NULL) {
        *result = NULL;
        return r;
    }
    g->g_name = b->pw_name;
    g->g_passwd = b->pw_passwd;
    g->g_uid = b->pw_uid;
    g->g_gid = b->pw_gid;
    g->g_gecos = b->pw_passwd;
    g->g_dir = b->pw_dir;
    g->g_shell = b->pw_shell;
    *result = g;
    return 0;
}

NVG_EXPORT int nvg_getpwnam_r(const char *name, struct nvg_glibc_passwd *g, char *buf, size_t len,
                              struct nvg_glibc_passwd **result) {
    struct passwd b, *bres = NULL;
    int r = getpwnam_r(name, &b, buf, len, &bres);
    return passwd_out(r, &b, bres, g, result);
}

NVG_EXPORT int nvg_getpwuid_r(uid_t uid, struct nvg_glibc_passwd *g, char *buf, size_t len,
                              struct nvg_glibc_passwd **result) {
    struct passwd b, *bres = NULL;
    int r = getpwuid_r(uid, &b, buf, len, &bres);
    return passwd_out(r, &b, bres, g, result);
}

/* getpwnam()/getpwuid() return a static struct in both libraries (POSIX: not thread-safe, overwritten by the
 * next call). Here the static is glibc's layout, pointing at the strings of bionic's own static result. */
static struct nvg_glibc_passwd passwd_static;

static struct nvg_glibc_passwd *passwd_static_out(const struct passwd *b) {
    if (b == NULL) {
        return NULL;
    }
    struct nvg_glibc_passwd *res = NULL;
    passwd_out(0, b, (struct passwd *)b, &passwd_static, &res);
    return res;
}

NVG_EXPORT struct nvg_glibc_passwd *nvg_getpwnam(const char *name) {
    return passwd_static_out(getpwnam(name));
}

NVG_EXPORT struct nvg_glibc_passwd *nvg_getpwuid(uid_t uid) {
    return passwd_static_out(getpwuid(uid));
}

/* ---- strtold_l ----
 * glibc's i386 long double is the 80-bit x87 type, bionic's x86 long double is double. Both
 * return the value in st(0), so the call ABI matches; what differs is precision (53-bit
 * mantissa instead of 64). The importers are the libstdc++ copies statically linked into
 * gpucomp/glvkspirv (num_get<long double>). Logged once per process (logcat, info) when it is
 * ever reached, so a precision question can be traced to it. */
NVG_EXPORT long double nvg_strtold_l(const char *s, char **end, locale_t loc) {
    static int said;
    if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) {
        __android_log_write(ANDROID_LOG_INFO, "libnvglibc",
                            "libnvglibc: strtold_l: bionic's x86 long double is double; the result "
                            "has 53-bit precision, not glibc's 64");
    }
    return strtold_l(s, end, loc);
}
