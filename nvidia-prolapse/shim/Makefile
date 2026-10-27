# libnvglibc: glibc-compat shims for NVIDIA's userspace inside bionic.
# See DESIGN.md. `make` builds out/ (x86_64) and runs every check; any failure stops the build.
# `make ARCH=x86` (or `make all32`) builds the i386 set into out32/ (with the host's 32-bit driver, when loaded)
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
FRONTS := libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 ld-linux-x86-64.so.2
GEN := gen
OUT := out
OVERRIDE :=
PTR_SIZE := 8
else ifeq ($(ARCH),x86)
CC          := $(TC)/bin/i686-linux-android33-clang
STUBDIR     := $(TC)/sysroot/usr/lib/i686-linux-android/33
HOSTCC      ?= gcc -m32
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

# The fronts export the union of every driver branch's imports (imports/<version>-<arch>.tsv, the
# reference lists cut from each branch's libraries; DESIGN.md "Driver branches"), so one build serves
# every supported driver. On a host with the NVIDIA driver loaded, its own closure (found through the
# loader cache at the kernel module's version: tools/find-driver.py) is added and must be covered too.
# A build machine without the driver names the reason: make REFERENCE_ONLY="<why>" (CI release builds).
REFERENCE_ONLY ?=
REFERENCES := $(sort $(wildcard imports/*-$(ARCH).tsv))
ifeq ($(REFERENCES),)
$(error no driver reference lists imports/*-$(ARCH).tsv)
endif

.PHONY: all all32 check-abi test clean dispositions FORCE
all: check-abi test $(OUT)/.checked

all32:
	$(MAKE) ARCH=x86 all

$(GEN) $(OUT):
	mkdir -p $@

$(GEN)/blobs.list: FORCE | $(GEN)
	@set -e; if [ -n "$(REFERENCE_ONLY)" ]; then \
	  : > $@.tmp; echo "shim: reference lists only ($(REFERENCE_ONLY))"; \
	else \
	  $(PYTHON) tools/find-driver.py --arch $(ARCH) > $@.tmp || { rm -f $@.tmp; \
	    echo "shim: no usable host driver for $(ARCH) (above); on a build machine without the NVIDIA driver use make REFERENCE_ONLY=\"<reason>\"" >&2; exit 1; }; \
	fi; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(GEN)/census.tsv: $(GEN)/blobs.list $(REFERENCES) dispositions.tsv $(OVERRIDE) tools/import-census.py
	$(PYTHON) tools/import-census.py --arch $(ARCH) --dispositions dispositions.tsv $(OVERRIDE_ARG) \
	    $(addprefix --reference ,$(REFERENCES)) --out $@ $$(cat $(GEN)/blobs.list)

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
