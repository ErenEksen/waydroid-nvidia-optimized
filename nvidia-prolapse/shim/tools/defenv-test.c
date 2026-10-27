/* Host unit test for the default-environment sidecar parser (core/defenv.c, NVG_DEFENV_TEST build).
 * `make test` runs it; every case prints one line and the exit code is the number of failures. */
#define NVG_DEFENV_TEST 1
#include "../core/defenv.c"

static int fails;

static void expect_ok(const char *what, const char *text, const char *name, const char *want) {
    struct nvgi_defenv d;
    char err[256] = "";
    int r = nvgi_defenv_parse(text, strlen(text), "/vendor_extra/lib64", &d, err, sizeof(err));
    const char *got = r == 0 ? nvgi_defenv_lookup(&d, name) : NULL;
    int ok = r == 0 && ((want == NULL && got == NULL) || (want && got && strcmp(want, got) == 0));
    printf("%s %s: %s -> %s\n", ok ? "PASS" : "FAIL", what, name, r ? err : got ? got : "(null)");
    fails += !ok;
    nvgi_defenv_free(&d);
}

static void expect_err(const char *what, const char *text, const char *needle) {
    struct nvgi_defenv d;
    char err[256] = "";
    int r = nvgi_defenv_parse(text, strlen(text), "/o", &d, err, sizeof(err));
    int ok = r != 0 && strstr(err, needle) != NULL;
    printf("%s %s: \"%s\"\n", ok ? "PASS" : "FAIL", what, r ? err : "(accepted)");
    fails += !ok;
}

int main(void) {
    const char *f = "# comment\n\n__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=${ORIGIN}/nvnative_egl_android.json\r\n"
                    "A_B=x=y\nEMPTY=\nTWO=${ORIGIN}:${ORIGIN}/sub";
    expect_ok("origin expansion", f, "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES",
              "/vendor_extra/lib64/nvnative_egl_android.json");
    expect_ok("value keeps later '='", f, "A_B", "x=y");
    expect_ok("empty value", f, "EMPTY", "");
    expect_ok("two expansions, no trailing newline", f, "TWO", "/vendor_extra/lib64:/vendor_extra/lib64/sub");
    expect_ok("absent name", f, "HOME", NULL);
    expect_ok("prefix is not a match", f, "A", NULL);
    expect_ok("empty file", "", "X", NULL);
    expect_err("no '='", "JUSTANAME\n", "line 1: expected NAME=VALUE");
    expect_err("bad name", "\n1X=2\n", "line 2: \"1X\" is not a valid variable name");
    expect_err("empty name", "=v\n", "not a valid variable name");
    expect_err("duplicate", "A=1\nA=2\n", "line 2: A is defined twice");
    expect_err("other expansion", "A=${HOME}/x\n", "line 1: only ${ORIGIN} may be expanded");
    expect_err("truncated token", "A=${ORIG\n", "only ${ORIGIN}");
    char big[NVGI_DEFENV_MAX * 8 + 64];
    size_t o = 0;
    for (int i = 0; i <= NVGI_DEFENV_MAX; i++) o += (size_t)sprintf(big + o, "V%d=1\n", i);
    expect_err("too many entries", big, "more than");
    printf("defenv-test: %d failure(s)\n", fails);
    return fails;
}
