/* Core initialisation, logging, front registration. */
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "nvglibc.h"

int nvgi_trace_on;
void *nvgi_lib_handle[3];
size_t nvgi_default_stack;
int nvgi_mutex_template[3];

static const char *const lib_names[3] = { "libc.so", "libm.so", "libdl.so" };

static void emit(int prio, const char *fmt, va_list ap) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "libnvglibc: ");
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    __android_log_write(prio, "libnvglibc", buf);
    size_t len = strlen(buf);
    if (len < sizeof(buf) - 1) {
        buf[len++] = '\n';
    }
    ssize_t r = write(STDERR_FILENO, buf, len);
    (void)r;
}

void nvgi_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(ANDROID_LOG_ERROR, fmt, ap);
    va_end(ap);
    abort();
}

void nvgi_trace(const char *fmt, ...) {
    if (!nvgi_trace_on) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    emit(ANDROID_LOG_INFO, fmt, ap);
    va_end(ap);
}

static int mutex_state_for(int type) {
    pthread_mutexattr_t a;
    pthread_mutex_t m;
    int r;
    if ((r = pthread_mutexattr_init(&a)) != 0 || (r = pthread_mutexattr_settype(&a, type)) != 0 ||
        (r = pthread_mutex_init(&m, &a)) != 0) {
        nvgi_die("init: bionic refused a type-%d mutex template (error %d)", type, r);
    }
    int state;
    memcpy(&state, &m, sizeof(state));
    pthread_mutex_destroy(&m);
    pthread_mutexattr_destroy(&a);
    return state;
}

static void core_init_once(void) {
    const char *t = getenv("NVGLIBC_TRACE");
    nvgi_trace_on = t != NULL && *t != '\0' && strcmp(t, "0") != 0;

    for (int i = 0; i < 3; i++) {
        /* libc and libdl are DT_NEEDED by the core: must already be loaded. */
        int flags = RTLD_NOW | (i == NVG_LIB_LIBM ? 0 : RTLD_NOLOAD);
        nvgi_lib_handle[i] = dlopen(lib_names[i], flags);
        if (nvgi_lib_handle[i] == NULL) {
            const char *e = dlerror();
            nvgi_die("init: dlopen(\"%s\") failed: %s", lib_names[i], e ? e : "(no dlerror)");
        }
    }

    /* glibc's default thread stack: RLIMIT_STACK soft limit, or 2 MiB
     * (ARCH_STACK_DEFAULT_SIZE) when unlimited; at least PTHREAD_STACK_MIN;
     * page-rounded. Bionic's default (1 MiB) would shrink driver threads. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0) {
        nvgi_die("init: getrlimit(RLIMIT_STACK) failed: %s", strerror(errno));
    }
    size_t sz = rl.rlim_cur == RLIM_INFINITY ? (size_t)2 << 20 : (size_t)rl.rlim_cur;
    if (sz < PTHREAD_STACK_MIN) {
        sz = PTHREAD_STACK_MIN;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) {
        nvgi_die("init: sysconf(_SC_PAGESIZE) failed");
    }
    sz = (sz + (size_t)pg - 1) & ~((size_t)pg - 1);
    nvgi_default_stack = sz;

    /* Bionic mutex state words for statically initialised glibc
     * recursive/errorcheck mutexes, produced by bionic itself. */
    int normal = mutex_state_for(PTHREAD_MUTEX_NORMAL);
    if (normal != 0) {
        nvgi_die("init: bionic's initialised normal mutex has state %#x, not 0: a zeroed glibc "
                 "mutex is not a bionic normal mutex on this system", normal);
    }
    nvgi_mutex_template[NVG_GLIBC_MUTEX_KIND_RECURSIVE] = mutex_state_for(PTHREAD_MUTEX_RECURSIVE);
    nvgi_mutex_template[NVG_GLIBC_MUTEX_KIND_ERRORCHECK] = mutex_state_for(PTHREAD_MUTEX_ERRORCHECK);
    for (int k = 1; k <= 2; k++) {
        if (nvgi_mutex_template[k] == 0) {
            nvgi_die("init: bionic's type-%d mutex state is 0; cannot convert glibc static "
                     "initialisers", k);
        }
    }
    nvgi_trace("core init: default stack %zu, mutex templates rec=%#x errchk=%#x", sz,
               nvgi_mutex_template[1], nvgi_mutex_template[2]);
}

static pthread_once_t core_once = PTHREAD_ONCE_INIT;

void nvgi_core_init(void) {
    pthread_once(&core_once, core_init_once);
}

__attribute__((constructor)) static void core_ctor(void) {
    nvgi_core_init();
}

/* ---- fronts ---- */

/* DATA rows copy one pointer from bionic's object into the front's pointer-sized
 * object (gen-fronts.py only allows stdin/stdout/stderr). */
_Static_assert(sizeof(stdin) == sizeof(void *) && sizeof(stdout) == sizeof(void *) &&
               sizeof(stderr) == sizeof(void *), "bionic FILE* objects are pointers");

#define MAX_FRONTS 16
static const struct nvg_front *fronts[MAX_FRONTS];
static unsigned nfronts;
static pthread_mutex_t fronts_lock = PTHREAD_MUTEX_INITIALIZER;

NVG_EXPORT void nvg_front_init(const struct nvg_front *f) {
    nvgi_core_init();
    if (f == NULL || f->magic != NVG_FRONT_MAGIC || f->abi != NVG_FRONT_ABI) {
        nvgi_die("front init: table %p has magic %#x abi %u, expected %#x abi %u (front and "
                 "core from different builds)", (const void *)f, f ? f->magic : 0,
                 f ? f->abi : 0, NVG_FRONT_MAGIC, NVG_FRONT_ABI);
    }
    for (unsigned i = 0; i < f->nsyms; i++) {
        const struct nvg_front_sym *s = &f->syms[i];
        if (s->addr == NULL) {
            nvgi_die("%s: table row \"%s\" has no definition address", f->soname, s->name);
        }
        if (s->kind == NVG_KIND_CORE) {
            continue;
        }
        if (s->lib > NVG_LIB_LIBDL || s->slot == NULL) {
            nvgi_die("%s: table row \"%s\" is malformed", f->soname, s->name);
        }
        void *p = dlsym(nvgi_lib_handle[s->lib], s->target);
        if (p == NULL) {
            nvgi_die("%s: bionic has no \"%s\" (needed for \"%s\") in %s", f->soname, s->target,
                     s->name, lib_names[s->lib]);
        }
        if (s->kind == NVG_KIND_SLOT) {
            *s->slot = p;
        } else if (s->kind == NVG_KIND_DATA) {
            *s->slot = *(void **)p;
        } else {
            nvgi_die("%s: table row \"%s\" has unknown kind %u", f->soname, s->name, s->kind);
        }
    }
    pthread_mutex_lock(&fronts_lock);
    for (unsigned i = 0; i < nfronts; i++) {
        if (fronts[i] == f) {
            pthread_mutex_unlock(&fronts_lock);
            nvgi_die("%s: front initialised twice", f->soname);
        }
    }
    if (nfronts == MAX_FRONTS) {
        pthread_mutex_unlock(&fronts_lock);
        nvgi_die("%s: more than %d fronts registered", f->soname, MAX_FRONTS);
    }
    __atomic_store_n(&fronts[nfronts], f, __ATOMIC_RELEASE);
    __atomic_store_n(&nfronts, nfronts + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&fronts_lock);
    nvgi_trace("front %s: %u symbols bound", f->soname, f->nsyms);
}

void *nvgi_front_lookup(const char *name) {
    unsigned n = __atomic_load_n(&nfronts, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; i++) {
        const struct nvg_front *f = __atomic_load_n(&fronts[i], __ATOMIC_ACQUIRE);
        for (unsigned j = 0; j < f->nsyms; j++) {
            if (strcmp(f->syms[j].name, name) == 0) {
                return f->syms[j].addr;
            }
        }
    }
    return NULL;
}

/* Every slot starts out pointing at a per-name stub in the front that lands
 * here, so a slot that front init never filled fails loudly on first call. */
NVG_EXPORT _Noreturn void nvg_unresolved_trap(const char *name, const char *soname) {
    nvgi_die("%s: \"%s\" called through an unfilled slot (front init did not run)", soname, name);
}
