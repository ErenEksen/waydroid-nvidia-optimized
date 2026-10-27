/* glibc's versioned stat wrappers. x86_64: struct stat is the kernel struct in
 * both C libraries (size asserted in glibc_abi.h). i386: the *64 variants use the
 * kernel stat64 layout in both (bionic's LP32 struct stat is that layout too), while
 * the non-LFS __xstat/__lxstat/__fxstat fill glibc's 88-byte struct stat, converted
 * here with glibc's overflow rule (EOVERFLOW when st_ino, st_size or st_blocks do not
 * fit; glibc sysdeps/unix/sysv/linux/xstatconv.c __xstat32_conv). */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "nvglibc.h"

#if defined(__x86_64__)
#define NVG_ARCH_NAME "x86_64"
#else
#define NVG_ARCH_NAME "i386"
#endif

static void check_ver(const char *fn, int ver, int want) {
    if (ver != want) {
        nvgi_die("%s: version argument %d, expected %d (_STAT_VER/_MKNOD_VER of " NVG_ARCH_NAME
                 " glibc)", fn, ver, want);
    }
}

#if defined(__x86_64__)

NVG_EXPORT int nvg___xstat(int ver, const char *path, struct stat *buf) {
    check_ver("__xstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return stat(path, buf);
}

NVG_EXPORT int nvg___xstat64(int ver, const char *path, struct stat *buf) {
    check_ver("__xstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return stat(path, buf);
}

NVG_EXPORT int nvg___lxstat(int ver, const char *path, struct stat *buf) {
    check_ver("__lxstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return lstat(path, buf);
}

NVG_EXPORT int nvg___lxstat64(int ver, const char *path, struct stat *buf) {
    check_ver("__lxstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return lstat(path, buf);
}

NVG_EXPORT int nvg___fxstat(int ver, int fd, struct stat *buf) {
    check_ver("__fxstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return fstat(fd, buf);
}

NVG_EXPORT int nvg___fxstat64(int ver, int fd, struct stat *buf) {
    check_ver("__fxstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return fstat(fd, buf);
}

NVG_EXPORT int nvg___xmknod(int ver, const char *path, mode_t mode, dev_t *dev) {
    check_ver("__xmknod", ver, NVG_GLIBCU_MKNOD_VER_LINUX);
    return mknod(path, mode, *dev);
}

#else /* i386 */

/* bionic struct stat64 (kernel stat64) -> glibc i386 struct stat */
static int to_glibc_stat(int r, const struct stat64 *b, struct nvg_glibc_stat *g) {
    if (r != 0) {
        return r;
    }
    if (b->st_ino > UINT32_MAX || b->st_size > INT32_MAX || b->st_size < INT32_MIN ||
        b->st_blocks > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(g, 0, sizeof(*g));
    g->st_dev = b->st_dev;
    g->st_ino = (uint32_t)b->st_ino;
    g->st_mode = b->st_mode;
    g->st_nlink = b->st_nlink;
    g->st_uid = b->st_uid;
    g->st_gid = b->st_gid;
    g->st_rdev = b->st_rdev;
    g->st_size = (int32_t)b->st_size;
    g->st_blksize = (int32_t)b->st_blksize;
    g->st_blocks = (int32_t)b->st_blocks;
    g->st_atim.tv_sec = (int32_t)b->st_atim.tv_sec;
    g->st_atim.tv_nsec = (int32_t)b->st_atim.tv_nsec;
    g->st_mtim.tv_sec = (int32_t)b->st_mtim.tv_sec;
    g->st_mtim.tv_nsec = (int32_t)b->st_mtim.tv_nsec;
    g->st_ctim.tv_sec = (int32_t)b->st_ctim.tv_sec;
    g->st_ctim.tv_nsec = (int32_t)b->st_ctim.tv_nsec;
    return 0;
}

NVG_EXPORT int nvg___xstat(int ver, const char *path, struct nvg_glibc_stat *buf) {
    check_ver("__xstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    struct stat64 b;
    return to_glibc_stat(stat64(path, &b), &b, buf);
}

NVG_EXPORT int nvg___lxstat(int ver, const char *path, struct nvg_glibc_stat *buf) {
    check_ver("__lxstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    struct stat64 b;
    return to_glibc_stat(lstat64(path, &b), &b, buf);
}

NVG_EXPORT int nvg___fxstat(int ver, int fd, struct nvg_glibc_stat *buf) {
    check_ver("__fxstat", ver, NVG_GLIBCU_STAT_VER_LINUX);
    struct stat64 b;
    return to_glibc_stat(fstat64(fd, &b), &b, buf);
}

/* glibc i386 struct stat64 == kernel stat64 == bionic struct stat64 (glibc_abi.h asserts
 * the sizes; tools/abi-check.c and the layout probe compare every offset). */
NVG_EXPORT int nvg___xstat64(int ver, const char *path, struct stat64 *buf) {
    check_ver("__xstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return stat64(path, buf);
}

NVG_EXPORT int nvg___lxstat64(int ver, const char *path, struct stat64 *buf) {
    check_ver("__lxstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return lstat64(path, buf);
}

NVG_EXPORT int nvg___fxstat64(int ver, int fd, struct stat64 *buf) {
    check_ver("__fxstat64", ver, NVG_GLIBCU_STAT_VER_LINUX);
    return fstat64(fd, buf);
}

/* glibc's dev_t is 64-bit on i386, bionic's LP32 dev_t is 32-bit. */
NVG_EXPORT int nvg___xmknod(int ver, const char *path, mode_t mode, const uint64_t *dev) {
    check_ver("__xmknod", ver, NVG_GLIBCU_MKNOD_VER_LINUX);
    if (*dev > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return mknod(path, mode, (dev_t)*dev);
}

#endif
