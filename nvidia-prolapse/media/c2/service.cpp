/* SPDX-License-Identifier: Apache-2.0
 * android.hardware.media.c2@1.2-service-nvprolapse — vendor Codec2 HAL service exposing
 * c2.nvprolapse.<codec>.decoder for every codec nv_video_query_caps() reports.
 * Follows frameworks/av media/codec2/hidl/services/vendor.cpp (android-13).
 *
 * Start-up checks, all fatal with a named reason (init restarts us, logcat says why):
 *  - the NVIDIA device opens and the backend ABI matches,
 *  - the media_codecs_c2.xml the framework will read lists exactly the live caps
 *    (a stale file would advertise codecs/sizes this GPU does not decode).
 */
#define LOG_TAG "nvc2-service"
#include <C2Component.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <hwminijail/HardwareMinijail.h>
#include <log/log.h>

#include <csignal>
#include <cstdio>
#include <string>

#include "NvC2Component.h"

namespace {
constexpr char kSeccompPolicy[] = "/odm/etc/seccomp_policy/android.hardware.media.c2@1.2-nvprolapse.policy";
constexpr char kCodecsXml[] = "/odm/etc/media_codecs_c2.xml";
} // namespace

int main()
{
    using namespace ::android;
    signal(SIGPIPE, SIG_IGN);
    hardware::SetupMinijail(kSeccompPolicy);

    std::string err;
    std::shared_ptr<nvc2::Store> impl = nvc2::Store::Create(&err);
    if (!impl) {
        ALOGE("no NVDEC store: %s", err.c_str());
        return 1;
    }
    /* stdio, not <fstream>: the image's libc++ lacks the r29 headers' basic_filebuf symbols */
    std::string have;
    bool readable = false;
    if (FILE *f = fopen(kCodecsXml, "re")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            have.append(buf, n);
        readable = !ferror(f);
        fclose(f);
    }
    if (!readable || have != nvc2::media_codecs_xml(impl->entries())) {
        ALOGE("%s does not match this GPU's decode caps; regenerate it on the host with nvc2-xmlgen "
              "(expected:\n%s)",
              kCodecsXml, nvc2::media_codecs_xml(impl->entries()).c_str());
        return 1;
    }

    ProcessState::initWithDriver("/dev/vndbinder");
    ProcessState::self()->startThreadPool();
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    using namespace ::android::hardware::media::c2::V1_2;
    sp<IComponentStore> store = new utils::ComponentStore(impl);
    if (store->registerAsService("default") != OK) {
        ALOGE("cannot register IComponentStore/default (VINTF manifest fragment missing?)");
        return 1;
    }
    ALOGI("IComponentStore/default registered, %zu decoder(s)", impl->entries().size());
    hardware::joinRpcThreadpool();
    return 0;
}
