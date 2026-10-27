/* Public NVIDIA open-kernel-module ABI interoperability diagnostic.
 * Build against the source headers matching /proc/driver/nvidia/version.
 * No proprietary userspace, no display or container changes. */
#define _GNU_SOURCE
#include <class/cl0071.h>
#include <class/cl0080.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0000/ctrl0000unix.h>
#include <ctrl/ctrl2080/ctrl2080fb.h>
#include <dirent.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <nv-ioctl.h>
#include <nv-unix-nvos-params-wrappers.h>
#include <nv_drm_common_ioctl.h>
#include <nv_escape.h>
#include <nvkms-kapi-private.h>
#include <nvos.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void fail(const char *what) {
  fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
  exit(1);
}
static void require(int condition, const char *what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
  }
}
static void call(int fd, unsigned nr, void *params, size_t size) {
  if (ioctl(fd, _IOC(_IOC_READ | _IOC_WRITE, 'F', nr, size), params) < 0)
    fail("RM ioctl");
}
static void status(uint32_t value, const char *what) {
  if (value) {
    fprintf(stderr, "FAIL: %s RM status 0x%x\n", what, value);
    exit(1);
  }
}
static int fd_count(void) {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory)
    fail("opendir fds");
  struct dirent *entry;
  int count = 0;
  while ((entry = readdir(directory)))
    if (entry->d_name[0] != '.')
      count++;
  closedir(directory);
  return count;
}
static NvHandle allocate(int ctl, NvHandle client, NvHandle parent,
                         uint32_t cls, void *params, size_t size) {
  NVOS21_PARAMETERS a = {.hRoot = client,
                         .hObjectParent = parent,
                         .hClass = cls,
                         .pAllocParms = (NvP64)(uintptr_t)params,
                         .paramsSize = (NvU32)size};
  call(ctl, NV_ESC_RM_ALLOC, &a, sizeof(a));
  status(a.status, "allocate");
  return a.hObjectNew;
}
static void release(int ctl, NvHandle client, NvHandle parent,
                    NvHandle object) {
  NVOS00_PARAMETERS a = {
      .hRoot = client, .hObjectParent = parent, .hObjectOld = object};
  call(ctl, NV_ESC_RM_FREE, &a, sizeof(a));
  status(a.status, "free");
}
static void control(int ctl, NvHandle client, NvHandle object, uint32_t cmd,
                    void *params, size_t size) {
  NVOS54_PARAMETERS a = {.hClient = client,
                         .hObject = object,
                         .cmd = cmd,
                         .params = (NvP64)(uintptr_t)params,
                         .paramsSize = (NvU32)size};
  call(ctl, NV_ESC_RM_CONTROL, &a, sizeof(a));
  status(a.status, "control");
}
static int gpu_open(int ctl, unsigned minor) {
  char path[80];
  snprintf(path, sizeof(path), "/dev/nvidia%u", minor);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    fail(path);
  nv_ioctl_register_fd_t p = {.ctl_fd = ctl};
  call(fd, NV_ESC_REGISTER_FD, &p, sizeof(p));
  return fd;
}
static volatile uint32_t *map_rm(int ctl, int fd, NvHandle client,
                                 NvHandle device, NvHandle memory,
                                 size_t bytes) {
  nv_ioctl_nvos33_parameters_with_fd m = {.params = {.hClient = client,
                                                     .hDevice = device,
                                                     .hMemory = memory,
                                                     .length = bytes},
                                          .fd = fd};
  call(ctl, NV_ESC_RM_MAP_MEMORY, &m, sizeof(m));
  status(m.params.status, "RM map");
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    fail("mmap RM");
  return p;
}
static int render_node(const nv_ioctl_card_info_t *card) {
  char pci[32];
  snprintf(pci, sizeof(pci), "%04x:%02x:%02x.%u", card->pci_info.domain,
           card->pci_info.bus, card->pci_info.slot, card->pci_info.function);
  DIR *directory = opendir("/sys/class/drm");
  if (!directory)
    fail("DRM sysfs");
  struct dirent *entry;
  int found = -1;
  while ((entry = readdir(directory))) {
    if (strncmp(entry->d_name, "renderD", 7))
      continue;
    char path[512], target[512];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/device", entry->d_name);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
      continue;
    target[n] = 0;
    const char *last = strrchr(target, '/');
    if (!last || strcmp(last + 1, pci))
      continue;
    snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
    found = open(path, O_RDWR | O_CLOEXEC);
    if (found < 0)
      fail(path);
    char name[64] = {0};
    struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    if (ioctl(found, DRM_IOCTL_VERSION, &version) < 0)
      fail("DRM version");
    require(!strcmp(name, "nvidia-drm"),
            "selected PCI GPU must use nvidia-drm");
    printf("DRM device: %s driver=%s PCI=%s\n", path, name, pci);
    break;
  }
  closedir(directory);
  require(found >= 0, "no NVIDIA render node matching RM PCI GPU");
  return found;
}
static uint32_t pattern(unsigned i) { return i * 0x10203u ^ 0xabbaf00du; }
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--abi")) {
    printf("RmObject\t%zu\n", sizeof(NV0000_CTRL_OS_UNIX_EXPORT_OBJECT));
    printf("ExportObject\t%zu\n",
           sizeof(NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS));
    printf("ImportObject\t%zu\n",
           sizeof(NV0000_CTRL_OS_UNIX_IMPORT_OBJECT_FROM_FD_PARAMS));
    printf("Surface\t%zu\n", sizeof(struct NvKmsKapiPrivSurfaceParams));
    printf("ImportMemory\t%zu\n",
           sizeof(struct NvKmsKapiPrivImportMemoryParams));
    printf("ImportGem\t%zu\n",
           sizeof(struct drm_nvidia_gem_import_nvkms_memory_params));
    printf("ExportGem\t%zu\n",
           sizeof(struct drm_nvidia_gem_export_nvkms_memory_params));
    printf("Prime\t%zu\n", sizeof(struct drm_prime_handle));
    printf("Identify\t%zu\n",
           sizeof(struct drm_nvidia_gem_identify_object_params));
    printf("CloseGem\t%zu\n", sizeof(struct drm_gem_close));
    printf("import-gem-ioctl\t0x%lx\n",
           (unsigned long)DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY);
    printf("export-prime-ioctl\t0x%lx\n",
           (unsigned long)DRM_IOCTL_PRIME_HANDLE_TO_FD);
    printf("close-gem-ioctl\t0x%lx\n", (unsigned long)DRM_IOCTL_GEM_CLOSE);
    printf("DRM_CLOEXEC\t0x%x\n", DRM_CLOEXEC);
    printf("DRM_RDWR\t0x%x\n", DRM_RDWR);
    return 0;
  }
  int direct = argc == 2 && !strcmp(argv[1], "--rm-dmabuf");
  int generic = argc == 2 && !strcmp(argv[1], "--generic");
  require(argc == 1 || direct || generic,
          "usage: PROBE [--rm-dmabuf | --generic | --abi]");
  setvbuf(stdout, NULL, _IONBF, 0);
  int before = fd_count(), ctl = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
  if (ctl < 0)
    fail("nvidiactl");
  FILE *file = fopen("/proc/driver/nvidia/version", "r");
  if (!file)
    fail("driver version");
  char line[512];
  require(fgets(line, sizeof(line), file) != NULL, "read driver version");
  fclose(file);
  nv_ioctl_rm_api_version_t version = {.cmd = NV_RM_API_VERSION_CMD_STRICT};
  char *save = NULL, *word = strtok_r(line, " \t\n", &save);
  while (word) {
    if (strchr(word, '.') && strspn(word, "0123456789.") == strlen(word)) {
      require(strlen(word) < sizeof(version.versionString), "version length");
      strcpy(version.versionString, word);
      break;
    }
    word = strtok_r(NULL, " \t\n", &save);
  }
  require(version.versionString[0], "parse running driver version");
  call(ctl, NV_ESC_CHECK_VERSION_STR, &version, sizeof(version));
  require(version.reply == NV_RM_API_VERSION_REPLY_RECOGNIZED,
          "strict driver version check");
  printf("running driver: %s\n", version.versionString);
  NvHandle client = allocate(ctl, 0, 0, 0x41, NULL, 0);
  nv_ioctl_card_info_t cards[64] = {0};
  call(ctl, NV_ESC_CARD_INFO, cards, sizeof(cards));
  nv_ioctl_card_info_t *card = NULL;
  for (unsigned i = 0; i < 64; i++)
    if (cards[i].valid && cards[i].pci_info.vendor_id == 0x10de) {
      card = &cards[i];
      break;
    }
  require(card != NULL, "enumerated NVIDIA GPU");
  int gpu = gpu_open(ctl, card->minor_number);
  NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS info = {.gpuId = card->gpu_id};
  control(ctl, client, client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &info,
          sizeof(info));
  NV0080_ALLOC_PARAMETERS device_params = {.deviceId = info.deviceInstance,
                                           .hClientShare = client};
  NvHandle device = allocate(ctl, client, client, NV01_DEVICE_0, &device_params,
                             sizeof(device_params));
  NvHandle subdevice =
      allocate(ctl, client, device, 0x2080, &info.subDeviceInstance,
               sizeof(info.subDeviceInstance));
  NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS layout = {0};
  control(ctl, client, subdevice,
          NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT, &layout,
          sizeof(layout));
  printf("semaphore layout: stride=%" PRIu64 " monitored-offset=%" PRIu64
         " max-submitted-offset=%" PRIu64 " caps=0x%x\n",
         (uint64_t)layout.size, (uint64_t)layout.monitoredFenceThresholdOffset,
         (uint64_t)layout.maxSubmittedSemaphoreValueOffset, layout.caps);
  const size_t bytes = 65536;
  NV_MEMORY_ALLOCATION_PARAMS mem = {
      .owner = client,
      .type = NVOS32_TYPE_IMAGE,
      .flags = NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
               NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE |
               NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED |
               NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED |
               NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM,
      .attr = 3u << 27,
      .attr2 = 5,
      .format = 6,
      .size = bytes,
      .alignment = 4096,
      .limit = bytes - 1};
  NvHandle memory = allocate(ctl, client, device, 0x40, &mem, sizeof(mem));
  int mapfd = gpu_open(ctl, card->minor_number);
  volatile uint32_t *mapping =
      map_rm(ctl, mapfd, client, device, memory, bytes);
  for (unsigned i = 0; i < bytes / 4; i++)
    mapping[i] = pattern(i);
  __sync_synchronize();
  int drm = render_node(card), dmabuf_fd = -1;
  uint32_t exported_gem = 0;
  if (direct) {
    nv_ioctl_export_to_dma_buf_fd_t exported = {
        .fd = -1,
        .hClient = client,
        .totalObjects = 1,
        .numObjects = 1,
        .totalSize = bytes,
        .mappingType = NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT,
        .bAllowMmap = 1};
    exported.handles[0] = memory;
    exported.sizes[0] = bytes;
    call(gpu, NV_ESC_EXPORT_TO_DMABUF_FD, &exported, sizeof(exported));
    status(exported.status, "export DMA-BUF");
    dmabuf_fd = exported.fd;
    printf("direct RM DMA-BUF exported: ABI-size=%zu status-offset=%zu\n",
           sizeof(exported), offsetof(nv_ioctl_export_to_dma_buf_fd_t, status));
  } else {
    int opaque = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (opaque < 0)
      fail("opaque RM fd");
    NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS export_rm = {.fd = opaque};
    export_rm.object.type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM;
    export_rm.object.data.rmObject.hDevice = device;
    export_rm.object.data.rmObject.hParent = device;
    export_rm.object.data.rmObject.hObject = memory;
    control(ctl, client, client, NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD,
            &export_rm, sizeof(export_rm));
    struct NvKmsKapiPrivImportMemoryParams private_import = {
        .memFd = opaque,
        .surfaceParams = {.layout = NvKmsSurfaceMemoryLayoutPitch}};
    if (generic) {
      private_import.surfaceParams.layout = NvKmsSurfaceMemoryLayoutBlockLinear;
      private_import.surfaceParams.blockLinear.genericMemory = NV_TRUE;
    }
    struct drm_nvidia_gem_import_nvkms_memory_params import_gem = {
        .mem_size = bytes,
        .nvkms_params_ptr = (uintptr_t)&private_import,
        .nvkms_params_size = sizeof(private_import)};
    if (ioctl(drm, DRM_IOCTL_NVIDIA_GEM_IMPORT_NVKMS_MEMORY, &import_gem) < 0)
      fail("DRM NVKMS memory import");
    close(opaque);
    exported_gem = import_gem.handle;
    struct drm_prime_handle export_dma = {
        .handle = exported_gem, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1};
    if (ioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export_dma) < 0)
      fail("DRM PRIME export");
    dmabuf_fd = export_dma.fd;
    printf("NVKMS export path: private ABI-size=%zu GEM=%u DMA-BUF=%d\n",
           sizeof(private_import), exported_gem, dmabuf_fd);
  }
  require(dmabuf_fd >= 0, "export returned FD");
  require((fcntl(dmabuf_fd, F_GETFD) & FD_CLOEXEC) != 0, "DMA-BUF CLOEXEC");
  require(lseek(dmabuf_fd, 0, SEEK_END) == (off_t)bytes, "DMA-BUF size");
  printf("DMA-BUF exported: fd=%d size=%zu CLOEXEC=yes\n", dmabuf_fd, bytes);
  volatile uint32_t *dma =
      mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
  if (dma == MAP_FAILED)
    fail("dma-buf mmap");
  for (unsigned i = 0; i < bytes / 4; i++)
    require(dma[i] == pattern(i), "DMA-BUF alias observes RM writes");
  int importer_drm = render_node(card);
  struct drm_prime_handle prime = {.fd = dmabuf_fd};
  if (ioctl(importer_drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
    fail("DRM PRIME import");
  printf("DRM PRIME import on independent DRM fd: GEM handle=%u\n",
         prime.handle);
  NvHandle import = 0;
  if (direct) {
    NV_OS_DESC_MEMORY_ALLOCATION_PARAMS imported = {
        .type = NVOS32_TYPE_IMAGE,
        .attr = 3u << 27,
        .attr2 = 5,
        .descriptor = (NvP64)(uintptr_t)dmabuf_fd,
        .limit = bytes - 1,
        .descriptorType = NVOS32_DESCRIPTOR_TYPE_OS_FILE_HANDLE};
    import = allocate(ctl, client, device, NV01_MEMORY_SYSTEM_OS_DESCRIPTOR,
                      &imported, sizeof(imported));
    printf("RM dma-buf import: class=0x71 handle=0x%x ABI-size=%zu\n", import,
           sizeof(imported));
  } else {
    int opaque = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (opaque < 0)
      fail("opaque RM fd");
    struct NvKmsKapiPrivExportMemoryParams private_export = {.memFd = opaque};
    struct drm_nvidia_gem_export_nvkms_memory_params export_gem = {
        .handle = prime.handle,
        .nvkms_params_ptr = (uintptr_t)&private_export,
        .nvkms_params_size = sizeof(private_export)};
    if (ioctl(importer_drm, DRM_IOCTL_NVIDIA_GEM_EXPORT_NVKMS_MEMORY,
              &export_gem) < 0)
      fail("DRM NVKMS opaque export");
    NV0000_CTRL_OS_UNIX_IMPORT_OBJECT_FROM_FD_PARAMS import_rm = {.fd = opaque};
    import_rm.object.type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM;
    import_rm.object.data.rmObject.hDevice = device;
    import_rm.object.data.rmObject.hParent = device;
    control(ctl, client, client, NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD,
            &import_rm, sizeof(import_rm));
    import = import_rm.object.data.rmObject.hObject;
    close(opaque);
    printf("RM opaque import from DRM GEM: handle=0x%x\n", import);
  }
  if (munmap((void *)mapping, bytes))
    fail("munmap RM");
  close(mapfd);
  release(ctl, client, device, memory);
  for (unsigned i = 0; i < bytes / 4; i++)
    require(dma[i] == pattern(i),
            "DMA-BUF survives original RM object destruction");
  int importfd = gpu_open(ctl, card->minor_number);
  volatile uint32_t *importmap =
      map_rm(ctl, importfd, client, device, import, bytes);
  for (unsigned i = 0; i < bytes / 4; i++)
    require(importmap[i] == pattern(i), "RM import observes DMA-BUF bytes");
  for (unsigned i = 0; i < bytes / 4; i++)
    importmap[i] = ~pattern(i);
  __sync_synchronize();
  for (unsigned i = 0; i < bytes / 4; i++)
    require(dma[i] == ~pattern(i), "DMA-BUF observes imported RM writes");
  struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
  if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    fail("dma-buf sync START");
  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
  if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    fail("dma-buf sync END");
  if (munmap((void *)importmap, bytes))
    fail("munmap imported");
  close(importfd);
  release(ctl, client, device, import);
  if (munmap((void *)dma, bytes))
    fail("munmap dma-buf");
  close(dmabuf_fd);
  struct drm_gem_close gem_close = {.handle = prime.handle};
  if (ioctl(importer_drm, DRM_IOCTL_GEM_CLOSE, &gem_close) < 0)
    fail("GEM close");
  close(importer_drm);
  if (exported_gem) {
    gem_close.handle = exported_gem;
    if (ioctl(drm, DRM_IOCTL_GEM_CLOSE, &gem_close) < 0)
      fail("exported GEM close");
  }
  close(drm);
  release(ctl, client, device, subdevice);
  release(ctl, client, client, device);
  release(ctl, client, 0, client);
  close(gpu);
  close(ctl);
  int after = fd_count();
  printf("file descriptors: %d -> %d\n", before, after);
  require(before == after, "file descriptor leak");
  puts("PASS: RM DMA-BUF export/import, DRM PRIME import, aliasing, "
       "original-handle lifetime; no GPU execution or presentation tested");
  return 0;
}
