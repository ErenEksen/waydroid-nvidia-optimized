/* glibc sigset_t (128 B) and struct sigaction (152 B, sa_flags after the mask)
 * marshalled to bionic's LP64 sigset64_t (8 B) and struct sigaction64. Signals
 * above 64 do not exist on Linux x86_64, so only the low 8 bytes carry data. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "nvglibc.h"
#include "constants.h"

static void g2b(const nvg_glibc_sigset_t *g, sigset64_t *b) {
    memcpy(b, g, sizeof(*b));
}

static void b2g(const sigset64_t *b, nvg_glibc_sigset_t *g) {
    memset(g, 0, sizeof(*g));
    memcpy(g, b, sizeof(*b));
}

NVG_EXPORT int nvg_sigaction(int sig, const struct nvg_glibc_sigaction *act,
                             struct nvg_glibc_sigaction *oact) {
    struct sigaction64 ba, bo;
    memset(&ba, 0, sizeof(ba));
    memset(&bo, 0, sizeof(bo));
    if (act != NULL) {
        if (act->sa_flags & NVG_BIONIC_SA_RESTORER) {
            nvgi_die("sigaction(%d): caller set SA_RESTORER (flags %#x); the restorer is libc-"
                     "internal and cannot be honoured", sig, act->sa_flags);
        }
        ba.sa_flags = act->sa_flags; /* SA_* values identical (gen-constants.py) */
        memcpy(&ba.sa_handler, &act->handler, sizeof(act->handler));
        g2b(&act->sa_mask, &ba.sa_mask);
    }
    int r = sigaction64(sig, act ? &ba : NULL, oact ? &bo : NULL);
    if (r == 0 && oact != NULL) {
        memset(oact, 0, sizeof(*oact));
        memcpy(&oact->handler, &bo.sa_handler, sizeof(oact->handler));
        b2g(&bo.sa_mask, &oact->sa_mask);
        /* Bionic reports its own restorer; strip it so that handing oact back
         * to sigaction (save/restore idiom) round-trips. */
        oact->sa_flags = bo.sa_flags & ~NVG_BIONIC_SA_RESTORER;
        oact->sa_restorer = NULL;
    }
    return r;
}

NVG_EXPORT int nvg_sigprocmask(int how, const nvg_glibc_sigset_t *set, nvg_glibc_sigset_t *old) {
    sigset64_t bs, bo;
    if (set != NULL) {
        g2b(set, &bs);
    }
    int r = sigprocmask64(how, set ? &bs : NULL, old ? &bo : NULL);
    if (r == 0 && old != NULL) {
        b2g(&bo, old);
    }
    return r;
}

NVG_EXPORT int nvg_pthread_sigmask(int how, const nvg_glibc_sigset_t *set,
                                   nvg_glibc_sigset_t *old) {
    sigset64_t bs, bo;
    if (set != NULL) {
        g2b(set, &bs);
    }
    int r = pthread_sigmask64(how, set ? &bs : NULL, old ? &bo : NULL);
    if (r == 0 && old != NULL) {
        b2g(&bo, old);
    }
    return r;
}

NVG_EXPORT int nvg_sigemptyset(nvg_glibc_sigset_t *set) {
    memset(set, 0, sizeof(*set));
    return 0;
}

NVG_EXPORT int nvg_sigfillset(nvg_glibc_sigset_t *set) {
    memset(set, 0xff, sizeof(*set));
    return 0;
}

NVG_EXPORT int nvg_sigaddset(nvg_glibc_sigset_t *set, int sig) {
    if (sig < 1 || sig > (int)(8 * sizeof(*set))) {
        errno = EINVAL;
        return -1;
    }
    unsigned bit = (unsigned)sig - 1;
    set->val[bit / (8 * sizeof(unsigned long))] |= 1UL << (bit % (8 * sizeof(unsigned long)));
    return 0;
}
