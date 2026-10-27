/* Internal header of the libnvglibc core. Everything here is hidden
 * (-fvisibility=hidden); only NVG_EXPORT functions are dynamic symbols, and all
 * of those are named nvg_*. Internal helpers are named nvgi_* so they can never
 * collide with an nvg_<glibc name> target. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "glibc_abi.h"
#include "nvg_front.h"

#define NVG_EXPORT __attribute__((visibility("default")))

/* Abort with "libnvglibc: <msg>" on stderr and logcat. */
_Noreturn void nvgi_die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Log "libnvglibc: <msg>" when NVGLIBC_TRACE is set. */
void nvgi_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

extern int nvgi_trace_on;
extern void *nvgi_lib_handle[3]; /* indexed by enum nvg_lib (libc, libm, libdl) */
extern size_t nvgi_default_stack;
extern int nvgi_mutex_template[3]; /* index: glibc __kind (1 recursive, 2 errorcheck) */

void nvgi_core_init(void);
/* Address of the glibc-ABI definition of `name` exported by a registered
 * front, or NULL. */
void *nvgi_front_lookup(const char *name);
