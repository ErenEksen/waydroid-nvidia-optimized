# libnvglibc — glibc-compat shims for NVIDIA's userspace inside bionic

Status: P1 design, frozen 2026-10-10; **implemented and verified in the guest the same day** (compute
dispatch correct, process visible to nvidia-smi). Deviations found during implementation are marked
*as built*. Implementation lives in this directory.
Everything here is derived from measured facts (readelf of the blobs, the
android13-release bionic linker source, NDK r30 API-33 bionic stubs). Do not
"improve" a mechanism without re-checking the fact it rests on.

## What this is

NVIDIA's Linux Vulkan ICD closure (`libEGL_nvidia.so.0`, `libnvidia-glsi`,
`libnvidia-eglcore`, `libnvidia-gpucomp`, `libnvidia-glvkspirv`) is glibc-linked.
We load those files **unmodified** into a bionic process. Their DT_NEEDED and
verneed name six glibc SONAMEs; every imported symbol is versioned
(`malloc@GLIBC_2.2.5`). We provide those six libraries ourselves, exporting
every imported name **unversioned**, forwarding to bionic.

## Linker facts the design depends on (android13-release, `linker/`)

1. `soinfo_do_lookup`: candidates are the namespace's **global group first**, then
   the dlopen root's local group in BFS order (`linker_soinfo.cpp:60-79`).
2. For a versioned reference, each candidate DSO is asked for the version *name*:
   `find_verdef_version_index` (`linker.cpp:2644`) returns that DSO's verdef index
   if the DSO defines a version of that name, else `kVersymGlobal` (=1).
   `check_symbol_version` (`linker_soinfo.cpp:108`) then accepts a definition iff
   `verneed == verdef` (or anything, if the DSO has no `.gnu.version` table).
   → `malloc@GLIBC_2.2.5` binds to bionic libc.so **never** (libc.so has no
   `GLIBC_2.2.5`, so verneed→1, but its `malloc` is `LIBC`=2). It binds to our
   **unversioned** `malloc` (verdef 1).
3. `VersionTracker::init_verneed` (`linker.cpp:~2570`) requires every verneed file
   name (`libc.so.6`, …) to be the **DT_SONAME of a direct DT_NEEDED child**.
   → our six libraries must carry exactly those SONAMEs and be loaded as the
   blobs' dependencies (found by SONAME among already-loaded libs, or by search path).
4. `dlsym(handle, name)` searches the handle's library and its DT_NEEDED closure.
5. Bionic has no lazy binding; everything resolves at load.

## Architecture: one core, six fronts

```
blob ──malloc@GLIBC_2.2.5──▶ libc.so.6        (FRONT, pure asm, SONAME libc.so.6)
                               malloc: jmp *slot_malloc(%rip)      ; identical → bionic malloc
                               __xstat: jmp nvg___xstat@PLT        ; translate/implement → core
                               stdin:  .quad 0  (filled at init)   ; data symbol
                               DT_NEEDED libnvglibc.so ; .init_array → nvg_front_init(...)
                                   │
                                   ▼
                             libnvglibc.so (CORE, C, normal NDK library)
                               defines ONLY nvg_* symbols; never a glibc name
                               resolves slots with dlsym(handle_of_bionic_lib, name)
                               translators, implementations, fail-loud init
                                   │ normal versioned refs (write@LIBC, dlsym@LIBC …)
                                   ▼
                             bionic libc.so / libm.so / libdl.so
```

Fronts: `libc.so.6`, `libm.so.6`, `libpthread.so.0`, `librt.so.1`, `libdl.so.2`,
`ld-linux-x86-64.so.2`. (`libgcc_s.so.1` only when rtcore is wanted; not in P1.)

### Why the core defines no glibc name

A library that defines `dlsym` and also *calls* `dlsym` would, in the local group, bind
to itself (fact 2: a DSO with no `GLIBC_*` verdef matches a `GLIBC_*`-versioned ref with
any unversioned definition). So all glibc-named definitions live in the fronts (assembly,
no C runtime), and the core — which does call bionic by name — defines none of them.

### Why every front carries dummy `LIBC*` version definitions

The core's references are versioned by lld against the NDK stubs (`dlsym@LIBC`,
`dlvsym@LIBC_N`, `sigaction64@LIBC_P`, `__tls_get_addr@LIBC_Q`, …). If a front defined
`dlsym` unversioned and had no `LIBC` verdef, the core's `dlsym@LIBC` could match the
front's trampoline (fact 2) should a front ever precede bionic in a lookup list. So each
front's version script defines **one dummy symbol under each bionic version name**
(`LIBC LIBC_N LIBC_O LIBC_P LIBC_Q LIBC_R LIBC_S LIBC_T LIBC_DEPRECATED LIBC_PLATFORM
LIBC_PRIVATE`), e.g. `LIBC { nvg_v_LIBC; };`. Then for a `@LIBC` reference the front's
verdef index for `LIBC` is ≥2 while its real exports are verdef 1 → no match. The blobs'
`@GLIBC_*` references still see verneed→1 and match. This makes binding **independent of
load order**; it is not a fallback, it is the mechanism. (lld rejects an anonymous version
node combined with named ones, so the real exports are simply *not listed* in the version
script; unlisted symbols stay unversioned-global.)

### Trampolines

- identical name: `name: jmp *nvg_slot_<name>(%rip)` (6 bytes). The slot is a hidden
  8-byte `.data` word in the front (*as built:* initialised to a per-name stub that calls
  `nvg_unresolved_trap(name, soname)`, so a slot that init never filled aborts by name). A tail jump preserves the full call frame, so
  varargs (`sscanf`, `printf`), sret and all register args pass through untouched.
- renamed identical (`__errno_location`→`__errno`, `strerror_r`→`__gnu_strerror_r`,
  `_IO_getc`→`getc`, `_IO_putc`→`putc`, `__isoc99_sscanf`→`sscanf`,
  `__isoc99_fscanf`→`fscanf`, `__pthread_key_create`→`pthread_key_create`,
  `__strdup`→`strdup`, `__<x>_l`→`<x>_l` for the locale set when the constants agree —
  see "Generated constant tables"): same trampoline, slot resolved under the target name.
- translate/implement: `name: jmp nvg_<name>@PLT` into the core.
- data: `stdin`, `stdout`, `stderr`: `.globl`, `.type @object`, `.size 8`, in `.data`,
  filled at init with `*(FILE**)dlsym(h_libc, "stdin")`.
- `_setjmp`/`longjmp`: implemented in the core in assembly (`nvg__setjmp`,
  `nvg_longjmp`); the front jumps to them. glibc `jmp_buf` is 200 B; we use the first
  64 B (rbx rbp r12 r13 r14 r15 rsp rip). `_setjmp` does not save the signal mask;
  `longjmp(env, 0)` returns 1.

### Front init

Each front has an `.init_array` entry (asm) that calls
`nvg_front_init(const struct nvg_front *f)` in the core, where the generated table
holds `{slot*, bionic_name, which_lib}` rows plus the data slots. The core's own
constructor (runs first: fronts DT_NEED it) does
`h_libc = dlopen("libc.so", RTLD_NOW|RTLD_NOLOAD)`, same for `libm.so`, `libdl.so`
(`RTLD_NOW` without NOLOAD for libm), and resolves `getrlimit(RLIMIT_STACK)` for the
thread default (below). `nvg_front_init` fills every slot with `dlsym(h_<lib>, name)`;
a NULL result **aborts** with `libnvglibc: <front>: bionic has no "<name>"` on stderr and
logcat (`__android_log_write` via liblog if available, else `write(2)`). Never a silent
NULL slot: an unfilled slot points at `nvg_unresolved_trap`, which prints the slot's
name and aborts, so even a missed init fails loudly on first call.

## Translators and implementations (the core, C)

All struct layouts of glibc are declared **locally** in `glibc_abi.h` with
`_Static_assert` on `sizeof` and `offsetof` (we cannot include glibc headers in an
NDK build). Values come from the glibc headers of the host and are re-checked by
the host-side `tools/abi-check.c` (compiled with the host glibc, prints the same
numbers; the build compares).

| glibc import | implementation |
|---|---|
| `__xstat(ver,p,b)` `__xstat64` `__lxstat` `__lxstat64` `__fxstat(ver,fd,b)` `__fxstat64` | `ver != 1` → abort(`_STAT_VER`); else `stat/lstat/fstat` via slot. `struct stat` is the kernel struct in both |
| `__xmknod(ver,p,mode,dev_t*)` | `ver != 0` → abort; `mknod(p, mode, *dev)` |
| `sigaction` | glibc `struct sigaction` {handler@0, sa_mask[128]@8, sa_flags@136, sa_restorer@144} 152 B ↔ bionic `struct sigaction64` {sa_flags@0, handler@8, sa_mask64@16, sa_restorer@24}. Copy low 8 B of the mask. `SA_RESTORER` set by the caller → abort (glibc internal). Convert `oldact` back (zero the upper 120 B of the mask) |
| `sigprocmask` `pthread_sigmask` | glibc 128-B set ↔ `sigset64_t`: low 8 B; → `sigprocmask64`/`pthread_sigmask64` |
| `sigemptyset` `sigfillset` `sigaddset` `sigdelset` `sigismember` | pure C on the 128-B set (bit n-1 ↔ signal n); `sig<1 || sig>1024` → EINVAL |
| `pthread_mutexattr_{init,destroy,settype,setprotocol}` | glibc attr 4 B ↔ bionic `long`: widen to a local `pthread_mutexattr_t`, call, narrow back (bionic stores type/protocol/shared in the low 16 bits). Constants identical |
| `pthread_condattr_{init,destroy,setclock}` | same 4↔8 widening (**these ARE imported**; the P0 contract wrongly said not) |
| `pthread_cond_init(c, attr)` | attr NULL → pass; else widen |
| `pthread_mutex_init(m, attr)` | widen attr; bionic init zeroes the 40 B so glibc's `__kind` (+16) is 0 afterwards |
| `pthread_mutex_{lock,trylock,unlock,destroy}` | **static non-normal initialiser conversion**: glibc `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP` etc. leave bytes 0..15 zero and set `__kind` at +16 (1 recursive, 2 errorcheck, 3 adaptive=normal). Bionic keeps all state in the first 4 B and never reads +16. On every lock/trylock/unlock: if `*(int*)(m+16) & 3` is 1 or 2 **and** `*(int*)m == 0` → `CAS((int*)m, 0, template_state[kind])`, where `template_state` was produced once at init by bionic `pthread_mutex_init` with the matching type attr (never hand-encode bionic's state bits). Count conversions; print the count at exit when `NVGLIBC_TRACE` is set (answers plan §7.1). Race-free because every lock path runs this check before bionic sees the mutex |
| `sysconf(id)` | glibc→bionic id table generated from both headers (see below); unknown id → abort with the id. eglcore calls `sysconf(30)`=`_SC_PAGESIZE` |
| `mallinfo` | bionic `struct mallinfo` (10×size_t) → glibc (10×int), returned by value (sret) |
| `strerror_r` | trampoline to `__gnu_strerror_r` (the glibc default is the GNU `char*` variant) |
| `dlvsym(h, name, ver)` | *as built:* `ver` starts with `GLIBC_` → look the name up in the **fronts' own export tables** and return that trampoline (never bionic's same-named function: for `RTLD_DEFAULT` bionic's dlsym searches the global group first and would hand back e.g. bionic's `sigaction` with bionic's ABI); not exported by any front → NULL. Other versions → bionic `dlvsym`; `RTLD_NEXT` aborts |
| `dlopen(name, flags)` | strip `RTLD_DEEPBIND` (0x8, bionic lacks it; trace it); any other non-glibc bit → abort (bionic rejects them, linker.cpp:2087); forward; on NULL trace name+flags (*as built:* without calling `dlerror()`, which would consume the message the blob reads next) and return NULL (eglcore probes optional libs; a hard error here is wrong). Guest run: tegrahv, drm, dbus, rtcore probed and tolerated |
| `dladdr1(addr, info, extra, flags)` | `dladdr`; `flags==RTLD_DL_LINKMAP(2)`: `*extra` = a cached synthetic `struct link_map{l_addr,l_name,l_ld,l_next,l_prev}` keyed by `dli_fbase`, filled from `dl_iterate_phdr` (`dlpi_addr`, `dlpi_name`, PT_DYNAMIC); `RTLD_DL_SYMENT(1)` → abort (unused per Ghidra) |
| `gnu_get_libc_version` | returns `getenv("NVGLIBC_GLIBC_VERSION")`; *as built (P2):* unset → reads the file `host-glibc-version` next to `libnvglibc.so` (located with `dladdr`), written by whatever installs the shims from the host; neither → abort with the remedy. Never a built-in default. An app process has no environment we control, so the file is the shipping path |
| `dgettext(domain,msgid)` `bind_textdomain_codeset` | return `msgid` / `NULL` |
| `getenv(name)` | *as built (P7):* the real environment first, unchanged (a name set there, even to "", wins); only for a name **absent** from it, the value from the sidecar `nvglibc-default-env` next to `libnvglibc.so` (`NAME=VALUE` lines, `#` comments, `${ORIGIN}` = that directory; parsed once; malformed file aborts with file:line; missing file = no defaults, traced). Only the blobs bind to it (they import `getenv@GLIBC_2.2.5` and no `secure_getenv`/`environ`, measured); the app's own `getenv` is bionic's and untouched, and nothing writes the environment. Used for `__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES`, which libEGL_nvidia reads with `getenv` at its first platform scan (LD_PRELOAD logger on 615.71.09), so the driver finds `nvidia-prolapse/egl`'s Android platform in an app process. `core/defenv.c`, host test `tools/defenv-test.c` (`make test`) |
| `pthread_setaffinity_np(t, size, set)` | `sched_setaffinity(pthread_gettid_np(t), size, set)`; return errno value (glibc returns the error, doesn't set errno) |
| `pthread_attr_init` | bionic init, then `pthread_attr_setstacksize(attr, nvg_default_stack)` where `nvg_default_stack` = `RLIMIT_STACK` soft limit if finite else 2 MiB, page-rounded (glibc semantics; bionic's default is 1 MiB and would silently shrink driver threads) |
| `pthread_create(t, attr, fn, arg)` | `attr == NULL` → use a local attr initialised as above |
| `__newlocale(mask, name, base)` | *as built:* `LC_*_MASK` bits are identical, **but** glibc's `newlocale` treats `mask == 1 << LC_ALL` (64) as all categories (locale/newlocale.c) and libstdc++'s `_S_create_c_locale` passes exactly that; bionic rejects it (EINVAL) → map 64 → `LC_ALL_MASK`. `__duplocale` `__freelocale` `__uselocale` stay renamed trampolines |
| `__nl_langinfo_l(item, loc)` | glibc `nl_item` is `(category<<16)|index`, bionic is a sequential enum → generated table; unknown → abort with the item |
| `__strtod_l` `__strtof_l` `__strcoll_l` `__strxfrm_l` `__strftime_l` `__wcscoll_l` `__wcsxfrm_l` `__wcsftime_l` `__iswctype_l` `__wctype_l` `__towlower_l` `__towupper_l` `__duplocale` `__freelocale` `__uselocale` | renamed trampolines to the POSIX `*_l` names (bionic exports them) |

Everything else in the minimal closure is an identical trampoline. The authoritative
per-name list is `dispositions.tsv` (generated from the per-import table
`imports-615.71.09.tsv`, notes stripped): columns `name  disposition  target`.

## Generated constant tables

`tools/gen-constants.py` extracts the numeric value of each constant from **both** C
libraries' headers without running any bionic code: it emits a probe source of
`#ifdef X` / `"X" (X)` / `#endif` lines (glibc's `_SC_*` are enum constants but glibc also
`#define`s each to itself, so `#ifdef` works), preprocesses it once with the host `gcc -E -P
-D_GNU_SOURCE` (glibc) and once with the NDK `x86_64-linux-android33-clang -E -P` (bionic),
and evaluates the resulting integer expressions in Python (`-I`). Output:
`gen/constants.h` with `nvg_sc_map[]`, `nvg_nl_item_map[]`, etc., plus a report of which
families are identical. Tables: `_SC_*` (glibc `bits/confname.h` ∩ bionic `bits/sysconf.h`), `LC_*_MASK`,
`nl_item` names (`langinfo.h`), `RTLD_*`, `SA_*`, `PTHREAD_MUTEX_*`, `PTHREAD_PRIO_*`,
`CLOCK_*`. Any constant that differs and is passed through an "identical" trampoline is a
build error (the generator cross-checks the identical list against the table of names that
take such constants: `sysconf`, `newlocale`, `nl_langinfo_l`, `dlopen`, `sigaction`,
`pthread_mutexattr_settype`, `pthread_mutexattr_setprotocol`, `pthread_condattr_setclock`,
`clock_gettime`).

## Build (`make`, NDK r30, API 33, x86_64)

Inputs: the per-branch reference import lists `imports/<version>-<arch>.tsv` (see "Driver
branches"), the installed blobs when the driver is loaded (`tools/find-driver.py`: found through
the loader cache; the version is read from `/proc/driver/nvidia/version`, never typed), `dispositions.tsv`,
the NDK API-33 stubs.

1. `tools/import-census.py --reference imports/… BLOBS…` → per-front name sets, the union over every
   reference list and blob, from each blob's
   `.gnu.version_r` (verneed **file**, by versym index) and dynamic symbol table.
   A name in the blobs that is not in `dispositions.tsv` is a **build error**
   ("undispositioned import X in Y") — this is the G0 invariant enforced for future
   driver versions. The names EGL_nvidia/eglcore resolve at runtime by `dlvsym` on
   `libpthread.so.0` that are in no import table (`pthread_equal pthread_setname_np
   pthread_getcpuclockid sem_timedwait pthread_rwlock_tryrdlock pthread_rwlock_trywrlock
   pthread_setaffinity_np pthread_exit sem_wait`; the last two were found by the first guest run)
   are added to `libpthread.so.0` from `dispositions.tsv` (column `runtime=libpthread.so.0`). The
   candidate set is the blob's `pthread_*`/`sem_*` string table; a missing one aborts the blob with
   "Unable to find symbol".
2. `tools/gen-fronts.py` → `gen/<front>.S` + `gen/<front>.map` + `gen/<front>_table.c`.
3. Core: `clang -shared -fPIC -fvisibility=hidden -O2 -Wall -Werror -Wl,-z,defs
   -Wl,--no-undefined -Wl,-soname,libnvglibc.so` with `NVG_EXPORT` on the `nvg_*` API.
   Links `-ldl -llog`.
4. Fronts: `clang -shared -nostdlib -nostartfiles -Wl,-soname,<SONAME>
   -Wl,--version-script=gen/<front>.map -Wl,-z,defs -L out -lnvglibc gen/<front>.S
   gen/<front>_table.c -o out/<SONAME>`. The table `.c` is compiled with
   `-ffreestanding -fno-builtin -fvisibility=hidden` and contains only data.
5. **Artifact checks (the build fails otherwise):**
   - `llvm-readelf -d`: SONAME exact; fronts' NEEDED == {libnvglibc.so} only.
   - `llvm-readelf --dyn-syms`: every name in the front's set is GLOBAL DEFAULT and
     unversioned (no `@`); `stdin/stdout/stderr` are OBJECT size 8; nothing UND except
     `nvg_*`.
   - core: no defined dynamic symbol outside `nvg_*`; `-Wl,--version-script` not used.
   - each front's `.gnu.version_d` contains all eleven `LIBC*` names.
   - every bionic target name exists in the NDK API-33 stub (`libc.so`/`libm.so`/
     `libdl.so` `llvm-nm -D --defined-only`).
6. `make check-abi`: host `tools/abi-check.c` prints glibc sizes/offsets; compare with
   `glibc_abi.h` constants.

## Driver branches (one build for 535–615)

The closure differs per branch (measured with `readelf -d` and the dlopen strings of each branch's libraries,
535.309.01, 550.163.01, 570.211.01, 580.178.04, 595.99.02, 610.57.04, 615.78.08): `libEGL_nvidia.so.0` (the ICD)
dlopens `libnvidia-eglcore`, which dlopens `libnvidia-glvkspirv`; every `libnvidia-*.so.<version>` in their
DT_NEEDED belongs to the closure too: `libnvidia-glsi` everywhere, `libnvidia-gpucomp` from 550 on (535's eglcore
neither needs nor names it). `tools/find-driver.py` derives the closure that way from the installed libraries, so
a library the branch has is never left out and one it lacks is never asked for.

The glibc imports differ too. Each branch's list is cut from its own libraries (`import-census.py --allow-unused
--out imports/<version>-<arch>.tsv <closure>`; only the names, fronts and versions are kept) and the build exports
the union, so one set of fronts serves every branch: a name only an older branch imports is an export the newer
blobs never reference, which costs one slot. A table row no list imports is still an error (the table stays exact
for the union). Rows the older branches added, by disposition:
- identical: `dup3 execle execvp fscanf iscntrl __isinf __isnan link perror rmdir scanf sem_trywait setenv strsep
  system pthread_kill pthread_attr_setdetachstate pthread_attr_setschedparam pthread_attr_setschedpolicy`, and on
  x86_64 `fstatfs getpwnam getpwuid` (struct statfs/passwd identical there); i386 `__register_frame
  __deregister_frame` (bionic `@LIBC_R`);
- translate: `__getdelim` → `getdelim`; `pthread_attr_setinheritsched` (glibc `PTHREAD_INHERIT_SCHED` 0 /
  `PTHREAD_EXPLICIT_SCHED` 1, bionic the other way round; gen-constants reports the family as differing);
  i386 `fstatfs` (the non-LFS statfs translator on an fd), `getpwnam`/`getpwuid` (glibc's i386 struct passwd in a
  static, pointing at bionic's static strings);
- implement: `gettext` (returns msgid, as `dgettext`).
The names eglcore resolves by `dlvsym` on `libpthread.so.0` (its `pthread_*`/`sem_*` strings) are covered for every
branch by the union. 470 and 390 are out of scope (their Vulkan has no dma-buf/modifier extensions; 470 imports
SysV shared memory, 390 has no glvkspirv).

A host driver is checked against a finished build with `import-census.py --covered-by gen/census.tsv <closure>`:
every (front, name) it imports must be exported, else the check names each missing import. The package's setup
runs it before it stages glibc-shim mode, so a driver release newer than the reference lists is refused there, by
name, instead of aborting inside an app.

## Runtime contract (what the loader must do)

- Load order does not matter for correctness (see version-definition trick), but the
  intended order is: core, fronts, then the ICD. In P1 `nvprobe` dlopens the fronts by
  absolute path first (so later DT_NEEDED lookups hit them by SONAME), then
  `libEGL_nvidia.so.0`. `LD_LIBRARY_PATH` holds the blob dir and the shim dir so
  eglcore's own `dlopen("libnvidia-glvkspirv.so.<ver>")` resolves. *P2 (as built):* the
  shims, the blobs and the ICD soname symlink sit in one directory that is a search path
  of the loader's namespace (on Waydroid/LOS20: `/vendor_extra/lib64`, a stock sphal path);
  the ICD is dlopened by soname (`android_load_sphal_library`), DT_NEEDED glibc names
  resolve to the fronts by soname, and the core's own dlopens are attributed by bionic to
  the core's namespace, so bare names work with no shim change.
- Sidecars next to `libnvglibc.so`: `host-glibc-version` (required when eglcore asks) and
  `nvglibc-default-env` (optional; the blobs' default environment, see the `getenv` row).
- Env: `NVGLIBC_GLIBC_VERSION` (optional override of the `host-glibc-version` sidecar),
  `NVGLIBC_TRACE=1` (optional: logs dlopen attempts, mutex conversions, the glibc-version
  source, unknown-constant paths before aborting).
- Fail loud, named: every abort message starts with `libnvglibc:` and names the symbol,
  id or file, plus the remedy when there is one.

## 32-bit x86 (i686; P6, as built 2026-10-11)

`make ARCH=x86` builds the same design for the image's 32-bit processes (32-bit apps, ARM32 apps
through the ARM translator's x86 side, mediaserver) from the host's `lib32-nvidia-utils` (the same
five-library closure, i386) into `out32/`; it lands in the guest's `/vendor_extra/lib`. The x86_64
artifacts are byte-identical to the pre-P6 build (checked by sha256). Facts the i386 build rests on,
all measured (readelf/objdump of the 615.71.09 lib32 blobs, `tools/abi-check.c` with `gcc -m32`
against lib32-glibc, the NDK i686 headers, bionic android13-release source, the guest's linker):

- **Census**: 372 distinct imported names (`tools/import-census.py --arch x86`); 370 are names the
  x86_64 closure also imports. New: `___tls_get_addr@GLIBC_2.3` (ld-linux.so.2: i386 GNU TLS, regparm
  argument in `%eax`; bionic exports it `@LIBC_Q`) and `_Unwind_Find_FDE@GCC_3.0` (libc.so.6; bionic
  `@LIBC_R`). The same nine `pthread_*`/`sem_*` names are resolved at runtime by `dlvsym`.
- **TLS**: no static TLS. Only `R_386_TLS_DTPMOD32` (general dynamic) and `___tls_get_addr` calls;
  no `R_386_TLS_TPOFF*`, no `%gs:` access at all in the five blobs (no stack-guard or TCB reads).
- **Text relocations**: `libnvidia-eglcore` i386 is non-PIC (`DT_TEXTREL`; 134,390
  `R_386_RELATIVE`, 17,608 `R_386_PC32` and 78 `R_386_32` in `.text`, 11,593 of them calls to
  `___tls_get_addr`). Stock bionic refuses TEXTREL in 32-bit processes targeting API >= 23
  (linker.cpp `link_image`). Waydroid's images carry `base-patches-33/bionic/0003-linker-Allow-text-relocs-for-x86-only`
  + `0005-We-need-text-relocations` (waydroid/android_vendor_waydroid), which remove that refusal on
  x86; measured in the guest with a synthetic i686 TEXTREL library at target SDK 10000, as root and
  as uid 10200 under the app filter. On an image without that patch the 32-bit closure cannot load.
  Cost: eglcore's text pages are relocated privately per process.
- **Trampolines**: an i386 PLT entry needs `%ebx` = the calling object's GOT, and eglcore calls
  imports directly from non-PIC code with any `%ebx`. So no front uses `@PLT`: each trampoline is
  `call __nvgfront_pc_ecx; addl $_GLOBAL_OFFSET_TABLE_, %ecx; jmp *slot@GOTOFF(%ecx)` (or
  `jmp *nvg_<name>@GOT(%ecx)` into the core). `%ecx` is caller-saved and is not an argument register
  for cdecl or for the regparm(1) `___tls_get_addr`. Data objects and init_array entries are 4 bytes.
- **Per-arch dispositions**: `dispositions-x86.tsv` overrides rows of `dispositions.tsv` by name.
  Base rows the i386 closure does not import (`__tls_get_addr dl_iterate_phdr isprint truncf`) are
  reported, not errors; every override row must be imported or a runtime name.
- **ABI classes x86_64 never had** (sizes glibc i386 / bionic LP32, each with its translator):
  - non-LFS `struct stat` 88/96 with `_STAT_VER` 3 (`push $3` at the blobs' call sites; glibc
    `x86/xstatver.h`): `__xstat/__lxstat/__fxstat` convert from bionic `stat64` with glibc's
    EOVERFLOW rule; the `*64` variants are layout-identical (96/96) and pass through. `__xmknod`:
    `_MKNOD_VER` 1, glibc's 64-bit `dev_t` narrowed to bionic's 32-bit one (EOVERFLOW if it cannot).
  - non-LFS `struct dirent` 268/276 (different offsets): `readdir` converts into a per-`DIR`
    buffer freed by `closedir` (no shared static buffer); `scandir` builds glibc dirents and calls
    the blob's filter/compar on them; `alphasort` compares glibc dirents. `*64` variants identical.
  - non-LFS `struct statfs` 64/84: `statfs` converts from `statfs64` with the kernel's
    compat EOVERFLOW rule; `statfs64` identical.
  - `struct passwd` 28/24: bionic LP32 has no `pw_gecos`; `getpw{nam,uid}_r` fill glibc's layout.
  - `pthread_rwlock_t` 32/40: bionic's init memsets 40 bytes, so the glibc object holds a pointer
    to a heap bionic rwlock, made on first use for the all-zero static initialiser (published by
    CAS; glibc's writer-preferring initialiser is honoured). Exercised: 8 static rwlocks in the
    first compute run.
  - `pthread_mutex_t` 24/4: pass-through is safe (bionic's is smaller), but glibc's static
    recursive initialiser keeps `__kind` at +12; the conversion uses that offset, and
    `pthread_mutex_init` clears all 24 bytes first because bionic writes only 4. 14 conversions in
    the first compute run.
  - `dlopen` flags and handles: bionic LP32 `RTLD_NOW` 0, `RTLD_GLOBAL` 2, `RTLD_DEFAULT`
    0xffffffff, `RTLD_NEXT` 0xfffffffe (dlfcn.h "LP32 has historical ABI breakage"); glibc 2, 0x100,
    NULL, -1. `dlopen` maps the flags, `dlsym`/`dlvsym` map NULL to bionic's `RTLD_DEFAULT` and refuse
    `RTLD_NEXT` loudly. (Found by `gen-constants.py`'s RTLD family check.)
  - `sigaction` 140 (flags at 132) vs bionic LP32 `sigaction64` (handler 0, flags 4, restorer 8,
    mask 12): the existing translator, with the glibc struct declared portably.
  - `jmp_buf` 156: `_setjmp`/`longjmp` in i386 assembly (ebx esi edi ebp esp eip).
  - `long double` 12/8: only `strtold_l` (libstdc++ inside gpucomp/glvkspirv). Both return in
    x87 `st(0)`, so the call ABI matches; the value has double precision (logged once per process).
  - Identical on i386 (pass-through): `mallinfo` (10 x 4 bytes both), cond/attr/sem/cpu_set/
    posix_spawn_file_actions (bionic smaller), `*64` LFS structs, tm, timespec, rusage, rlimit
    (RLIM_INFINITY equal), Dl_info, dl_phdr_info, utsname, 32-bit off_t/time_t.
- **Not translated, by measurement**: `fcntl@GLIBC_2.0` (non-LFS lock commands use the same
  kernel 32-bit `struct flock` in both), `lseek/mmap/ftruncate/fseeko` (bionic's same-named 32-bit
  `off_t` variants).

Guest result (2026-10-11): `nvprobe32` through the sphal namespace as uid 10200 under bionic's
app filter: 4096 elements, 0 wrong, in `nvidia-smi`.
