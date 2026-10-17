/* getenv() for the blobs, with a sidecar-driven default environment.
 *
 * NVIDIA's libraries read their configuration with plain getenv() (the import tables carry
 * getenv@GLIBC_2.2.5 and no secure_getenv/environ; measured on 615.71.09 with an LD_PRELOAD getenv
 * logger: libEGL_nvidia asks getenv("__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES") once, at the first
 * eglGetProcAddress-time platform scan). Inside an Android app nobody controls the process
 * environment, and our code must not mutate it. So the blobs' getenv (and only theirs: they bind
 * to the libc.so.6 front, every other library binds to bionic's getenv) answers
 *   1. the real environment, unchanged, when NAME is set there (even to an empty string);
 *   2. otherwise NAME's value from the file "nvglibc-default-env" next to libnvglibc.so,
 *      written by whatever installs the shims (the same placement rule as "host-glibc-version");
 *   3. otherwise NULL.
 *
 * File format (one entry per line):
 *   # comment            ignored, as are empty lines; a trailing "\r" is stripped
 *   NAME=VALUE           NAME = [A-Za-z_][A-Za-z0-9_]*, VALUE is taken verbatim to the end of the line
 *   ${ORIGIN} in VALUE   the directory that holds libnvglibc.so (no trailing slash), as in the
 *                        dynamic linker's $ORIGIN; any other "${" is an error
 * A duplicate NAME, a line without "=", or a bad NAME aborts the process with the file and line:
 * a malformed sidecar is an installation defect and must not be guessed around. A missing file
 * means "no defaults" (processes that only use Vulkan need none); NVGLIBC_TRACE logs it.
 *
 * The parser below is pure C so that tools/defenv-test.c can compile it on the host
 * (NVG_DEFENV_TEST); nvg_getenv itself only exists in the core build. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVGI_DEFENV_MAX 16
#define NVGI_DEFENV_FILE "nvglibc-default-env"

struct nvgi_defenv {
    unsigned n;
    char *name[NVGI_DEFENV_MAX];
    char *value[NVGI_DEFENV_MAX];
};

static int name_ok(const char *s, size_t n) {
    if (n == 0 || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* Frees every entry (the core never does: getenv results stay valid forever; used on parse errors and by
 * the host test). */
static void nvgi_defenv_free(struct nvgi_defenv *d) {
    for (unsigned i = 0; i < d->n; i++) {
        free(d->name[i]);
        free(d->value[i]);
    }
    d->n = 0;
}

/* Parses `text` (len bytes, need not be NUL-terminated). On success returns 0 and fills `out`
 * with heap strings that are never freed (getenv results must stay valid forever). On error
 * returns -1 with "line N: reason" in err. */
static int defenv_parse_entries(const char *text, size_t len, const char *origin, struct nvgi_defenv *out,
                                char *err, size_t errn) {
    static const char ORIGIN_TOKEN[] = "${ORIGIN}";
    const size_t tok = sizeof(ORIGIN_TOKEN) - 1, olen = strlen(origin);
    unsigned line = 0;
    out->n = 0;
    for (size_t pos = 0; pos < len;) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        size_t ll = end - pos;
        const char *l = text + pos;
        pos = end + 1;
        line++;
        if (ll > 0 && l[ll - 1] == '\r') ll--;
        if (ll == 0 || l[0] == '#') continue;
        if (memchr(l, '\0', ll) != NULL) {
            snprintf(err, errn, "line %u: NUL byte", line);
            return -1;
        }
        const char *eq = memchr(l, '=', ll);
        if (eq == NULL) {
            snprintf(err, errn, "line %u: expected NAME=VALUE", line);
            return -1;
        }
        size_t nl = (size_t)(eq - l), vl = ll - nl - 1;
        if (!name_ok(l, nl)) {
            snprintf(err, errn, "line %u: \"%.*s\" is not a valid variable name", line, (int)nl, l);
            return -1;
        }
        for (unsigned i = 0; i < out->n; i++) {
            if (strlen(out->name[i]) == nl && memcmp(out->name[i], l, nl) == 0) {
                snprintf(err, errn, "line %u: %.*s is defined twice", line, (int)nl, l);
                return -1;
            }
        }
        if (out->n == NVGI_DEFENV_MAX) {
            snprintf(err, errn, "line %u: more than %d entries", line, NVGI_DEFENV_MAX);
            return -1;
        }
        /* expanded length */
        size_t need = 0;
        for (size_t i = 0; i < vl;) {
            if (vl - i >= 2 && eq[1 + i] == '$' && eq[2 + i] == '{') {
                if (vl - i < tok || memcmp(eq + 1 + i, ORIGIN_TOKEN, tok) != 0) {
                    snprintf(err, errn, "line %u: only ${ORIGIN} may be expanded", line);
                    return -1;
                }
                need += olen;
                i += tok;
            } else {
                need++;
                i++;
            }
        }
        char *name = malloc(nl + 1), *value = malloc(need + 1);
        if (name == NULL || value == NULL) {
            snprintf(err, errn, "line %u: out of memory", line);
            free(name);
            free(value);
            return -1;
        }
        memcpy(name, l, nl);
        name[nl] = '\0';
        size_t o = 0;
        for (size_t i = 0; i < vl;) {
            if (vl - i >= tok && memcmp(eq + 1 + i, ORIGIN_TOKEN, tok) == 0) {
                memcpy(value + o, origin, olen);
                o += olen;
                i += tok;
            } else {
                value[o++] = eq[1 + i++];
            }
        }
        value[o] = '\0';
        out->name[out->n] = name;
        out->value[out->n] = value;
        out->n++;
    }
    return 0;
}

static int nvgi_defenv_parse(const char *text, size_t len, const char *origin, struct nvgi_defenv *out,
                             char *err, size_t errn) {
    int r = defenv_parse_entries(text, len, origin, out, err, errn);
    if (r != 0) {
        nvgi_defenv_free(out);
    }
    return r;
}

static char *nvgi_defenv_lookup(const struct nvgi_defenv *d, const char *name) {
    for (unsigned i = 0; i < d->n; i++) {
        if (strcmp(d->name[i], name) == 0) {
            return d->value[i];
        }
    }
    return NULL;
}

#ifndef NVG_DEFENV_TEST
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvglibc.h"

NVG_EXPORT char *nvg_getenv(const char *name);

static struct nvgi_defenv defenv;
static pthread_once_t defenv_once = PTHREAD_ONCE_INIT;

static void defenv_load(void) {
    Dl_info di;
    if (dladdr((const void *)&nvg_getenv, &di) == 0 || di.dli_fname == NULL || di.dli_fname[0] != '/') {
        nvgi_die("getenv: cannot locate libnvglibc.so with dladdr (got \"%s\"); the default environment "
                 "file %s must sit next to it", di.dli_fname ? di.dli_fname : "(null)", NVGI_DEFENV_FILE);
    }
    const char *slash = strrchr(di.dli_fname, '/');
    char origin[512], path[600];
    size_t dirlen = (size_t)(slash - di.dli_fname);
    if (dirlen == 0) dirlen = 1; /* "/" */
    if (dirlen >= sizeof(origin)) {
        nvgi_die("getenv: path of libnvglibc.so is too long: %s", di.dli_fname);
    }
    memcpy(origin, di.dli_fname, dirlen);
    origin[dirlen] = '\0';
    snprintf(path, sizeof(path), "%s/%s", dirlen == 1 ? "" : origin, NVGI_DEFENV_FILE);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            nvgi_die("getenv: cannot open %s: %s", path, strerror(errno));
        }
        nvgi_trace("getenv: no default environment (%s absent)", path);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 65536) {
        nvgi_die("getenv: %s: cannot stat it or larger than 64 KiB", path);
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (buf == NULL) {
        nvgi_die("getenv: out of memory reading %s", path);
    }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            nvgi_die("getenv: short read of %s (%zu of %lld bytes)", path, got, (long long)st.st_size);
        }
        got += (size_t)r;
    }
    close(fd);
    char err[256];
    if (nvgi_defenv_parse(buf, got, origin, &defenv, err, sizeof(err)) != 0) {
        nvgi_die("getenv: %s: %s (format: NAME=VALUE lines, # comments, ${ORIGIN} = this directory). "
                 "Remedy: rewrite the file with the installer", path, err);
    }
    free(buf);
    for (unsigned i = 0; i < defenv.n; i++) {
        nvgi_trace("getenv: default %s=%s (from %s)", defenv.name[i], defenv.value[i], path);
    }
}

NVG_EXPORT char *nvg_getenv(const char *name) {
    char *v = getenv(name);
    if (v != NULL || name == NULL) {
        return v;
    }
    pthread_once(&defenv_once, defenv_load);
    v = nvgi_defenv_lookup(&defenv, name);
    if (v != NULL) {
        nvgi_trace("getenv(\"%s\"): absent from the environment, default \"%s\"", name, v);
    }
    return v;
}
#endif
