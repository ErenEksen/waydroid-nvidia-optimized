# libnvglibc: glibc-compat shims for NVIDIA's userspace inside bionic.
# See DESIGN.md. `make` builds out/ (x86_64) and runs every check; any failure stops the build.
# `make ARCH=x86` (or `make all32`) builds the i386 set into out32/ from the host's lib32 driver
# (DESIGN.md "32-bit x86"). Both share the sources; gen/ and gen32/ are per arch.

ARCH        ?= x86_64
NDK         ?= /opt/android-ndk
TC          := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
READELF     := $(TC)/bin/llvm-readelf
NM          := $(TC)/bin/llvm-nm
NDK_INCLUDE := $(TC)/sysroot/usr/include
PYTHON      := python3 -I
CORE_SRCS := core/init.c core/stat.c core/signal.c core/pthread.c core/dl.c core/misc.c core/defenv.c core/setjmp.S
CORE_HDRS := core/nvglibc.h core/glibc_abi.h core/nvg_front.h

ifeq ($(ARCH),x86_64)
CC          := $(TC)/bin/x86_64-linux-android33-clang
STUBDIR     := $(TC)/sysroot/usr/lib/x86_64-linux-android/33
HOSTCC      ?= gcc
BLOBDIR     ?= /usr/lib
FRONTS := libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 ld-linux-x86-64.so.2
GEN := gen
OUT := out
OVERRIDE :=
PTR_SIZE := 8
else ifeq ($(ARCH),x86)
CC          := $(TC)/bin/i686-linux-android33-clang
STUBDIR     := $(TC)/sysroot/usr/lib/i686-linux-android/33
HOSTCC      ?= gcc -m32
BLOBDIR     ?= /usr/lib32
FRONTS := libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 ld-linux.so.2
GEN := gen32
OUT := out32
OVERRIDE := dispositions-x86.tsv
PTR_SIZE := 4
CORE_SRCS += core/i386.c
else
$(error ARCH must be x86_64 or x86, not '$(ARCH)')
endif
OVERRIDE_ARG := $(if $(OVERRIDE),--override $(OVERRIDE))

CORE_CFLAGS  := -D_GNU_SOURCE -fPIC -fvisibility=hidden -O2 -Wall -Wextra -Werror -Icore -I$(GEN)
CORE_LDFLAGS := -shared -Wl,-z,defs -Wl,--no-undefined -Wl,-soname,libnvglibc.so
FRONT_CFLAGS := -fPIC -ffreestanding -fno-builtin -fvisibility=hidden -O2 -Wall -Werror -Icore
FRONT_LDFLAGS = -shared -nostdlib -nostartfiles -Wl,-soname,$(1) \
                -Wl,--version-script=$(GEN)/$(1).map -Wl,--no-undefined-version -Wl,-z,defs

# The driver version is read from the running kernel module, never typed in.
DRIVER_VERSION_CMD := awk '/^NVRM version/ { for (i = 1; i <= NF; i++) \
    if ($$i ~ /^[0-9]+\.[0-9]+(\.[0-9]+)?$$/) { print $$i; exit } }' /proc/driver/nvidia/version

.PHONY: all all32 check-abi test clean dispositions FORCE
all: check-abi test $(OUT)/.checked

all32:
	$(MAKE) ARCH=x86 all

$(GEN) $(OUT):
	mkdir -p $@

# Blob paths: libEGL_nvidia.so.0 resolved to its real file, which must carry the
# kernel module's version; the other four by name in the same directory.
$(GEN)/blobs.list: FORCE | $(GEN)
	@set -e; v=$$($(DRIVER_VERSION_CMD) 2>/dev/null || true); \
	if [ -z "$$v" ]; then echo "shim: cannot read the driver version from /proc/driver/nvidia/version (NVIDIA kernel module not loaded?)" >&2; exit 1; fi; \
	egl=$$(readlink -f $(BLOBDIR)/libEGL_nvidia.so.0 || true); \
	case "$$egl" in *.so.$$v) ;; *) echo "shim: $(BLOBDIR)/libEGL_nvidia.so.0 resolves to '$$egl', not driver $$v (userspace/kernel mismatch, wrong BLOBDIR, or the $(ARCH) userspace (lib32-nvidia-utils for x86) not installed)" >&2; exit 1;; esac; \
	{ echo "$$egl"; for l in glsi eglcore gpucomp glvkspirv; do f=$(BLOBDIR)/libnvidia-$$l.so.$$v; \
	  if [ ! -f "$$f" ]; then echo "shim: missing $$f (driver $$v)" >&2; exit 1; fi; echo "$$f"; done; } > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; echo "shim: driver $$v, blobs in $(BLOBDIR)"; fi

$(GEN)/census.tsv: $(GEN)/blobs.list dispositions.tsv $(OVERRIDE) tools/import-census.py
	$(PYTHON) tools/import-census.py --arch $(ARCH) --dispositions dispositions.tsv $(OVERRIDE_ARG) \
	    --out $@ $$(cat $(GEN)/blobs.list)

$(GEN)/constants.h: tools/gen-constants.py dispositions.tsv $(OVERRIDE) | $(GEN)
	$(PYTHON) tools/gen-constants.py --host-cc "$(HOSTCC)" --ndk-cc "$(CC) -ferror-limit=0" \
	    --ndk-include $(NDK_INCLUDE) --dispositions dispositions.tsv $(OVERRIDE_ARG) --workdir $(GEN) --out $@

$(GEN)/fronts.stamp: $(GEN)/census.tsv dispositions.tsv $(OVERRIDE) tools/gen-fronts.py
	$(PYTHON) tools/gen-fronts.py --arch $(ARCH) --census $(GEN)/census.tsv --dispositions dispositions.tsv \
	    $(OVERRIDE_ARG) --nm $(NM) --stubdir $(STUBDIR) --outdir $(GEN)
	touch $@

$(OUT)/libnvglibc.so: $(CORE_SRCS) $(CORE_HDRS) $(GEN)/constants.h | $(OUT)
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ $(CORE_SRCS) -ldl -llog

define FRONT_RULE
$(OUT)/$(1): $(GEN)/fronts.stamp $(OUT)/libnvglibc.so core/nvg_front.h
	$$(CC) $$(FRONT_CFLAGS) $$(call FRONT_LDFLAGS,$(1)) -o $$@ $(GEN)/$(1).S $(GEN)/$(1)_table.c -L$(OUT) -lnvglibc
endef
$(foreach f,$(FRONTS),$(eval $(call FRONT_RULE,$(f))))

$(OUT)/.checked: $(OUT)/libnvglibc.so $(addprefix $(OUT)/,$(FRONTS)) tools/check-artifacts.py
	$(PYTHON) tools/check-artifacts.py --readelf $(READELF) --gen $(GEN) --out $(OUT) --ptr-size $(PTR_SIZE) $(FRONTS)
	touch $@

# Host-side glibc layout check: core/glibc_abi.h, as the target's compiler sees it, must equal
# what the host glibc says (gcc -m32 against lib32-glibc for x86).
$(GEN)/abi-check: tools/abi-check.c | $(GEN)
	$(HOSTCC) -O0 -Wall -Werror -o $@ $<

check-abi: $(GEN)/abi-check core/glibc_abi.h
	@$(GEN)/abi-check | sort > $(GEN)/abi-host.txt
	@$(HOSTCC) -E -dM -x c core/glibc_abi.h | sed -n 's/^#define NVG_GLIBC_\([A-Z0-9_]*\) \([0-9][0-9]*\).*/\1 \2/p' | sort > $(GEN)/abi-header.txt
	@if ! diff -u $(GEN)/abi-header.txt $(GEN)/abi-host.txt; then \
	    echo "check-abi: core/glibc_abi.h (-) differs from host glibc (+) for $(ARCH)" >&2; exit 1; fi
	@echo "check-abi: $$(wc -l < $(GEN)/abi-host.txt) $(ARCH) glibc constants in core/glibc_abi.h match the host glibc"

# Host unit test of the default-environment sidecar parser (core/defenv.c; DESIGN.md "getenv").
$(GEN)/defenv-test: tools/defenv-test.c core/defenv.c | $(GEN)
	$(HOSTCC) -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o $@ tools/defenv-test.c
test: $(GEN)/defenv-test
	@$(GEN)/defenv-test > $(GEN)/defenv-test.log || { cat $(GEN)/defenv-test.log; exit 1; }
	@tail -n 1 $(GEN)/defenv-test.log

# Regenerate dispositions.tsv from a per-import table: make dispositions IMPORTS_TSV=<file>.
IMPORTS_TSV ?=
dispositions:
	@test -n "$(IMPORTS_TSV)" || { echo 'set IMPORTS_TSV=<per-import table, imports-<driver version>.tsv>' >&2; exit 1; }
	$(PYTHON) tools/gen-dispositions.py $(IMPORTS_TSV) dispositions.tsv

clean:
	rm -rf out gen out32 gen32
