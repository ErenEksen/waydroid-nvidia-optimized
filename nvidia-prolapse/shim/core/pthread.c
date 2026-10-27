/* pthread translations: 4-byte glibc attrs <-> bionic long attrs, glibc static
 * recursive/errorcheck mutex initialisers, glibc default thread stack size,
 * pthread_setaffinity_np; on i386 also pthread_rwlock_t (bionic's LP32 rwlock is
 * 40 bytes, glibc's 32: the glibc object holds a pointer to a bionic one). */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvglibc.h"

/* ---- 4-byte attr <-> long ---- */

static long widen(const void *g) {
    uint32_t v;
    memcpy(&v, g, sizeof(v));
    return (long)v;
}

static void narrow(void *g, long v, const char *fn) {
#if defined(__LP64__)
    if ((unsigned long)v >> 32) {
#else
    if (0) { /* LP32: a bionic long is 4 bytes, like glibc's attr */
#endif
        nvgi_die("%s: bionic attr value %#lx does not fit glibc's 4-byte attr", fn,
                 (unsigned long)v);
    }
    uint32_t u = (uint32_t)v;
    memcpy(g, &u, sizeof(u));
}

/* destroy writes a poison value (-1 / 0xdeada11d); keep its low 32 bits. */
static void narrow_poison(void *g, long v) {
    uint32_t u = (uint32_t)v;
    memcpy(g, &u, sizeof(u));
}

NVG_EXPORT int nvg_pthread_mutexattr_init(nvg_glibc_mutexattr_t *ga) {
    pthread_mutexattr_t a;
    int r = pthread_mutexattr_init(&a);
    if (r == 0) {
        narrow(ga, a, "pthread_mutexattr_init");
    }
    return r;
}

NVG_EXPORT int nvg_pthread_mutexattr_destroy(nvg_glibc_mutexattr_t *ga) {
    pthread_mutexattr_t a = widen(ga);
    int r = pthread_mutexattr_destroy(&a);
    narrow_poison(ga, a);
    return r;
}

NVG_EXPORT int nvg_pthread_mutexattr_settype(nvg_glibc_mutexattr_t *ga, int type) {
    pthread_mutexattr_t a = widen(ga);
    int r = pthread_mutexattr_settype(&a, type); /* PTHREAD_MUTEX_* identical */
    if (r == 0) {
        narrow(ga, a, "pthread_mutexattr_settype");
    }
    return r;
}

NVG_EXPORT int nvg_pthread_mutexattr_setprotocol(nvg_glibc_mutexattr_t *ga, int protocol) {
    pthread_mutexattr_t a = widen(ga);
    int r = pthread_mutexattr_setprotocol(&a, protocol); /* PTHREAD_PRIO_* identical */
    if (r == 0) {
        narrow(ga, a, "pthread_mutexattr_setprotocol");
    }
    return r;
}

NVG_EXPORT int nvg_pthread_condattr_init(nvg_glibc_condattr_t *ga) {
    pthread_condattr_t a;
    int r = pthread_condattr_init(&a);
    if (r == 0) {
        narrow(ga, a, "pthread_condattr_init");
    }
    return r;
}

NVG_EXPORT int nvg_pthread_condattr_destroy(nvg_glibc_condattr_t *ga) {
    pthread_condattr_t a = widen(ga);
    int r = pthread_condattr_destroy(&a);
    narrow_poison(ga, a);
    return r;
}

NVG_EXPORT int nvg_pthread_condattr_setclock(nvg_glibc_condattr_t *ga, clockid_t clk) {
    pthread_condattr_t a = widen(ga);
    int r = pthread_condattr_setclock(&a, clk); /* CLOCK_* identical */
    if (r == 0) {
        narrow(ga, a, "pthread_condattr_setclock");
    }
    return r;
}

NVG_EXPORT int nvg_pthread_cond_init(pthread_cond_t *c, const nvg_glibc_condattr_t *ga) {
    if (ga == NULL) {
        return pthread_cond_init(c, NULL);
    }
    pthread_condattr_t a = widen(ga);
    return pthread_cond_init(c, &a);
}

/* ---- mutexes ---- */

static unsigned long converted[3];

/* glibc PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP / ..._ERRORCHECK_... leave bytes
 * 0..15 zero and set __kind (+16). Bionic keeps its state in the first word and
 * never reads +16, so turn such a mutex into the bionic equivalent before
 * bionic first sees it. Every lock path runs this first; the CAS settles races
 * between first users. */
static void convert_static(pthread_mutex_t *m) {
    int kind = ((const struct nvg_glibc_mutex *)(void *)m)->kind & 3;
    if (kind != NVG_GLIBC_MUTEX_KIND_RECURSIVE && kind != NVG_GLIBC_MUTEX_KIND_ERRORCHECK) {
        return;
    }
    int *w = (int *)(void *)m;
    int expected = 0;
    if (__atomic_load_n(w, __ATOMIC_ACQUIRE) == 0 &&
        __atomic_compare_exchange_n(w, &expected, nvgi_mutex_template[kind], 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&converted[kind], 1, __ATOMIC_RELAXED);
        nvgi_trace("converted glibc static %s mutex %p",
                   kind == NVG_GLIBC_MUTEX_KIND_RECURSIVE ? "recursive" : "errorcheck",
                   (void *)m);
    }
}

NVG_EXPORT int nvg_pthread_mutex_init(pthread_mutex_t *m, const nvg_glibc_mutexattr_t *ga) {
#if !defined(__LP64__)
    /* bionic's LP32 mutex is 4 bytes and its init writes only those; clear glibc's whole
     * object so no stale __kind (+12) can later be taken for a static initialiser. */
    memset(m, 0, NVG_GLIBC_SIZEOF_MUTEX);
#endif
    if (ga == NULL) {
        return pthread_mutex_init(m, NULL);
    }
    pthread_mutexattr_t a = widen(ga);
    /* bionic zeroes all 40 bytes, so glibc's __kind (+16) reads 0 afterwards */
    return pthread_mutex_init(m, &a);
}

NVG_EXPORT int nvg_pthread_mutex_lock(pthread_mutex_t *m) {
    convert_static(m);
    return pthread_mutex_lock(m);
}

NVG_EXPORT int nvg_pthread_mutex_trylock(pthread_mutex_t *m) {
    convert_static(m);
    return pthread_mutex_trylock(m);
}

NVG_EXPORT int nvg_pthread_mutex_unlock(pthread_mutex_t *m) {
    convert_static(m);
    return pthread_mutex_unlock(m);
}

NVG_EXPORT int nvg_pthread_mutex_destroy(pthread_mutex_t *m) {
    convert_static(m);
    return pthread_mutex_destroy(m);
}

__attribute__((destructor)) static void report_conversions(void) {
    if (nvgi_trace_on) {
        nvgi_trace("glibc static mutex conversions: recursive %lu, errorcheck %lu",
                   __atomic_load_n(&converted[1], __ATOMIC_RELAXED),
                   __atomic_load_n(&converted[2], __ATOMIC_RELAXED));
    }
}

/* ---- threads ---- */

static int attr_init_glibc(pthread_attr_t *attr) {
    int r = pthread_attr_init(attr);
    if (r != 0) {
        return r;
    }
    r = pthread_attr_setstacksize(attr, nvgi_default_stack);
    if (r != 0) {
        nvgi_die("pthread_attr_init: pthread_attr_setstacksize(%zu) failed: %d",
                 nvgi_default_stack, r);
    }
    return 0;
}

NVG_EXPORT int nvg_pthread_attr_init(pthread_attr_t *attr) {
    return attr_init_glibc(attr);
}

/* glibc PTHREAD_INHERIT_SCHED is 0 and PTHREAD_EXPLICIT_SCHED 1 (pthread.h enum); bionic swaps them
 * (PTHREAD_EXPLICIT_SCHED 0, PTHREAD_INHERIT_SCHED 1). Any other value is EINVAL in glibc
 * (nptl/pthread_attr_setinheritsched.c); the table check in gen/constants.h pins both values. */
NVG_EXPORT int nvg_pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit) {
    switch (inherit) {
    case NVG_GLIBC_PTHREAD_INHERIT_SCHED:
        return pthread_attr_setinheritsched(attr, PTHREAD_INHERIT_SCHED);
    case NVG_GLIBC_PTHREAD_EXPLICIT_SCHED:
        return pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
    default:
        return EINVAL;
    }
}

NVG_EXPORT int nvg_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                                  void *(*fn)(void *), void *arg) {
    if (attr != NULL) {
        return pthread_create(t, attr, fn, arg);
    }
    pthread_attr_t a;
    int r = attr_init_glibc(&a);
    if (r != 0) {
        return r;
    }
    r = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}

NVG_EXPORT int nvg_pthread_setaffinity_np(pthread_t t, size_t size, const void *set) {
    pid_t tid = pthread_gettid_np(t);
    if (sched_setaffinity(tid, size, (const cpu_set_t *)set) != 0) {
        return errno; /* glibc returns the error number instead of -1 */
    }
    return 0;
}

#if !defined(__LP64__)
/* ---- i386 rwlocks ----
 *
 * bionic's LP32 pthread_rwlock_t is 40 bytes (pthread_rwlock.cpp pthread_rwlock_internal_t:
 * init memsets all 40, the pending-writer serial sits at +32); glibc's i386 object is 32.
 * So the glibc object holds a pointer (word 0) to a heap bionic rwlock (libhybris's hooks.c
 * uses the same indirection in the other direction). PTHREAD_RWLOCK_INITIALIZER is all zero
 * (abi-check proves it), so pointer 0 means "statically initialised": the bionic lock is made
 * on first use and published with a CAS. glibc's writer-preferring static initialiser sets
 * __flags (+24); that preference is carried over. pthread_rwlockattr_* is not imported, so a
 * non-NULL attr cannot have been made by glibc code we know: refused loudly. */

static pthread_rwlock_t *rw_make(int prefer_writer) {
    pthread_rwlock_t *b = malloc(sizeof(*b));
    if (b == NULL) {
        nvgi_die("pthread_rwlock: out of memory for a bionic rwlock");
    }
    pthread_rwlockattr_t a;
    int r = pthread_rwlockattr_init(&a);
    if (r == 0 && prefer_writer) {
        r = pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    }
    if (r == 0) {
        r = pthread_rwlock_init(b, &a);
    }
    pthread_rwlockattr_destroy(&a);
    if (r != 0) {
        nvgi_die("pthread_rwlock: bionic refused to initialise a rwlock (error %d)", r);
    }
    return b;
}

static pthread_rwlock_t *rw_get(struct nvg_glibc_rwlock *g) {
    pthread_rwlock_t *b = __atomic_load_n((pthread_rwlock_t **)&g->bionic, __ATOMIC_ACQUIRE);
    if (b != NULL) {
        return b;
    }
    int prefer_writer = g->flags == NVG_GLIBC_RWLOCK_PREFER_WRITER_NONRECURSIVE;
    if (g->flags != 0 && !prefer_writer) {
        nvgi_die("pthread_rwlock %p: glibc __flags %u from a static initialiser this shim does not "
                 "know", (void *)g, g->flags);
    }
    pthread_rwlock_t *mine = rw_make(prefer_writer);
    pthread_rwlock_t *expected = NULL;
    if (__atomic_compare_exchange_n((pthread_rwlock_t **)&g->bionic, &expected, mine, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        nvgi_trace("pthread_rwlock %p: statically initialised, bionic rwlock %p made", (void *)g,
                   (void *)mine);
        return mine;
    }
    pthread_rwlock_destroy(mine);
    free(mine);
    return expected;
}

NVG_EXPORT int nvg_pthread_rwlock_init(struct nvg_glibc_rwlock *g, const void *attr) {
    if (attr != NULL) {
        nvgi_die("pthread_rwlock_init(%p, attr %p): glibc rwlock attributes are not translated "
                 "(no pthread_rwlockattr_* import exists to have made one)", (void *)g, attr);
    }
    memset(g, 0, sizeof(*g));
    __atomic_store_n((pthread_rwlock_t **)&g->bionic, rw_make(0), __ATOMIC_RELEASE);
    return 0;
}

NVG_EXPORT int nvg_pthread_rwlock_destroy(struct nvg_glibc_rwlock *g) {
    pthread_rwlock_t *b = __atomic_exchange_n((pthread_rwlock_t **)&g->bionic, NULL,
                                              __ATOMIC_ACQ_REL);
    if (b == NULL) {
        return 0; /* statically initialised and never used */
    }
    int r = pthread_rwlock_destroy(b);
    if (r != 0) {
        __atomic_store_n((pthread_rwlock_t **)&g->bionic, b, __ATOMIC_RELEASE); /* EBUSY: keep it */
        return r;
    }
    free(b);
    return 0;
}

NVG_EXPORT int nvg_pthread_rwlock_rdlock(struct nvg_glibc_rwlock *g) {
    return pthread_rwlock_rdlock(rw_get(g));
}

NVG_EXPORT int nvg_pthread_rwlock_wrlock(struct nvg_glibc_rwlock *g) {
    return pthread_rwlock_wrlock(rw_get(g));
}

NVG_EXPORT int nvg_pthread_rwlock_tryrdlock(struct nvg_glibc_rwlock *g) {
    return pthread_rwlock_tryrdlock(rw_get(g));
}

NVG_EXPORT int nvg_pthread_rwlock_trywrlock(struct nvg_glibc_rwlock *g) {
    return pthread_rwlock_trywrlock(rw_get(g));
}

NVG_EXPORT int nvg_pthread_rwlock_unlock(struct nvg_glibc_rwlock *g) {
    return pthread_rwlock_unlock(rw_get(g));
}
#endif
