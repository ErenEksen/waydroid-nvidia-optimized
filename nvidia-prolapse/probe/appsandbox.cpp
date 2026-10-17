/* appsandbox: run a command the way zygote hands a process to an app.
 *
 *   appsandbox [--uid N] [--gid N] [--no-seccomp] -- CMD [ARGS...]
 *
 * In order: drop supplementary groups, setresgid/setresuid to an app uid (default 10200 =
 * u0_a200), set PR_SET_NO_NEW_PRIVS, install bionic's *app* seccomp-bpf filter, execve.
 *
 * The filter is bionic's own: the generated x86_64/x86 app policies (gen/<arch>_app_policy.cpp,
 * produced by bionic's libc/tools/genseccomp.py from SYSCALLS.TXT and the SECCOMP_*LIST
 * files at the pinned android13-release commit, see the Makefile) assembled exactly as
 * libseccomp_policy's _set_seccomp_filter(FilterType::APP) does: arch check with a jump to
 * the secondary (32-bit) half, load nr, policy, SECCOMP_RET_TRAP. A disallowed syscall is
 * therefore a SIGSYS, never a silent EPERM.
 *
 * The one thing zygote does not do is exec: zygote forks and the child *is* the app. We exec
 * so the thing under test is an unmodified binary; execve itself is in SYSCALLS.TXT and not
 * blocklisted for apps, so the filter admits it (the kernel evaluates the filter for the exec).
 *
 * Fails loudly at every step. Nothing here is Waydroid-specific. */
#include <errno.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <vector>

#include "seccomp_bpfs.h"

#if !defined(__x86_64__)
#error appsandbox is written for the x86_64 (dual-arch x86_64 + x86) app filter
#endif

static void die(const char *what) {
    fprintf(stderr, "appsandbox: %s: %s\n", what, strerror(errno));
    exit(90);
}

static void usage() {
    fprintf(stderr, "usage: appsandbox [--uid N] [--gid N] [--no-seccomp] -- CMD [ARGS...]\n");
    exit(90);
}

#define syscall_nr (offsetof(struct seccomp_data, nr))
#define arch_nr (offsetof(struct seccomp_data, arch))

typedef std::vector<sock_filter> filter;

static void Allow(filter &f) { f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)); }
static void Disallow(filter &f) { f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP)); }
static void ExamineSyscall(filter &f) { f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, syscall_nr)); }

/* Verbatim structure of bionic's libc/seccomp/seccomp_policy.cpp (android13-release). */
static size_t ValidateArchitectureAndJumpIfNeeded(filter &f) {
    f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, arch_nr));
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 2, 0));
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, 1, 0));
    Disallow(f);
    return f.size() - 2;
}

static void SetValidateArchitectureJumpTarget(size_t offset, filter &f) {
    size_t jump_length = f.size() - offset - 1;
    auto u8_jump_length = (__u8)jump_length;
    if (u8_jump_length != jump_length) {
        fprintf(stderr, "appsandbox: secondary-arch jump %zu does not fit in a byte\n", jump_length);
        exit(90);
    }
    f[offset] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_I386, u8_jump_length, 0);
}

static filter build_app_filter() {
    filter f;
    auto offset_to_secondary_filter = ValidateArchitectureAndJumpIfNeeded(f);
    ExamineSyscall(f);
    for (size_t i = 0; i < x86_64_app_filter_size; ++i) f.push_back(x86_64_app_filter[i]);
    Disallow(f);
    SetValidateArchitectureJumpTarget(offset_to_secondary_filter, f);
    ExamineSyscall(f);
    for (size_t i = 0; i < x86_app_filter_size; ++i) f.push_back(x86_app_filter[i]);
    Disallow(f);
    (void)Allow;
    return f;
}

int main(int argc, char **argv) {
    uid_t uid = 10200;
    gid_t gid = 10200;
    bool seccomp = true;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--uid") && i + 1 < argc) uid = (uid_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--gid") && i + 1 < argc) gid = (gid_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--no-seccomp")) seccomp = false;
        else usage();
    }
    if (i >= argc) usage();

    if (setgroups(0, nullptr) != 0) die("setgroups(0)");
    if (setresgid(gid, gid, gid) != 0) die("setresgid");
    if (setresuid(uid, uid, uid) != 0) die("setresuid");
    if (getuid() != uid || geteuid() != uid) { fprintf(stderr, "appsandbox: uid did not change\n"); return 90; }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("prctl(PR_SET_NO_NEW_PRIVS)");

    if (seccomp) {
        filter f = build_app_filter();
        struct sock_fprog prog = { static_cast<unsigned short>(f.size()), f.data() };
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) die("prctl(PR_SET_SECCOMP)");
        fprintf(stderr, "appsandbox: uid %u gid %u, app seccomp filter installed (%zu insns: x86_64 %zu + x86 %zu + glue), exec %s\n",
                (unsigned)uid, (unsigned)gid, f.size(), x86_64_app_filter_size, x86_app_filter_size, argv[i]);
    } else {
        fprintf(stderr, "appsandbox: uid %u gid %u, NO seccomp (--no-seccomp), exec %s\n", (unsigned)uid, (unsigned)gid, argv[i]);
    }
    execv(argv[i], &argv[i]);
    die("execv");
}
