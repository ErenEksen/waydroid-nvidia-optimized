#!/usr/bin/env bash
# packaging/install.sh — install waydroid-nvidia-optimized from this source tree on any systemd distro. Needs no
# other waydroid or waydroid-nvidia installation. Installs what the AUR waydroid-nvidia-bin package installs, with
# this fork's optimized Venus components, then you run
#   sudo waydroid-nvidia-setup [-s GAPPS] (runs waydroid init if needed, starts the container); waydroid session start
#
#   sudo packaging/install.sh [--from DIR] [--check] [--no-deps]      (./install.sh in the checkout does this)
#   sudo packaging/install.sh --uninstall
#
#   The binaries are pinned in packaging/releases/release.env and packaging/releases/SHA256SUMS: the base release
#   (prebuilt vendor image, native driver bundle, guest and host stack) and this fork's Venus overlay. The scripts,
#   services and Waydroid patches come from this tree.
#   --from DIR    use the pinned files already in DIR instead of downloading them; their <asset>.sigstore.json
#                 bundles, when present in DIR, are verified with cosign
#   --check       download and verify only; install nothing
#   --no-deps     do not install distro packages (each required tool is still checked by name)
#   --uninstall   put the official vendor image back (waydroid-nvidia-setup --restore-official-vendor), then
#                 remove every file a previous run installed (list in /var/lib/waydroid-nvidia/installed-files)
#
# Verification: every file must match its line in packaging/releases/SHA256SUMS (versioned with this tree, never
# taken from the download). The base assets' build provenance is verified too when a verifier is available: cosign
# (>= 2.4.3, no GitHub login) against the Sigstore bundle the release ships next to each asset
# (<asset>.sigstore.json), else a logged-in GitHub CLI (gh attestation verify); without either the script says "not
# checked". The commit in the native bundle's BUILD-RECORD.json must be the pinned BASE_COMMIT.
# Files owned by a distro package are never overwritten: the script stops and names the package.
#
# Credits: distro-family detection, per-family dependency lists, the one-line-per-asset SHA256SUMS rule
# and the interpreter pinning of /usr/lib/waydroid/waydroid.py follow quinovax/waydroid-nvidia
# packaging/install-from-release.sh; the verify-before-install --check mode follows
# ErenEksen/waydroid-nvidia-optimized install.sh.
set -euo pipefail
# Tool output parsed here and in every child (readelf, debugfs, lxc-info, systemctl, ...) is the untranslated text
export LC_ALL=C
unset LANGUAGE

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
WAYDROID_UPSTREAM=https://github.com/waydroid/waydroid
WAYDROID_SHA=a33a5c0b31d89d6ce687381104b30aff4dd2d330     # patches/waydroid/BASE
WAYDROID_TARBALL_SHA256=e7817beac7e26f7d54b2d9f49c847dab8e137c92e403825829e7abbd0b63c411
STATE=/var/lib/waydroid-nvidia
LIST=$STATE/installed-files
PREFIX=/usr/lib/waydroid-nvidia

die()  { echo "install.sh: $*" >&2; exit 1; }
info() { echo "==> $*"; }

FROM="" CHECK=0 DEPS=1 UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --from) FROM=$(cd "${2:?--from needs a directory}" && pwd); shift 2 ;;
        --check) CHECK=1; shift ;;
        --no-deps) DEPS=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,30p' "$0" 2>/dev/null || true; exit 0 ;;
        *) die "unknown argument: $1 (--help)" ;;
    esac
done
[ "$(id -u)" = 0 ] || [ "$CHECK" = 1 ] || die "run as root (sudo)"
[ "$(uname -m)" = x86_64 ] || die "only x86_64 hosts are supported"

# ---------------------------------------------------------------- SELinux
# Where SELinux is enabled, Waydroid runs confined by Fedora's policy for it (packaging/selinux, module "waydroid",
# built by CI and shipped as the release asset waydroid-nvidia-selinux-<tag>.pp): without it an enforcing host denies
# Android's service managers the binder context manager and the container never boots. Loaded at priority 400, above
# the 200 a distribution package uses, so a leftover module of the distro's own waydroid package is shadowed rather
# than duplicated; SEMODULE records that this script loaded it.
SEMODULE=$STATE/selinux-module
selinux_on() { command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; }
relabel() {   # the paths waydroid.fc labels (its /var/lib/waydroid(.*)? also covers /var/lib/waydroid-nvidia), where they exist
    local paths=() p
    for p in /usr/lib/waydroid /var/lib/waydroid /var/lib/waydroid-nvidia /run/waydroid-*; do [ -e "$p" ] && paths+=("$p"); done
    # -x: not into a mounted rootfs (a running container mounts its images under /var/lib/waydroid)
    [ ${#paths[@]} -eq 0 ] || restorecon -R -x "${paths[@]}" ||
        die "restorecon -R -x ${paths[*]} failed (see above); fix the labels with that command, then re-run"
}

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" = 1 ]; then
    [ -f "$LIST" ] || die "$LIST missing: nothing was installed by this script"
    if grep -qx 'nvidia_prolapse_install = vendor-image' /var/lib/waydroid/waydroid.cfg 2>/dev/null; then
        info "putting the official vendor image back (waydroid-nvidia-setup --restore-official-vendor)"
        /usr/bin/waydroid-nvidia-setup --restore-official-vendor ||
            die "the official vendor image was not restored (see above); fix that first, or run 'sudo waydroid init -f' and re-run --uninstall"
    fi
    dropin=/etc/systemd/system/waydroid-container.service.d/source-native.conf
    if [ -f "$dropin" ] && grep -q "$PREFIX/native" "$dropin"; then rm -f "$dropin"; info "removed $dropin"; fi
    n=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if [ -e "$f" ] || [ -L "$f" ]; then rm -f -- "$f"; n=$((n + 1)); fi
    done < "$LIST"
    # empty directories the install created
    sort -r "$LIST" | while IFS= read -r f; do d=$(dirname -- "$f"); rmdir --ignore-fail-on-non-empty -p -- "$d" 2>/dev/null || true; done
    rm -f "$LIST"
    if [ -f "$SEMODULE" ]; then
        semodule -X 400 -r waydroid ||
            die "semodule -X 400 -r waydroid failed (see above); remove the waydroid policy module with it, then re-run"
        rm -f "$SEMODULE"
        ! selinux_on || relabel
        info "removed the waydroid SELinux policy module; Waydroid's data is relabelled"
    fi
    systemctl daemon-reload 2>/dev/null || true
    systemctl reload dbus.service 2>/dev/null || systemctl reload dbus-broker.service 2>/dev/null || true
    info "removed $n files"
    cat <<'EOF'
Gate logs, old staged driver roots and the Waydroid data stay on disk. Remove the first two with
  sudo rm -rf /var/lib/waydroid-nvidia /var/lib/waydroid/nvidia-source-native-* /var/lib/waydroid/nvidia-prolapse
EOF
    exit 0
fi

# ---------------------------------------------------------------- distro family
detect_family() {   # attribute a system file to exactly one package manager
    local probe hits=""
    for probe in /usr/lib/os-release /etc/os-release /bin/sh; do [ -e "$probe" ] && break; done
    if command -v dpkg-query >/dev/null 2>&1 && dpkg-query -S "$probe" >/dev/null 2>&1; then hits="$hits debian"; fi
    if command -v rpm >/dev/null 2>&1 && rpm -qf "$probe" >/dev/null 2>&1; then hits="$hits fedora"; fi
    if command -v pacman >/dev/null 2>&1 && pacman -Qo "$probe" >/dev/null 2>&1; then hits="$hits arch"; fi
    hits=${hits# }
    case "$hits" in
        debian|fedora|arch) echo "$hits" ;;
        "") if command -v apt-get >/dev/null 2>&1; then echo debian
            elif command -v dnf >/dev/null 2>&1; then echo fedora
            elif command -v pacman >/dev/null 2>&1; then echo arch
            else die "cannot detect the package manager (apt, dnf or pacman); install the dependencies yourself and use --no-deps"; fi ;;
        *) die "ambiguous package manager for $probe ($hits)" ;;
    esac
}
owner_of() {   # the distro package owning a path, or nothing
    case "$FAMILY" in
        debian) dpkg-query -S "$1" 2>/dev/null | head -1 | cut -d: -f1 ;;
        fedora) rpm -qf "$1" 2>/dev/null | grep -v 'not owned' | head -1 ;;
        arch) pacman -Qqo "$1" 2>/dev/null | head -1 ;;
    esac
}
FAMILY=$(detect_family)
info "package manager family: $FAMILY"

if [ "$DEPS" = 1 ] && [ "$CHECK" = 0 ]; then
    case "$FAMILY" in
        debian)
            apt-get update -q
            apt-get install -yq lxc python3 python3-gi python3-dbus nftables dnsmasq gir1.2-gtk-3.0 \
                binutils e2fsprogs util-linux zstd curl git make patch \
                libepoxy0 libdrm2 libgbm1 libx11-6 libexpat1 libvulkan1
            apt-get install -yq python3-gbinder || die "python3-gbinder is not available: add the Waydroid repository
(curl -fsSL https://repo.waydro.id | sudo bash) and re-run" ;;
        fedora)
            sepkgs=()
            ! selinux_on || sepkgs=(container-selinux)   # the waydroid policy module requires its types
            dnf install -y lxc python3 python3-gobject python3-dbus python3-gbinder nftables dnsmasq gtk3 \
                binutils e2fsprogs util-linux zstd curl git make patch \
                libepoxy libdrm mesa-libgbm libX11 expat vulkan-loader "${sepkgs[@]}" ;;
        arch)
            # Install only what is missing: `pacman -S --needed` would also UPGRADE installed-but-older packages one
            # by one, a partial upgrade that breaks other packages (e.g. expat vs lib32-expat). Never upgrade here.
            arch_pkgs=(lxc python python-gobject python-dbus python-gbinder nftables dnsmasq gtk3 \
                binutils e2fsprogs util-linux zstd curl git make patch libepoxy libdrm mesa libx11 expat vulkan-icd-loader)
            mapfile -t arch_missing < <(pacman -T "${arch_pkgs[@]}" || true)
            if [ "${#arch_missing[@]}" -gt 0 ]; then
                echo "==> installing missing packages: ${arch_missing[*]}"
                pacman -S --noconfirm "${arch_missing[@]}" \
                    || die "pacman could not install ${arch_missing[*]} without upgrading other packages; bring the system up to date first (sudo pacman -Syu), then re-run this script"
            fi ;;
    esac
fi
need=(curl tar zstd gzip sha256sum python3 git)          # verification (--check needs only these)
[ "$CHECK" = 1 ] || need+=(make patch readelf debugfs)
for tool in "${need[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required (install it${DEPS:+, or drop --no-deps}; --check installs no packages)"
done
# the tools and Waydroid's generator need Python 3.8 or newer (assignment expressions); Waydroid itself needs 3.7
[ "$CHECK" = 1 ] || python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' ||
    die "Python 3.8 or newer is required, $(command -v python3) is $(python3 -V 2>&1)"

# ---------------------------------------------------------------- assets
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
fetch() { curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --connect-timeout 15 -sS -o "$2" "$1"; }
SRC=$(cd "$HERE/.." && pwd)
[ -f "$SRC/packaging/releases/release.env" ] || die "run this script from a waydroid-nvidia-optimized checkout"
# shellcheck source=releases/release.env
source "$SRC/packaging/releases/release.env"
TAG=$BASE_TAG GH_REPO=$BASE_PROVENANCE_REPO
info "base release $TAG, overlay $OVERLAY"
ASSETS=(waydroid-nvidia-host-x86_64-$TAG.tar.zst waydroid-nvidia-guest-android-x86_64-$TAG.tar.zst
        waydroid-nvidia-guest-prebuilts-$TAG.tar.zst waydroid-nvidia-native-x86_64-$TAG.tar.zst
        waydroid-nvidia-vendor-x86_64-$TAG.zip waydroid-nvidia-vendor-ota-$TAG.json waydroid-nvidia-selinux-$TAG.pp)
SEPP=waydroid-nvidia-selinux-$TAG.pp
# pinned sha256 of a file name, or death: an unpinned file is never used
pin() { awk -v f="$1" '$2 == f { print $1; n++ } END { exit n != 1 }' "$SRC/packaging/releases/SHA256SUMS" ||
        die "packaging/releases/SHA256SUMS has no single line for $1"; }
get() {   # get NAME URL_BASE...: from --from DIR or the first source with the pinned bytes
    local f=$1 sha; shift
    sha=$(pin "$f")
    if [ -n "$FROM" ]; then
        [ -f "$FROM/$f" ] || die "$FROM/$f missing"; cp -- "$FROM/$f" "$WORK/$f"
        echo "$sha  $WORK/$f" | sha256sum -c --quiet - || die "$FROM/$f does not match its pinned sha256"
    else
        info "downloading $f"
        python3 -I "$SRC/tools/fetch.py" --out "$WORK/$f" --sha256 "$sha" "${@/%//$f}" || die "cannot download $f"
    fi
}
for f in "${ASSETS[@]}"; do get "$f" "${BASE_URLS[@]}"; done
get "$OVERLAY" "${OVERLAY_URLS[@]}"
info "${#ASSETS[@]} base assets and the overlay match packaging/releases/SHA256SUMS"
# Sigstore attestation bundles: a missing one only means cosign cannot be used
for f in "${ASSETS[@]}"; do
    if [ -n "$FROM" ]; then [ ! -f "$FROM/$f.sigstore.json" ] || cp -- "$FROM/$f.sigstore.json" "$WORK/"
    else for u in "${BASE_URLS[@]}"; do fetch "$u/$f.sigstore.json" "$WORK/$f.sigstore.json" 2>/dev/null && break; rm -f "$WORK/$f.sigstore.json"; done
    fi
done
# Build provenance. 1) cosign against the release's Sigstore bundles: no GitHub account needed; the signing
# certificate must name this repository's build.yml at this tag, issued for GitHub Actions. cosign before 2.4.3
# cannot read the bundle format GitHub writes (v0.3), so it does not count as a verifier.
# 2) gh attestation verify needs a logged-in GitHub CLI. Under sudo root usually isn't logged in, so run gh as the
# user who invoked sudo when that user is; an unauthenticated gh is "not checked", never "does not verify".
verifier="" why=""
have_bundles=1
for f in "${ASSETS[@]}"; do [ -f "$WORK/$f.sigstore.json" ] || have_bundles=0; done
cosign_ver=""
if command -v cosign >/dev/null 2>&1; then
    cosign_ver=$(cosign version --json 2>/dev/null | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["gitVersion"].lstrip("v"))' 2>/dev/null || true)
fi
if [ "$have_bundles" = 1 ] && [ -n "$cosign_ver" ] &&
   [ "$(printf '%s\n' 2.4.3 "${cosign_ver%%[-+]*}" | sort -V | head -1)" = 2.4.3 ]; then
    verifier=cosign
fi
gh_run=(gh)
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ] && command -v runuser >/dev/null 2>&1; then
    gh_run=(runuser -u "$SUDO_USER" -- gh)
    chmod -R a+rX "$WORK"
fi
if [ -z "$verifier" ]; then
    if [ -n "$FROM" ]; then why="local assets from $FROM without cosign >= 2.4.3 and their .sigstore.json bundles"
    elif ! command -v gh >/dev/null 2>&1; then why="neither cosign >= 2.4.3${cosign_ver:+ (have $cosign_ver)} nor the GitHub CLI (gh) installed"
    elif ! "${gh_run[@]}" auth status >/dev/null 2>&1; then why="no cosign >= 2.4.3${cosign_ver:+ (have $cosign_ver)}, and gh is not logged in (run: gh auth login)"
    else verifier=gh
    fi
    [ "$have_bundles" = 1 ] || [ -n "$FROM" ] || why="$why; this release ships no .sigstore.json bundles"
fi
case "$verifier" in
    cosign)
        for f in "${ASSETS[@]}"; do
            # cosign refuses blobs above COSIGN_MAX_ATTACHMENT_SIZE (default 128 MiB; the vendor zip is larger)
            out=$(COSIGN_MAX_ATTACHMENT_SIZE=$(( $(stat -c %s "$WORK/$f") + 1 )) cosign verify-blob-attestation --bundle "$WORK/$f.sigstore.json" --type slsaprovenance1 \
                --certificate-identity "https://github.com/$GH_REPO/.github/workflows/build.yml@refs/tags/$TAG" \
                --certificate-oidc-issuer https://token.actions.githubusercontent.com "$WORK/$f" 2>&1) ||
                die "build provenance of $f does not verify (cosign verify-blob-attestation --bundle $f.sigstore.json): ${out##*$'\n'}"
        done
        info "build provenance verified (cosign $cosign_ver, Sigstore bundles: build.yml at $TAG in $GH_REPO)" ;;
    gh)
        for f in "${ASSETS[@]}"; do
            "${gh_run[@]}" attestation verify "$WORK/$f" --repo "$GH_REPO" >/dev/null ||
                die "build provenance of $f does not verify (gh attestation verify)"
        done
        info "build provenance verified (gh attestation verify)" ;;
    *)  info "build provenance NOT verified ($why): pinned SHA256SUMS only" ;;
esac
RECORD_COMMIT=$(tar --zstd -xOf "$WORK/waydroid-nvidia-native-x86_64-$TAG.tar.zst" ./BUILD-RECORD.json |
                python3 -I -c 'import json,sys; print(json.load(sys.stdin)["commit"])')
[ "$RECORD_COMMIT" = "$BASE_COMMIT" ] || die "the native bundle was built from $RECORD_COMMIT, this tree pins $BASE_COMMIT"
info "native bundle build commit $RECORD_COMMIT = pinned BASE_COMMIT"
python3 "$SRC/packaging/releases/extract-bundle.py" "$WORK/$OVERLAY" "$WORK/overlay" || die "$OVERLAY is not a complete overlay"
if [ "$CHECK" = 1 ]; then info "--check: verified, nothing installed"; exit 0; fi

# ---------------------------------------------------------------- stage into a package root
ROOT=$WORK/root
mkdir -p "$ROOT"
info "patched waydroid ($WAYDROID_SHA + patches/waydroid/0001 + waydroid/0001-lxc-nvidia-native-mounts)"
fetch "$WAYDROID_UPSTREAM/archive/$WAYDROID_SHA.tar.gz" "$WORK/waydroid.tar.gz" || die "cannot download waydroid $WAYDROID_SHA"
echo "$WAYDROID_TARBALL_SHA256  $WORK/waydroid.tar.gz" | sha256sum -c --quiet || die "waydroid source tarball checksum mismatch"
mkdir -p "$WORK/waydroid" && tar -xzf "$WORK/waydroid.tar.gz" -C "$WORK/waydroid" --strip-components=1
patch -s -d "$WORK/waydroid" -p1 < "$SRC/patches/waydroid/0001-nvidia-integration.patch"
patch -s -d "$WORK/waydroid" -p1 < "$SRC/waydroid/0001-lxc-nvidia-native-mounts.patch"
cmp -s "$SRC/waydroid/nvidia_native.py" "$WORK/waydroid/tools/helpers/nvidia_native.py" || die "patched generator differs from the release's"
NFT=1; command -v nft >/dev/null 2>&1 || NFT=0
make -s -C "$WORK/waydroid" install DESTDIR="$ROOT" USE_NFTABLES=$NFT >/dev/null
# an absolute interpreter that has Waydroid's bindings: `#!/usr/bin/env python3` follows the caller's PATH
PYBIN=""
for cand in /usr/bin/python3 /usr/local/bin/python3; do
    [ -x "$cand" ] && "$cand" -c 'import dbus, gi, gbinder' >/dev/null 2>&1 && { PYBIN=$cand; break; }
done
[ -n "$PYBIN" ] || die "no system python3 with the dbus, gobject and gbinder bindings (install python3-dbus, python3-gi/python3-gobject, python3-gbinder)"
"$PYBIN" -c 'import sys; sys.exit(sys.version_info < (3, 8))' || die "$PYBIN is $("$PYBIN" -V 2>&1); Python 3.8 or newer is required"
sed -i "1s|^#!.*python3.*$|#!$PYBIN|" "$ROOT/usr/lib/waydroid/waydroid.py"

info "host Venus renderer, guest stack, nvidia-prolapse vendor image, bundle and tools"
mkdir -p "$WORK/host" "$WORK/guest"
tar --zstd -xf "$WORK/waydroid-nvidia-host-x86_64-$TAG.tar.zst" -C "$WORK/host"
tar --zstd -xf "$WORK/waydroid-nvidia-guest-android-x86_64-$TAG.tar.zst" -C "$WORK/guest"
tar --zstd -xf "$WORK/waydroid-nvidia-guest-prebuilts-$TAG.tar.zst" -C "$WORK/guest"
info "optimized Venus components from $OVERLAY"
O=$WORK/overlay
for f in virgl_test_server virgl_render_server libvirglrenderer.so.1; do cp -- "$O/$f" "$WORK/host/$f"; done
cp -- "$O/vulkan-x86_64.so" "$WORK/guest/vendor/lib64/hw/vulkan.virtio.so"
cp -- "$O/vulkan-x86.so" "$WORK/guest/vendor/lib/hw/vulkan.virtio.so"
cp -- "$O/libgbm_mesa_wrapper.so" "$WORK/guest/vendor/lib64/libgbm_mesa_wrapper.so"
# perf-v1's hwcomposer predates hwcomposer/patches 0002-0017: the base release's stays
for f in virgl_test_server virgl_render_server libvirglrenderer.so.1; do
    install -Dm755 "$WORK/host/$f" "$ROOT$PREFIX/$f"
done
for rel in vendor/lib/hw/vulkan.virtio.so vendor/lib/egl/libEGL_angle.so vendor/lib/egl/libGLESv1_CM_angle.so \
           vendor/lib/egl/libGLESv2_angle.so vendor/lib64/hw/vulkan.virtio.so vendor/lib64/egl/libEGL_angle.so \
           vendor/lib64/egl/libGLESv1_CM_angle.so vendor/lib64/egl/libGLESv2_angle.so \
           vendor/lib64/libgbm_mesa_wrapper.so vendor/lib64/hw/hwcomposer.waydroid.so; do
    [ -f "$WORK/guest/$rel" ] || die "release guest payload lacks $rel"
    install -Dm644 "$WORK/guest/$rel" "$ROOT$PREFIX/guest/$rel"
done
install -Dm755 "$WORK/guest/system/bin/surfaceflinger" "$ROOT$PREFIX/guest/system/bin/surfaceflinger"
bash "$SRC/packaging/native/install-native.sh" "$SRC" "$WORK/waydroid-nvidia-native-x86_64-$TAG.tar.zst" \
    "$WORK/waydroid-nvidia-vendor-x86_64-$TAG.zip" "$WORK/waydroid-nvidia-vendor-ota-$TAG.json" "$ROOT" "$TAG"
A=$SRC/packaging/aur/waydroid-nvidia-bin
install -Dm644 "$A/wd-venus.service" "$ROOT/usr/lib/systemd/user/wd-venus.service"
install -Dm644 "$A/waydroid-venus.tmpfiles" "$ROOT/usr/lib/tmpfiles.d/waydroid-venus.conf"
install -Dm644 "$A/waydroid-nvidia.rules" "$ROOT/usr/lib/udev/rules.d/70-waydroid-nvidia.rules"
install -Dm644 "$A/waydroid-nvidia-gpu.rules" "$ROOT/usr/lib/udev/rules.d/70-waydroid-nvidia-gpu.rules"
install -Dm755 "$A/waydroid-nvidia-setup" "$ROOT/usr/bin/waydroid-nvidia-setup"
install -Dm755 "$SRC/packaging/install.sh" "$ROOT$PREFIX/install.sh"     # for --uninstall

# ---------------------------------------------------------------- conflicts, copy, file list
(cd "$ROOT" && find . \( -type f -o -type l \) | sed 's#^\.##' | sort) > "$WORK/files"
conflicts=()
while IFS= read -r f; do
    if [ -e "$f" ] || [ -L "$f" ]; then
        pkg=$(owner_of "$f" || true)
        [ -z "$pkg" ] || conflicts+=("$f ($pkg)")
    fi
done < "$WORK/files"
[ ${#conflicts[@]} -eq 0 ] || die "these files belong to distro packages; remove them first (e.g. the distro's waydroid):
  ${conflicts[*]:0:5}"
if [ -f "$LIST" ]; then   # upgrade: drop files the previous release had and this one does not
    comm -23 <(sort "$LIST") "$WORK/files" | while IFS= read -r f; do rm -f -- "$f"; done
fi
# existing directories keep their owner, mode and times (--no-overwrite-dir)
tar -C "$ROOT" -cf - . | tar -C / -xpf - --no-overwrite-dir --no-same-owner
mkdir -p "$STATE" && cp "$WORK/files" "$LIST"
if selinux_on; then
    # semodule names a module after its file: install it as waydroid.pp
    mkdir "$WORK/selinux" && cp "$WORK/$SEPP" "$WORK/selinux/waydroid.pp"
    semodule -X 400 -i "$WORK/selinux/waydroid.pp" ||
        die "semodule could not load the waydroid policy module (see above); without it an enforcing host refuses the container"
    echo "waydroid 400" > "$SEMODULE"
    relabel
    info "SELinux: policy module waydroid loaded (priority 400); /usr/lib/waydroid, /var/lib/waydroid{,-nvidia} relabelled"
fi
systemd-tmpfiles --create /usr/lib/tmpfiles.d/waydroid-venus.conf 2>/dev/null || true
# What a package manager's post-install hooks would do: the running services must see the new files.
# Without the DBus reload the container manager's bus name is refused by the old policy and
# waydroid-container.service fails to start ("Request to own name refused by policy").
udevadm control --reload 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
systemctl reload dbus.service 2>/dev/null || systemctl reload dbus-broker.service 2>/dev/null \
    || die "could not reload the system DBus configuration; run: sudo systemctl reload dbus (or reboot), then sudo waydroid-nvidia-setup"
info "installed $(wc -l < "$LIST") files (list: $LIST; remove with: sudo $PREFIX/install.sh --uninstall)"
cat <<'EOF'

Next (README steps 2 and 3):
  sudo waydroid-nvidia-setup     (downloads the Android image if needed, checks your GPU, installs the vendor image,
                                  starts the container; add -s GAPPS for Google apps on a fresh install)
  waydroid session start
EOF
