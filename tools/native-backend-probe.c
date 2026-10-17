/* Diagnostic ABI probe for the source-built RM backend.
 * This contains our own C ABI calls only; no proprietary code or GPU packets.
 * Build: $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang
 *        -std=c11 -Wall -Wextra -Werror tools/native-backend-probe.c -ldl -o PROBE
 * Run under bionic: PROBE /absolute/path/libnv_bionic_native.so
 * Host build (cc, same flags) also works with the host library.
 * A pass is a backend smoke check, not Vulkan or Android sandbox conformance.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') count++;
    closedir(directory);
    return count;
}

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s /absolute/path/libnv_bionic_native.so\n", argv[0]);
        return 2;
    }
    const int before = open_fd_count();
    if (before < 0) { perror("count file descriptors"); return 1; }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    uint32_t (*version)(void) = (uint32_t (*)(void))dlsym(library, "nv_native_backend_abi_version");
    int32_t (*check)(uint32_t) = (int32_t (*)(uint32_t))dlsym(library, "nv_native_backend_check");
    if (!version || !check || version() != 1) {
        fprintf(stderr, "missing or incompatible native backend diagnostic ABI\n");
        dlclose(library);
        return 1;
    }
    int failed = 0;
    const int32_t expected[] = {16384, 16384, 256};
    const char *names[] = {"CPU VRAM", "GPU copy", "GPU compute"};
    for (uint32_t round = 0; round < 3 && !failed; round++) {
      for (uint32_t mode = 0; mode < 3; mode++) {
        int32_t words = check(mode);
        printf("round %u shared library %s: %d verified words (expected %d)\n", round+1, names[mode], words, expected[mode]);
        if (words != expected[mode]) { failed = 1; break; }
    }
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) { perror("read runtime mappings"); failed = 1; }
    else {
        char line[4096];
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libnvidia-") || strstr(line, "libEGL_nvidia") || strstr(line, "libnvglibc")) {
                fprintf(stderr, "unexpected proprietary driver/shim mapping: %s", line);
                failed = 1;
            }
            if (strstr(line, "libc.so") || strstr(line, "libnv_bionic_native.so")) printf("runtime-map: %s", line);
        }
        if (ferror(maps)) { perror("read runtime mappings"); failed = 1; }
        fclose(maps);
    }
    if (dlclose(library) != 0) { fprintf(stderr, "dlclose: %s\n", dlerror()); failed = 1; }
    const int after = open_fd_count();
    printf("file descriptors before/after: %d/%d\n", before, after);
    if (after != before) { fprintf(stderr, "native backend leaked file descriptors or fd inspection failed\n"); failed = 1; }
    puts(failed ? "FAIL: native shared-library checks" : "PASS: native shared-library memory, copy and compute checks");
    return failed;
}
