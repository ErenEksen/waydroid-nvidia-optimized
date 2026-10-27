/*
 * glibc x86 ABI facts the core depends on, declared locally (an NDK build
 * cannot include glibc headers). Source: state abi-contract.md §4 (x86_64) and
 * DESIGN.md "32-bit x86" (i386), and the glibc public headers. Every
 * NVG_GLIBC_<NAME> value is re-measured by the host-side tools/abi-check.c
 * (compiled with the host gcc against the host glibc, -m32 for i386) and compared
 * by `make check-abi`, which prints the same <NAME> <value> lines.
 *
 * NVG_GLIBCU_* values cannot be measured from current glibc headers (glibc >=
 * 2.33 removed _STAT_VER/_MKNOD_VER from the public headers); their source is
 * cited inline.
 */
#pragma once

#if defined(__x86_64__)
#define NVG_GLIBC_SIZEOF_SIGSET 128
#define NVG_GLIBC_SIZEOF_SIGACTION 152
#define NVG_GLIBC_OFFSETOF_SIGACTION_HANDLER 0
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_MASK 8
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_FLAGS 136
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_RESTORER 144
#define NVG_GLIBC_SIZEOF_JMP_BUF 200
#define NVG_GLIBC_OFFSETOF_JMP_BUF_MASK_WAS_SAVED 64
#define NVG_GLIBC_SIZEOF_MUTEXATTR 4
#define NVG_GLIBC_SIZEOF_CONDATTR 4
#define NVG_GLIBC_SIZEOF_MALLINFO 40
#define NVG_GLIBC_SIZEOF_MUTEX 40
#define NVG_GLIBC_OFFSETOF_MUTEX_KIND 16
#define NVG_GLIBC_SIZEOF_COND 48
#define NVG_GLIBC_SIZEOF_ATTR 56
#define NVG_GLIBC_SIZEOF_STAT 144
#define NVG_GLIBC_SIZEOF_CPU_SET 128
#define NVG_GLIBC_SIZEOF_DL_INFO 32
#define NVG_GLIBC_SIZEOF_LINK_MAP 40
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_NAME 8
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_LD 16
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_PREV 32
#elif defined(__i386__)
#define NVG_GLIBC_SIZEOF_SIGSET 128
#define NVG_GLIBC_SIZEOF_SIGACTION 140
#define NVG_GLIBC_OFFSETOF_SIGACTION_HANDLER 0
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_MASK 4
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_FLAGS 132
#define NVG_GLIBC_OFFSETOF_SIGACTION_SA_RESTORER 136
#define NVG_GLIBC_SIZEOF_JMP_BUF 156
#define NVG_GLIBC_OFFSETOF_JMP_BUF_MASK_WAS_SAVED 24
#define NVG_GLIBC_SIZEOF_MUTEXATTR 4
#define NVG_GLIBC_SIZEOF_CONDATTR 4
#define NVG_GLIBC_SIZEOF_MALLINFO 40
#define NVG_GLIBC_SIZEOF_MUTEX 24
#define NVG_GLIBC_OFFSETOF_MUTEX_KIND 12
#define NVG_GLIBC_SIZEOF_COND 48
#define NVG_GLIBC_SIZEOF_ATTR 36
#define NVG_GLIBC_SIZEOF_STAT 88
#define NVG_GLIBC_SIZEOF_CPU_SET 128
#define NVG_GLIBC_SIZEOF_DL_INFO 16
#define NVG_GLIBC_SIZEOF_LINK_MAP 20
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_NAME 4
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_LD 8
#define NVG_GLIBC_OFFSETOF_LINK_MAP_L_PREV 16
/* i386-only classes (DESIGN.md "32-bit x86"): non-LFS struct stat / dirent / statfs,
 * struct passwd (bionic LP32 has no pw_gecos), pthread_rwlock_t (bionic LP32 is 40 B). */
#define NVG_GLIBC_SIZEOF_STAT64 96
#define NVG_GLIBC_OFFSETOF_STAT_ST_INO 12
#define NVG_GLIBC_OFFSETOF_STAT_ST_MODE 16
#define NVG_GLIBC_OFFSETOF_STAT_ST_NLINK 20
#define NVG_GLIBC_OFFSETOF_STAT_ST_UID 24
#define NVG_GLIBC_OFFSETOF_STAT_ST_GID 28
#define NVG_GLIBC_OFFSETOF_STAT_ST_RDEV 32
#define NVG_GLIBC_OFFSETOF_STAT_ST_SIZE 44
#define NVG_GLIBC_OFFSETOF_STAT_ST_BLKSIZE 48
#define NVG_GLIBC_OFFSETOF_STAT_ST_BLOCKS 52
#define NVG_GLIBC_OFFSETOF_STAT_ST_ATIM 56
#define NVG_GLIBC_OFFSETOF_STAT_ST_MTIM 64
#define NVG_GLIBC_OFFSETOF_STAT_ST_CTIM 72
#define NVG_GLIBC_SIZEOF_DIRENT 268
#define NVG_GLIBC_OFFSETOF_DIRENT_D_OFF 4
#define NVG_GLIBC_OFFSETOF_DIRENT_D_RECLEN 8
#define NVG_GLIBC_OFFSETOF_DIRENT_D_TYPE 10
#define NVG_GLIBC_OFFSETOF_DIRENT_D_NAME 11
#define NVG_GLIBC_SIZEOF_STATFS 64
#define NVG_GLIBC_OFFSETOF_STATFS_F_BLOCKS 8
#define NVG_GLIBC_OFFSETOF_STATFS_F_FILES 20
#define NVG_GLIBC_OFFSETOF_STATFS_F_FSID 28
#define NVG_GLIBC_OFFSETOF_STATFS_F_NAMELEN 36
#define NVG_GLIBC_OFFSETOF_STATFS_F_FRSIZE 40
#define NVG_GLIBC_OFFSETOF_STATFS_F_FLAGS 44
#define NVG_GLIBC_SIZEOF_PASSWD 28
#define NVG_GLIBC_OFFSETOF_PASSWD_PW_UID 8
#define NVG_GLIBC_OFFSETOF_PASSWD_PW_GECOS 16
#define NVG_GLIBC_OFFSETOF_PASSWD_PW_DIR 20
#define NVG_GLIBC_OFFSETOF_PASSWD_PW_SHELL 24
#define NVG_GLIBC_SIZEOF_RWLOCK 32
#define NVG_GLIBC_OFFSETOF_RWLOCK_FLAGS 24
#define NVG_GLIBC_RWLOCK_PREFER_WRITER_NONRECURSIVE 2 /* PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP */
#define NVG_GLIBC_SIZEOF_DEV_T 8
#else
#error "libnvglibc: unsupported architecture (x86_64 and i386 only)"
#endif
/* glibc dlopen flags (same on x86_64 and i386). bionic's LP32 values differ ("LP32 has
 * historical ABI breakage", dlfcn.h: RTLD_NOW 0, RTLD_GLOBAL 2, RTLD_DEFAULT 0xffffffff,
 * RTLD_NEXT 0xfffffffe), so core/dl.c maps them on i386. glibc RTLD_DEFAULT is NULL and
 * RTLD_NEXT (void *)-1 (abi-check prints 1 for each when that holds). */
#define NVG_GLIBC_RTLD_LAZY 1
#define NVG_GLIBC_RTLD_NOW 2
#define NVG_GLIBC_RTLD_NOLOAD 4
#define NVG_GLIBC_RTLD_GLOBAL 256
#define NVG_GLIBC_RTLD_NODELETE 4096
#define NVG_GLIBC_RTLD_DEFAULT_IS_NULL 1
#define NVG_GLIBC_RTLD_NEXT_IS_MINUS_ONE 1
#define NVG_GLIBC_RTLD_DL_SYMENT 1
#define NVG_GLIBC_RTLD_DL_LINKMAP 2
#define NVG_GLIBC_RTLD_DEEPBIND 8
#define NVG_GLIBC_MUTEX_KIND_RECURSIVE 1   /* PTHREAD_MUTEX_RECURSIVE_NP */
#define NVG_GLIBC_MUTEX_KIND_ERRORCHECK 2  /* PTHREAD_MUTEX_ERRORCHECK_NP */
#define NVG_GLIBC_MUTEX_KIND_ADAPTIVE 3    /* PTHREAD_MUTEX_ADAPTIVE_NP */
#define NVG_GLIBC_PTHREAD_INHERIT_SCHED 0  /* pthread.h enum */
#define NVG_GLIBC_PTHREAD_EXPLICIT_SCHED 1

/* glibc sysdeps/unix/sysv/linux/x86/xstatver.h (pre-2.33 bits/stat.h):
 * x86_64 _STAT_VER_LINUX 1, _MKNOD_VER_LINUX 0 (observed: eglcore calls __fxstat(1, ...),
 * abi-contract.md §6); i386 _STAT_VER_LINUX 3, _MKNOD_VER_LINUX 1 (observed: the i386
 * glsi/EGL_nvidia/gpucomp call sites push $3 before __xstat/__fxstat/__lxstat[64]). */
#if defined(__x86_64__)
#define NVG_GLIBCU_STAT_VER_LINUX 1
#define NVG_GLIBCU_MKNOD_VER_LINUX 0
#else
#define NVG_GLIBCU_STAT_VER_LINUX 3
#define NVG_GLIBCU_MKNOD_VER_LINUX 1
#endif

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

/* 1024 signal bits on every glibc target: 16 longs on x86_64, 32 on i386 */
typedef struct { unsigned long val[1024 / (8 * sizeof(unsigned long))]; } nvg_glibc_sigset_t;

struct nvg_glibc_sigaction {
    void *handler;                 /* sa_handler / sa_sigaction union */
    nvg_glibc_sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

struct nvg_glibc_jmp_buf_tag {
#if defined(__x86_64__)
    long jb[8];                    /* rbx rbp r12 r13 r14 r15 rsp rip */
#else
    int jb[6];                     /* ebx esi edi ebp esp eip */
#endif
    int mask_was_saved;
    nvg_glibc_sigset_t saved_mask;
};

typedef struct { uint32_t v; } nvg_glibc_mutexattr_t;
typedef struct { uint32_t v; } nvg_glibc_condattr_t;

#if defined(__x86_64__)
struct nvg_glibc_mutex {           /* struct __pthread_mutex_s */
    int lock;
    unsigned count;
    int owner;
    unsigned nusers;
    int kind;
    short spins;
    short elision;
    void *prev, *next;
};
#else
struct nvg_glibc_mutex {           /* struct __pthread_mutex_s, i386 */
    int lock;
    unsigned count;
    int owner;
    int kind;
    unsigned nusers;
    int spins_or_list;
};

struct nvg_glibc_stat {            /* non-LFS struct stat, i386 (88 B) */
    uint64_t st_dev;
    uint16_t pad1;
    uint32_t st_ino __attribute__((aligned(4)));
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint16_t pad2;
    int32_t st_size __attribute__((aligned(4)));
    int32_t st_blksize;
    int32_t st_blocks;
    struct { int32_t tv_sec, tv_nsec; } st_atim, st_mtim, st_ctim;
    uint32_t reserved4, reserved5;
} __attribute__((packed, aligned(4)));

struct nvg_glibc_dirent {          /* non-LFS struct dirent, i386 (268 B) */
    uint32_t d_ino;
    int32_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} __attribute__((aligned(4)));

struct nvg_glibc_statfs {          /* non-LFS struct statfs, i386 (64 B) */
    int32_t f_type, f_bsize;
    uint32_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    int32_t f_namelen, f_frsize, f_flags, f_spare[4];
};

struct nvg_glibc_passwd {          /* struct passwd (glibc keeps pw_gecos on i386); fields are
                                    * prefixed g: bionic #defines pw_gecos on LP32 */
    char *g_name, *g_passwd;
    uint32_t g_uid, g_gid;
    char *g_gecos, *g_dir, *g_shell;
};

struct nvg_glibc_rwlock {          /* pthread_rwlock_t storage, i386 (32 B) */
    void *bionic;                  /* ours: the real (40-byte) bionic rwlock; 0 = not yet made */
    uint32_t words[5];             /* glibc __writers .. __pad4: unused by us */
    uint8_t flags;                 /* glibc __flags: PTHREAD_RWLOCK_PREFER_* (static initialisers) */
    uint8_t shared, elision, pad2;
    int32_t cur_writer;
};

_Static_assert(sizeof(struct nvg_glibc_stat) == NVG_GLIBC_SIZEOF_STAT, "stat");
_Static_assert(offsetof(struct nvg_glibc_stat, st_ino) == NVG_GLIBC_OFFSETOF_STAT_ST_INO, "st_ino");
_Static_assert(offsetof(struct nvg_glibc_stat, st_mode) == NVG_GLIBC_OFFSETOF_STAT_ST_MODE, "st_mode");
_Static_assert(offsetof(struct nvg_glibc_stat, st_nlink) == NVG_GLIBC_OFFSETOF_STAT_ST_NLINK, "st_nlink");
_Static_assert(offsetof(struct nvg_glibc_stat, st_uid) == NVG_GLIBC_OFFSETOF_STAT_ST_UID, "st_uid");
_Static_assert(offsetof(struct nvg_glibc_stat, st_gid) == NVG_GLIBC_OFFSETOF_STAT_ST_GID, "st_gid");
_Static_assert(offsetof(struct nvg_glibc_stat, st_rdev) == NVG_GLIBC_OFFSETOF_STAT_ST_RDEV, "st_rdev");
_Static_assert(offsetof(struct nvg_glibc_stat, st_size) == NVG_GLIBC_OFFSETOF_STAT_ST_SIZE, "st_size");
_Static_assert(offsetof(struct nvg_glibc_stat, st_blksize) == NVG_GLIBC_OFFSETOF_STAT_ST_BLKSIZE, "st_blksize");
_Static_assert(offsetof(struct nvg_glibc_stat, st_blocks) == NVG_GLIBC_OFFSETOF_STAT_ST_BLOCKS, "st_blocks");
_Static_assert(offsetof(struct nvg_glibc_stat, st_atim) == NVG_GLIBC_OFFSETOF_STAT_ST_ATIM, "st_atim");
_Static_assert(offsetof(struct nvg_glibc_stat, st_mtim) == NVG_GLIBC_OFFSETOF_STAT_ST_MTIM, "st_mtim");
_Static_assert(offsetof(struct nvg_glibc_stat, st_ctim) == NVG_GLIBC_OFFSETOF_STAT_ST_CTIM, "st_ctim");
_Static_assert(sizeof(struct nvg_glibc_dirent) == NVG_GLIBC_SIZEOF_DIRENT, "dirent");
_Static_assert(offsetof(struct nvg_glibc_dirent, d_off) == NVG_GLIBC_OFFSETOF_DIRENT_D_OFF, "d_off");
_Static_assert(offsetof(struct nvg_glibc_dirent, d_reclen) == NVG_GLIBC_OFFSETOF_DIRENT_D_RECLEN, "d_reclen");
_Static_assert(offsetof(struct nvg_glibc_dirent, d_type) == NVG_GLIBC_OFFSETOF_DIRENT_D_TYPE, "d_type");
_Static_assert(offsetof(struct nvg_glibc_dirent, d_name) == NVG_GLIBC_OFFSETOF_DIRENT_D_NAME, "d_name");
_Static_assert(sizeof(struct nvg_glibc_statfs) == NVG_GLIBC_SIZEOF_STATFS, "statfs");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_blocks) == NVG_GLIBC_OFFSETOF_STATFS_F_BLOCKS, "f_blocks");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_files) == NVG_GLIBC_OFFSETOF_STATFS_F_FILES, "f_files");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_fsid) == NVG_GLIBC_OFFSETOF_STATFS_F_FSID, "f_fsid");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_namelen) == NVG_GLIBC_OFFSETOF_STATFS_F_NAMELEN, "f_namelen");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_frsize) == NVG_GLIBC_OFFSETOF_STATFS_F_FRSIZE, "f_frsize");
_Static_assert(offsetof(struct nvg_glibc_statfs, f_flags) == NVG_GLIBC_OFFSETOF_STATFS_F_FLAGS, "f_flags");
_Static_assert(sizeof(struct nvg_glibc_passwd) == NVG_GLIBC_SIZEOF_PASSWD, "passwd");
_Static_assert(offsetof(struct nvg_glibc_passwd, g_uid) == NVG_GLIBC_OFFSETOF_PASSWD_PW_UID, "pw_uid");
_Static_assert(offsetof(struct nvg_glibc_passwd, g_gecos) == NVG_GLIBC_OFFSETOF_PASSWD_PW_GECOS, "pw_gecos");
_Static_assert(offsetof(struct nvg_glibc_passwd, g_dir) == NVG_GLIBC_OFFSETOF_PASSWD_PW_DIR, "pw_dir");
_Static_assert(offsetof(struct nvg_glibc_passwd, g_shell) == NVG_GLIBC_OFFSETOF_PASSWD_PW_SHELL, "pw_shell");
_Static_assert(sizeof(struct nvg_glibc_rwlock) == NVG_GLIBC_SIZEOF_RWLOCK, "pthread_rwlock_t");
_Static_assert(offsetof(struct nvg_glibc_rwlock, flags) == NVG_GLIBC_OFFSETOF_RWLOCK_FLAGS, "__flags");
#endif

struct nvg_glibc_mallinfo {
    int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};

struct nvg_glibc_link_map {        /* public part of glibc's struct link_map */
    uintptr_t l_addr;
    char *l_name;
    void *l_ld;
    struct nvg_glibc_link_map *l_next, *l_prev;
};

_Static_assert(sizeof(nvg_glibc_sigset_t) == NVG_GLIBC_SIZEOF_SIGSET, "sigset_t");
_Static_assert(sizeof(struct nvg_glibc_sigaction) == NVG_GLIBC_SIZEOF_SIGACTION, "sigaction");
_Static_assert(offsetof(struct nvg_glibc_sigaction, handler) == NVG_GLIBC_OFFSETOF_SIGACTION_HANDLER, "sa_handler");
_Static_assert(offsetof(struct nvg_glibc_sigaction, sa_mask) == NVG_GLIBC_OFFSETOF_SIGACTION_SA_MASK, "sa_mask");
_Static_assert(offsetof(struct nvg_glibc_sigaction, sa_flags) == NVG_GLIBC_OFFSETOF_SIGACTION_SA_FLAGS, "sa_flags");
_Static_assert(offsetof(struct nvg_glibc_sigaction, sa_restorer) == NVG_GLIBC_OFFSETOF_SIGACTION_SA_RESTORER, "sa_restorer");
_Static_assert(sizeof(struct nvg_glibc_jmp_buf_tag) == NVG_GLIBC_SIZEOF_JMP_BUF, "jmp_buf");
_Static_assert(offsetof(struct nvg_glibc_jmp_buf_tag, mask_was_saved) == NVG_GLIBC_OFFSETOF_JMP_BUF_MASK_WAS_SAVED, "__mask_was_saved");
_Static_assert(sizeof(nvg_glibc_mutexattr_t) == NVG_GLIBC_SIZEOF_MUTEXATTR, "pthread_mutexattr_t");
_Static_assert(sizeof(nvg_glibc_condattr_t) == NVG_GLIBC_SIZEOF_CONDATTR, "pthread_condattr_t");
_Static_assert(sizeof(struct nvg_glibc_mutex) == NVG_GLIBC_SIZEOF_MUTEX, "pthread_mutex_t");
_Static_assert(offsetof(struct nvg_glibc_mutex, kind) == NVG_GLIBC_OFFSETOF_MUTEX_KIND, "__kind");
_Static_assert(sizeof(struct nvg_glibc_mallinfo) == NVG_GLIBC_SIZEOF_MALLINFO, "mallinfo");
_Static_assert(sizeof(struct nvg_glibc_link_map) == NVG_GLIBC_SIZEOF_LINK_MAP, "link_map");
_Static_assert(offsetof(struct nvg_glibc_link_map, l_name) == NVG_GLIBC_OFFSETOF_LINK_MAP_L_NAME, "l_name");
_Static_assert(offsetof(struct nvg_glibc_link_map, l_ld) == NVG_GLIBC_OFFSETOF_LINK_MAP_L_LD, "l_ld");
_Static_assert(offsetof(struct nvg_glibc_link_map, l_prev) == NVG_GLIBC_OFFSETOF_LINK_MAP_L_PREV, "l_prev");

/* Types passed straight through identical trampolines must have glibc's size
 * in bionic too (only meaningful when compiled against bionic headers). */
#if defined(__BIONIC__)
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <pwd.h>
#include <semaphore.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#if defined(__LP64__)
_Static_assert(sizeof(pthread_mutex_t) == NVG_GLIBC_SIZEOF_MUTEX, "bionic pthread_mutex_t size");
_Static_assert(sizeof(pthread_cond_t) == NVG_GLIBC_SIZEOF_COND, "bionic pthread_cond_t size");
_Static_assert(sizeof(pthread_attr_t) == NVG_GLIBC_SIZEOF_ATTR, "bionic pthread_attr_t size");
_Static_assert(sizeof(struct stat) == NVG_GLIBC_SIZEOF_STAT, "bionic struct stat size");
_Static_assert(sizeof(cpu_set_t) == NVG_GLIBC_SIZEOF_CPU_SET, "bionic cpu_set_t size");
_Static_assert(sizeof(Dl_info) == NVG_GLIBC_SIZEOF_DL_INFO, "bionic Dl_info size");
_Static_assert(sizeof(pthread_mutexattr_t) == 8, "bionic pthread_mutexattr_t is a long");
_Static_assert(sizeof(pthread_condattr_t) == 8, "bionic pthread_condattr_t is a long");
_Static_assert(sizeof(sigset64_t) == 8, "bionic LP64 sigset64_t is 8 bytes");
_Static_assert(offsetof(struct sigaction64, sa_flags) == 0, "bionic sigaction64 sa_flags");
_Static_assert(offsetof(struct sigaction64, sa_handler) == 8, "bionic sigaction64 handler");
_Static_assert(offsetof(struct sigaction64, sa_mask) == 16, "bionic sigaction64 sa_mask");
_Static_assert(offsetof(struct sigaction64, sa_restorer) == 24, "bionic sigaction64 sa_restorer");
#else
/* LP32: an identical trampoline is safe when bionic's object is no larger than glibc's
 * (the blob owns glibc-sized storage) and the zero initialiser means the same; where bionic's
 * is larger or laid out differently there is a translator (DESIGN.md "32-bit x86"). */
_Static_assert(sizeof(pthread_mutex_t) <= NVG_GLIBC_SIZEOF_MUTEX, "bionic LP32 pthread_mutex_t fits");
_Static_assert(sizeof(pthread_cond_t) <= NVG_GLIBC_SIZEOF_COND, "bionic LP32 pthread_cond_t fits");
_Static_assert(sizeof(pthread_attr_t) <= NVG_GLIBC_SIZEOF_ATTR, "bionic LP32 pthread_attr_t fits");
_Static_assert(sizeof(sem_t) <= 16, "bionic LP32 sem_t fits glibc's 16 bytes");
_Static_assert(sizeof(cpu_set_t) <= NVG_GLIBC_SIZEOF_CPU_SET, "bionic LP32 cpu_set_t fits");
_Static_assert(sizeof(pthread_rwlock_t) > NVG_GLIBC_SIZEOF_RWLOCK, "bionic LP32 rwlock is larger: translated");
_Static_assert(sizeof(struct stat) == NVG_GLIBC_SIZEOF_STAT64, "bionic LP32 struct stat is the stat64 layout");
_Static_assert(sizeof(struct stat64) == NVG_GLIBC_SIZEOF_STAT64, "bionic LP32 struct stat64 size");
_Static_assert(sizeof(Dl_info) == NVG_GLIBC_SIZEOF_DL_INFO, "bionic Dl_info size");
_Static_assert(sizeof(pthread_mutexattr_t) == 4, "bionic LP32 pthread_mutexattr_t is a 4-byte long");
_Static_assert(sizeof(pthread_condattr_t) == 4, "bionic LP32 pthread_condattr_t is a 4-byte long");
_Static_assert(sizeof(sigset64_t) == 8, "bionic sigset64_t is 8 bytes");
_Static_assert(offsetof(struct sigaction64, sa_handler) == 0, "bionic LP32 sigaction64 handler");
_Static_assert(offsetof(struct sigaction64, sa_flags) == 4, "bionic LP32 sigaction64 sa_flags");
_Static_assert(offsetof(struct sigaction64, sa_restorer) == 8, "bionic LP32 sigaction64 sa_restorer");
_Static_assert(offsetof(struct sigaction64, sa_mask) == 12, "bionic LP32 sigaction64 sa_mask");
#endif
#endif
#endif /* !__ASSEMBLER__ */
