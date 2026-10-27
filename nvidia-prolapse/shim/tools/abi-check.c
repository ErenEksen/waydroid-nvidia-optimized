/*
 * Host-side glibc layout probe. Compiled with the host gcc against the host
 * glibc headers; prints "<NAME> <value>" for every NVG_GLIBC_<NAME> constant
 * declared in core/glibc_abi.h. `make check-abi` compares the two lists.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <pwd.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

#define P(name, v) printf("%s %ld\n", name, (long)(v))

int main(void) {
    P("SIZEOF_SIGSET", sizeof(sigset_t));
    P("SIZEOF_SIGACTION", sizeof(struct sigaction));
    P("OFFSETOF_SIGACTION_HANDLER", offsetof(struct sigaction, sa_handler));
    P("OFFSETOF_SIGACTION_SA_MASK", offsetof(struct sigaction, sa_mask));
    P("OFFSETOF_SIGACTION_SA_FLAGS", offsetof(struct sigaction, sa_flags));
    P("OFFSETOF_SIGACTION_SA_RESTORER", offsetof(struct sigaction, sa_restorer));
    P("SIZEOF_JMP_BUF", sizeof(jmp_buf));
    P("OFFSETOF_JMP_BUF_MASK_WAS_SAVED", offsetof(struct __jmp_buf_tag, __mask_was_saved));
    P("SIZEOF_MUTEXATTR", sizeof(pthread_mutexattr_t));
    P("SIZEOF_CONDATTR", sizeof(pthread_condattr_t));
    P("SIZEOF_MALLINFO", sizeof(struct mallinfo));
    P("SIZEOF_MUTEX", sizeof(pthread_mutex_t));
    P("OFFSETOF_MUTEX_KIND", offsetof(pthread_mutex_t, __data.__kind));
    P("SIZEOF_COND", sizeof(pthread_cond_t));
    P("SIZEOF_ATTR", sizeof(pthread_attr_t));
    P("SIZEOF_STAT", sizeof(struct stat));
    P("SIZEOF_CPU_SET", sizeof(cpu_set_t));
    P("SIZEOF_DL_INFO", sizeof(Dl_info));
    P("SIZEOF_LINK_MAP", sizeof(struct link_map));
    P("OFFSETOF_LINK_MAP_L_NAME", offsetof(struct link_map, l_name));
    P("OFFSETOF_LINK_MAP_L_LD", offsetof(struct link_map, l_ld));
    P("OFFSETOF_LINK_MAP_L_PREV", offsetof(struct link_map, l_prev));
    P("RTLD_DL_SYMENT", RTLD_DL_SYMENT);
    P("RTLD_DL_LINKMAP", RTLD_DL_LINKMAP);
    P("RTLD_DEEPBIND", RTLD_DEEPBIND);
    P("RTLD_LAZY", RTLD_LAZY);
    P("RTLD_NOW", RTLD_NOW);
    P("RTLD_NOLOAD", RTLD_NOLOAD);
    P("RTLD_GLOBAL", RTLD_GLOBAL);
    P("RTLD_NODELETE", RTLD_NODELETE);
    P("RTLD_DEFAULT_IS_NULL", RTLD_DEFAULT == (void *)0);
    P("RTLD_NEXT_IS_MINUS_ONE", RTLD_NEXT == (void *)-1L);
    P("MUTEX_KIND_RECURSIVE", PTHREAD_MUTEX_RECURSIVE_NP);
    P("MUTEX_KIND_ERRORCHECK", PTHREAD_MUTEX_ERRORCHECK_NP);
    P("MUTEX_KIND_ADAPTIVE", PTHREAD_MUTEX_ADAPTIVE_NP);
    P("PTHREAD_INHERIT_SCHED", PTHREAD_INHERIT_SCHED);
    P("PTHREAD_EXPLICIT_SCHED", PTHREAD_EXPLICIT_SCHED);
#if defined(__i386__)
    P("SIZEOF_STAT64", sizeof(struct stat64));
    P("OFFSETOF_STAT_ST_INO", offsetof(struct stat, st_ino));
    P("OFFSETOF_STAT_ST_MODE", offsetof(struct stat, st_mode));
    P("OFFSETOF_STAT_ST_NLINK", offsetof(struct stat, st_nlink));
    P("OFFSETOF_STAT_ST_UID", offsetof(struct stat, st_uid));
    P("OFFSETOF_STAT_ST_GID", offsetof(struct stat, st_gid));
    P("OFFSETOF_STAT_ST_RDEV", offsetof(struct stat, st_rdev));
    P("OFFSETOF_STAT_ST_SIZE", offsetof(struct stat, st_size));
    P("OFFSETOF_STAT_ST_BLKSIZE", offsetof(struct stat, st_blksize));
    P("OFFSETOF_STAT_ST_BLOCKS", offsetof(struct stat, st_blocks));
    P("OFFSETOF_STAT_ST_ATIM", offsetof(struct stat, st_atim));
    P("OFFSETOF_STAT_ST_MTIM", offsetof(struct stat, st_mtim));
    P("OFFSETOF_STAT_ST_CTIM", offsetof(struct stat, st_ctim));
    P("SIZEOF_DIRENT", sizeof(struct dirent));
    P("OFFSETOF_DIRENT_D_OFF", offsetof(struct dirent, d_off));
    P("OFFSETOF_DIRENT_D_RECLEN", offsetof(struct dirent, d_reclen));
    P("OFFSETOF_DIRENT_D_TYPE", offsetof(struct dirent, d_type));
    P("OFFSETOF_DIRENT_D_NAME", offsetof(struct dirent, d_name));
    P("SIZEOF_STATFS", sizeof(struct statfs));
    P("OFFSETOF_STATFS_F_BLOCKS", offsetof(struct statfs, f_blocks));
    P("OFFSETOF_STATFS_F_FILES", offsetof(struct statfs, f_files));
    P("OFFSETOF_STATFS_F_FSID", offsetof(struct statfs, f_fsid));
    P("OFFSETOF_STATFS_F_NAMELEN", offsetof(struct statfs, f_namelen));
    P("OFFSETOF_STATFS_F_FRSIZE", offsetof(struct statfs, f_frsize));
    P("OFFSETOF_STATFS_F_FLAGS", offsetof(struct statfs, f_flags));
    P("SIZEOF_PASSWD", sizeof(struct passwd));
    P("OFFSETOF_PASSWD_PW_UID", offsetof(struct passwd, pw_uid));
    P("OFFSETOF_PASSWD_PW_GECOS", offsetof(struct passwd, pw_gecos));
    P("OFFSETOF_PASSWD_PW_DIR", offsetof(struct passwd, pw_dir));
    P("OFFSETOF_PASSWD_PW_SHELL", offsetof(struct passwd, pw_shell));
    P("SIZEOF_RWLOCK", sizeof(pthread_rwlock_t));
    P("OFFSETOF_RWLOCK_FLAGS", offsetof(pthread_rwlock_t, __data.__flags));
    P("RWLOCK_PREFER_WRITER_NONRECURSIVE", PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    P("SIZEOF_DEV_T", sizeof(dev_t));
    {
        /* PTHREAD_RWLOCK_INITIALIZER is all zero: the translator's "pointer 0 = make it on first use" */
        pthread_rwlock_t r = PTHREAD_RWLOCK_INITIALIZER;
        const unsigned char *p = (const unsigned char *)&r;
        for (unsigned i = 0; i < sizeof(r); i++) {
            if (p[i]) {
                fprintf(stderr, "abi-check: PTHREAD_RWLOCK_INITIALIZER byte %u is non-zero\n", i);
                return 1;
            }
        }
        pthread_rwlock_t w = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
        if (w.__data.__flags != PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) {
            fprintf(stderr, "abi-check: the writer-preferring rwlock initialiser does not set __flags\n");
            return 1;
        }
    }
#endif
    /* The static initialisers really store the kind at __kind and nothing in
     * the bytes before it (DESIGN.md mutex conversion premise; 16 on x86_64, 12 on i386). */
    {
        pthread_mutex_t r = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
        pthread_mutex_t e = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
        const unsigned char *pr = (const unsigned char *)&r, *pe = (const unsigned char *)&e;
        for (int i = 0; i < (int)offsetof(pthread_mutex_t, __data.__kind); i++) {
            if (pr[i] || pe[i]) {
                fprintf(stderr, "abi-check: static initialiser byte %d is non-zero\n", i);
                return 1;
            }
        }
        if (r.__data.__kind != PTHREAD_MUTEX_RECURSIVE_NP ||
            e.__data.__kind != PTHREAD_MUTEX_ERRORCHECK_NP) {
            fprintf(stderr, "abi-check: static initialisers do not set __kind as expected\n");
            return 1;
        }
    }
    return 0;
}
