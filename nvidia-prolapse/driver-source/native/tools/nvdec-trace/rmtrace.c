// LD_PRELOAD RM/UVM observer for capturing a stock driver's channel contract:
// RM allocs (class + params), controls, CPU/GPU mappings, and the method
// words of every GPFIFO entry of every channel (polled from USERD GPPut).
// Interfaces only: uses public open-gpu-kernel-modules headers. Output goes
// to $RMTRACE (text). Diagnostic tool for disposable host processes only.
// For video engines (NV2080 engine 0x13-0x16) it also dumps 8 KiB at the
// NVC9B0_SET_DRV_PIC_SETUP_OFFSET target (the codec pic_s descriptor).
// Stock 615 notes: libcuda/libnvcuvid allocate with NVOS64 and paramsSize 0
// (RM derives the size from the class); NV_ESC_RM_ALLOC_MEMORY returns a
// cookie in pMemory and the CPU mapping is the later mmap of the passed fd.
// Build (K = open-gpu-kernel-modules tree of the running driver version):
//   cc -shared -fPIC -O2 -Wall -Wno-attribute-alias -I$K/kernel-open/nvidia-uvm
//     -I$K/kernel-open/common/inc -I$K/src/common/sdk/nvidia/inc
//     -I$K/src/nvidia/arch/nvalloc/unix/include -o rmtrace.so rmtrace.c -ldl -pthread
// Run: RMTRACE=out.trace LD_PRELOAD=./rmtrace.so ffmpeg -hwaccel cuda -i in.webm -frames:v 3 -f null -
//      python3 methods.py out.trace   (decode the first large video pushbuffer)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <setjmp.h>
#include <signal.h>
#include "nvos.h"
#include "nv-ioctl.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "uvm_ioctl.h"
#include "alloc/alloc_channel.h"

#define MAXN 8192
static FILE *out;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int (*real_ioctl)(int, unsigned long, void *);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

struct cpu { uint32_t h; uint64_t addr, size; };          // hMemory -> CPU
struct gpu { uint32_t h; uint64_t va, off, len; };         // GPU VA -> hMemory
struct fdmap { int fd; uint32_t h; uint64_t off, len; };   // nvos33 pending
struct chan { uint32_t h, cls, engine, userd_h, entries; uint64_t gpfifo, userd_off; uint32_t last, skipped; };
static struct cpu cpus[MAXN]; static int ncpu;
static struct gpu gpus[MAXN]; static int ngpu;
static struct fdmap fds[256]; static int nfd;
static struct chan chans[64]; static int nchan;

static int is_gpfifo(uint32_t c) {
    switch (c) { case 0xa06f: case 0xa16f: case 0xb06f: case 0xc06f: case 0xc36f: case 0xc46f:
    case 0xc56f: case 0xc76f: case 0xc86f: case 0xc96f: case 0xca6f: return 1; }
    return 0;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000ull + t.tv_nsec; }
// Device (PFNMAP) mappings cannot go through process_vm_readv, so reads are
// done directly under a per-thread SIGSEGV/SIGBUS recovery point.
static __thread sigjmp_buf *volatile guard;
static struct sigaction old_segv, old_bus;
static void on_fault(int sig, siginfo_t *si, void *uc) {
    if (guard) siglongjmp(*guard, 1);
    struct sigaction *o = sig == SIGSEGV ? &old_segv : &old_bus;
    if (o->sa_flags & SA_SIGINFO) o->sa_sigaction(sig, si, uc);
    else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) o->sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}
static int safe_read(void *dst, const void *src, size_t n) {
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) { guard = NULL; return -1; }
    guard = &jb; __asm__ volatile("" ::: "memory");
    const volatile uint32_t *s4 = src; uint32_t *d4 = dst;
    if (((uintptr_t)src & 3) == 0 && (n & 3) == 0) for (size_t i = 0; i < n / 4; i++) d4[i] = s4[i];
    else memcpy(dst, src, n);
    guard = NULL;
    return 0;
}
static void hexdump(const char *tag, const void *p, size_t n) {
    const uint32_t *w = p;
    fprintf(out, "%s", tag);
    for (size_t i = 0; i < n / 4; i++) fprintf(out, "%s%08x", i % 8 ? " " : "\n  ", w[i]);
    fprintf(out, "\n");
}
static void *cpu_of_va(uint64_t va, uint64_t len) {
    for (int i = ngpu - 1; i >= 0; i--) {
        if (va < gpus[i].va || va + len > gpus[i].va + gpus[i].len) continue;
        for (int j = ncpu - 1; j >= 0; j--)
            if (cpus[j].h == gpus[i].h) {
                uint64_t o = gpus[i].off + (va - gpus[i].va);
                if (o + len <= cpus[j].size) return (void *)(uintptr_t)(cpus[j].addr + o);
            }
    }
    return NULL;
}
static void *cpu_of_mem(uint32_t h, uint64_t off, uint64_t len) {
    for (int j = ncpu - 1; j >= 0; j--)
        if (cpus[j].h == h && off + len <= cpus[j].size) return (void *)(uintptr_t)(cpus[j].addr + off);
    return NULL;
}
static uint32_t pbbuf[0x200000 / 4];
static void scan(void) {
    for (int c = 0; c < nchan; c++) {
        struct chan *ch = &chans[c];
        void *userd = cpu_of_mem(ch->userd_h, ch->userd_off, 0x100);
        uint32_t *ring = cpu_of_va(ch->gpfifo, (uint64_t)ch->entries * 8);
        uint32_t put;
        if (!userd || !ring || safe_read(&put, (char *)userd + 0x8c, 4)) {
            if (!ch->skipped++ || getenv("RMTRACE_DEBUG")) fprintf(out, "SKIP chan %#x userd %p ring %p\n", ch->h, userd, (void *)ring);
            continue;
        }
        if (getenv("RMTRACE_DEBUG") && ch->engine) {
            uint32_t u[64] = {0}, rw[4] = {0}; safe_read(u, userd, 256); safe_read(rw, ring, 16);
            fprintf(out, "DBG chan %#x put %u last %u ring %08x %08x %08x %08x\n", ch->h, put, ch->last, rw[0], rw[1], rw[2], rw[3]);
            hexdump("DBG userd", u, 256);
        }
        while (ch->last != put && put < ch->entries) {
            uint32_t ew[2];
            if (safe_read(ew, ring + ch->last * 2, 8)) break;
            uint64_t e = ew[0] | (uint64_t)ew[1] << 32;
            uint64_t va = e & 0xfffffffffcull, words = (e >> 42) & 0x1fffff;
            fprintf(out, "T %llu GPFIFO chan %#x class %#x engine %#x idx %u entry %016llx va %#llx words %llu\n",
                    (unsigned long long)now_ns(), ch->h, ch->cls, ch->engine, ch->last, (unsigned long long)e,
                    (unsigned long long)va, (unsigned long long)words);
            void *pb = words ? cpu_of_va(va, words * 4) : NULL;
            if (pb && words * 4 <= sizeof(pbbuf) && !safe_read(pbbuf, pb, words * 4)) {
                hexdump("PB", pbbuf, words * 4);
                // Video engines: also capture the picture-setup buffer named by
                // NVC9B0_SET_DRV_PIC_SETUP_OFFSET (0x404, 256-byte units).
                for (uint64_t i = 0; i < words && ch->engine >= 0x13 && ch->engine <= 0x16;) {
                    uint32_t h = pbbuf[i], op = h >> 29, n = (h >> 16) & 0x1fff, m = (h & 0xfff) << 2, sc = (h >> 13) & 7;
                    if (op == 4) { i++; continue; } // immediate
                    if (sc == 4 && m == 0x404 && n >= 1 && i + 1 < words) {
                        static uint32_t desc[0x2000 / 4];
                        uint64_t dva = (uint64_t)pbbuf[i + 1] << 8;
                        void *d = cpu_of_va(dva, sizeof(desc));
                        if (d && !safe_read(desc, d, sizeof(desc))) { fprintf(out, "PICSETUP va %#llx\n", (unsigned long long)dva); hexdump("DESC", desc, sizeof(desc)); }
                        else fprintf(out, "PICSETUP va %#llx unreadable\n", (unsigned long long)dva);
                    }
                    // Other buffer methods: is the target CPU-visible, and what
                    // does it hold at submit time (first 256 bytes + nonzero
                    // count over 64 KiB)?
                    if (sc == 4 && n >= 1 && i + 1 < words &&
                        (m == 0x408 || m == 0x4c8 || m == 0x4dc || (m >= 0x5c8 && m <= 0x5dc))) {
                        static uint8_t buf[0x10000];
                        uint64_t bva = (uint64_t)pbbuf[i + 1] << 8;
                        void *b = cpu_of_va(bva, sizeof(buf));
                        if (b && !safe_read(buf, b, sizeof(buf))) {
                            size_t nz = 0, last = 0;
                            for (size_t k = 0; k < sizeof(buf); k++) if (buf[k]) { nz++; last = k; }
                            fprintf(out, "BUF method %#x va %#llx nonzero %zu last %#zx\n", m, (unsigned long long)bva, nz, last);
                            hexdump("BUFHEAD", buf, 256);
                        } else fprintf(out, "BUF method %#x va %#llx not cpu-visible\n", m, (unsigned long long)bva);
                    }
                    i += 1 + n;
                }
            }
            else if (words) fprintf(out, "PB unreadable\n");
            ch->last = (ch->last + 1) % ch->entries;
        }
    }
    fflush(out);
}
static void *poller(void *arg) {
    (void)arg;
    for (;;) { pthread_mutex_lock(&lock); scan(); pthread_mutex_unlock(&lock);
        struct timespec t = {0, 20000}; nanosleep(&t, NULL); }
    return NULL;
}
// Keep our fault handler first; later SIGSEGV/SIGBUS installers are chained.
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if ((sig == SIGSEGV || sig == SIGBUS) && act && act->sa_sigaction != on_fault) {
        struct sigaction *o = sig == SIGSEGV ? &old_segv : &old_bus;
        if (oldact) *oldact = *o;
        *o = *act;
        return 0;
    }
    return real_sigaction(sig, act, oldact);
}
__attribute__((constructor)) static void init(void) {
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    const char *p = getenv("RMTRACE");
    out = fopen(p ? p : "/dev/stderr", "w");
    struct sigaction sa = {0}; sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    real_sigaction(SIGSEGV, &sa, &old_segv); real_sigaction(SIGBUS, &sa, &old_bus);
    pthread_t t; pthread_create(&t, NULL, poller, NULL); pthread_detach(t);
}
__attribute__((destructor)) static void fini(void) { pthread_mutex_lock(&lock); scan(); fprintf(out, "END\n"); fflush(out); pthread_mutex_unlock(&lock); }

void *mmap(void *a, size_t len, int prot, int flags, int fd, off_t off) {
    void *r = real_mmap(a, len, prot, flags, fd, off);
    if (r == MAP_FAILED || fd < 0) return r;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nfd; i++)
        if (fds[i].fd == fd) {
            if (ncpu < MAXN) cpus[ncpu++] = (struct cpu){fds[i].h, (uint64_t)(uintptr_t)r - fds[i].off, fds[i].off + len};
            fprintf(out, "MMAP h %#x cpu %p len %#zx (mem off %#llx)\n", fds[i].h, r, len, (unsigned long long)fds[i].off);
            memmove(&fds[i], &fds[i + 1], (--nfd - i) * sizeof(fds[0]));
            break;
        }
    pthread_mutex_unlock(&lock);
    return r;
}
void *mmap64(void *a, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    unsigned nr = req & 0xff, type = (req >> 8) & 0xff;
    if (type == NV_IOCTL_MAGIC && nr == NV_ESC_RM_FREE) { pthread_mutex_lock(&lock); scan(); pthread_mutex_unlock(&lock); }
    int r = real_ioctl(fd, req, arg);
    pthread_mutex_lock(&lock);
    if (type == NV_IOCTL_MAGIC) {
        if (nr == NV_ESC_RM_ALLOC) {
            // libcuda/libnvcuvid use the NVOS64 form (size selects the layout).
            uint32_t parent, obj, cls, size, status; uint64_t parms;
            if (_IOC_SIZE(req) == sizeof(NVOS64_PARAMETERS)) {
                NVOS64_PARAMETERS *p = arg; parent = p->hObjectParent; obj = p->hObjectNew; cls = p->hClass;
                size = p->paramsSize; status = p->status; parms = (uint64_t)(uintptr_t)p->pAllocParms;
            } else {
                NVOS21_PARAMETERS *p = arg; parent = p->hObjectParent; obj = p->hObjectNew; cls = p->hClass;
                size = p->paramsSize; status = p->status; parms = (uint64_t)(uintptr_t)p->pAllocParms;
            }
            fprintf(out, "ALLOC parent %#x new %#x class %#x size %u status %#x iosz %u\n", parent, obj, cls, size, status, (unsigned)_IOC_SIZE(req));
            // paramsSize 0: RM derives the size from the class (stock libcuda does this).
            uint32_t dump = size ? size : (is_gpfifo(cls) ? sizeof(NV_CHANNEL_ALLOC_PARAMS) : 64);
            static uint32_t pbuf[128];
            if (parms && dump && !safe_read(pbuf, (void *)(uintptr_t)parms, dump < 512 ? dump : 512))
                hexdump(size ? "  params" : "  params(implicit size, first bytes)", pbuf, dump < 512 ? dump : 512);
            if (is_gpfifo(cls) && status == 0 && nchan < 64 && parms) {
                NV_CHANNEL_ALLOC_PARAMS *c = (void *)(uintptr_t)parms;
                chans[nchan++] = (struct chan){obj, cls, c->engineType, c->hUserdMemory[0], c->gpFifoEntries, c->gpFifoOffset, c->userdOffset[0], 0, 0};
                fprintf(out, "CHANNEL %#x gpfifo %#llx entries %u userd h %#x off %#llx engine %#x ctxshare %#x vaspace %#x\n",
                        obj, (unsigned long long)c->gpFifoOffset, c->gpFifoEntries, c->hUserdMemory[0],
                        (unsigned long long)c->userdOffset[0], c->engineType, c->hContextShare, c->hVASpace);
            }
        } else if (nr == NV_ESC_RM_ALLOC_MEMORY) {
            nv_ioctl_nvos02_parameters_with_fd *w = arg; NVOS02_PARAMETERS *p = &w->params;
            fprintf(out, "ALLOC_MEMORY parent %#x new %#x class %#x flags %#x pMemory %#llx limit %#llx status %#x\n", p->hObjectParent, p->hObjectNew, p->hClass, p->flags,
                    (unsigned long long)p->pMemory, (unsigned long long)p->limit, p->status);
            // Linux returns pMemory as a cookie; the CPU address comes from the
            // caller's later mmap of w->fd (OS descriptors 0x71 are user memory).
            if (p->status == 0 && p->hClass == 0x71 && ncpu < MAXN) cpus[ncpu++] = (struct cpu){p->hObjectNew, (uint64_t)(uintptr_t)p->pMemory, p->limit + 1};
            else if (p->status == 0 && w->fd >= 0 && nfd < 256) fds[nfd++] = (struct fdmap){w->fd, p->hObjectNew, 0, p->limit + 1};
            fprintf(out, "  alloc_memory fd %d\n", w->fd);
        } else if (nr == NV_ESC_RM_CONTROL) {
            NVOS54_PARAMETERS *p = arg;
            fprintf(out, "CONTROL obj %#x cmd %#x size %u status %#x\n", p->hObject, p->cmd, p->paramsSize, p->status);
            if (p->params && p->paramsSize && p->paramsSize <= 256) hexdump("  params", (void *)(uintptr_t)p->params, p->paramsSize);
        } else if (nr == NV_ESC_RM_MAP_MEMORY) {
            nv_ioctl_nvos33_parameters_with_fd *w = arg;
            fprintf(out, "MAP_MEMORY h %#x off %#llx len %#llx flags %#x fd %d status %#x\n", w->params.hMemory, (unsigned long long)w->params.offset,
                    (unsigned long long)w->params.length, w->params.flags, w->fd, w->params.status);
            if (w->params.status == 0 && nfd < 256) fds[nfd++] = (struct fdmap){w->fd, w->params.hMemory, w->params.offset, w->params.length};
        } else if (nr == NV_ESC_RM_MAP_MEMORY_DMA) {
            NVOS46_PARAMETERS *p = arg;
            fprintf(out, "MAP_DMA dma %#x h %#x off %#llx len %#llx flags %#x va %#llx status %#x\n", p->hDma, p->hMemory, (unsigned long long)p->offset,
                    (unsigned long long)p->length, p->flags, (unsigned long long)p->dmaOffset, p->status);
            if (p->status == 0 && ngpu < MAXN) gpus[ngpu++] = (struct gpu){p->hMemory, p->dmaOffset, p->offset, p->length};
        } else if (nr == NV_ESC_RM_FREE) {
            NVOS00_PARAMETERS *p = arg;
            fprintf(out, "FREE parent %#x obj %#x\n", p->hObjectParent, p->hObjectOld);
            for (int i = 0; i < ncpu; i++) if (cpus[i].h == p->hObjectOld) cpus[i--] = cpus[--ncpu];
            for (int i = 0; i < ngpu; i++) if (gpus[i].h == p->hObjectOld) gpus[i--] = gpus[--ngpu];
            for (int i = 0; i < nchan; i++) if (chans[i].h == p->hObjectOld) chans[i--] = chans[--nchan];
        }
    } else if (req == UVM_MAP_EXTERNAL_ALLOCATION) {
        UVM_MAP_EXTERNAL_ALLOCATION_PARAMS *p = arg;
        fprintf(out, "UVM_MAP h %#x base %#llx len %#llx off %#llx status %#x\n", p->hMemory, (unsigned long long)p->base, (unsigned long long)p->length,
                (unsigned long long)p->offset, p->rmStatus);
        if (r == 0 && p->rmStatus == 0 && ngpu < MAXN) gpus[ngpu++] = (struct gpu){p->hMemory, p->base, p->offset, p->length};
    }
    if (type == NV_IOCTL_MAGIC) scan();
    fflush(out);
    pthread_mutex_unlock(&lock);
    return r;
}
