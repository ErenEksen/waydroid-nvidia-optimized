# Cutting a release

Everything is CI-built and attested; there are no manual asset uploads.
Prereq (one-time, already done): the self-hosted runner with the `lineage`
label must be registered and online — it builds ANGLE (persistent
`~/angle-ci` checkout) and surfaceflinger (from the lineage-20 tree on the
runner host).

CI never runs on branch pushes or pull requests, only on `v*` tags and by hand
(`gh workflow run <workflow> --ref <branch>`; docs/dev-setup.md, "CI").

1. **Preflight** — run the full pipeline on the branch you will tag, without
   releasing (also exercises the self-hosted jobs and the vendor image):
   ```sh
   gh workflow run build.yml --ref <branch>
   gh run watch $(gh run list --workflow build.yml --branch <branch> -L 1 --json databaseId -q '.[0].databaseId') --exit-status
   ```
   Debug any red job locally/hermetically first (container with CI parity);
   don't iterate by pushing.

   Confirm the Mesa matrix produced both `guest-vulkan-virtio-x86` and
   `guest-vulkan-virtio-x86_64`, and inspect the assembled archives before
   tagging. The guest tree must contain separate `vendor/lib` and
   `vendor/lib64` Vulkan/ANGLE files; no app-facing `.so` may be flat.

   Tag runs restore GitHub caches only from their own ref or the default branch (`main`), not from the branch
   you preflighted. Without a cache the hosted hwcomposer job provisions cold: it downloads the 1.2 GB OTA system
   image from SourceForge (`hwcomposer/pins.env`, `IMG_*`), 3 min on a good mirror; `tools/fetch.py` gives up a
   mirror below 2 MiB/s and the job times out after 45 min. To start the tag run warm, save the cache on `main`
   first (hosted jobs only, after `main` carries the same `hwcomposer/pins.env` and `provision.sh`):
   ```sh
   gh workflow run build.yml --ref main -f self_hosted=false
   ```

2. **Tag & push** — the release job rebuilds at the tag, assembles all four
   tarballs (host, guest, guest-prebuilts, native), the nvidia-prolapse vendor image built from that run's
   native bundle and ANGLE (vendor-image.yml, called by build.yml; self-hosted lineage runner) as
   `waydroid-nvidia-vendor-x86_64-<tag>.zip` + its OTA channel `waydroid-nvidia-vendor-ota-<tag>.json`
   (`packaging/vendor-ota.py`), and SHA256SUMS, attaches SLSA attestations to every asset (also as
   `<asset>.sigstore.json` release files, listed in SHA256SUMS), and creates a **draft** release.
   `-rcN` tags become prereleases; plain version tags full releases:
   ```sh
   git tag v<X.Y.Z>[-rcN] && git push origin v<X.Y.Z>[-rcN]
   ```
   Before tagging, commit `release-notes/<tag>.md`: what changed for users, in plain words, and the install
   commands (see the earlier files there). The release job refuses a tag without it.

3. **Test the draft like a user, then publish.** Drafts are invisible to users and to install.sh.
   ```sh
   gh release download <tag> -D /tmp/rel && curl -fLo /tmp/rel/waydroid-nvidia-<tag>.tar.gz \
     https://github.com/Shiro836/waydroid-nvidia/archive/refs/tags/<tag>.tar.gz
   sudo packaging/install.sh --tag <tag> --from /tmp/rel && sudo waydroid-nvidia-setup
   # reboot; the container must come up by itself; then:
   gh release edit <tag> --draft=false
   ```
   The channel JSON must stay published once released: installs point `vendor_ota` at it.

4. **PKGBUILD**: bump `pkgver`/`_tag` in
   `packaging/aur/waydroid-nvidia-bin/PKGBUILD` (the seven release downloads carry zero sums in
   the tree until then: source tarball, host, guest-android, guest-prebuilts, native, vendor zip, vendor channel), then `updpkgsums`, test
   `makepkg -s` from a clean copy (run it in a temp dir — `makepkg` litters
   `src/`/`pkg/` and stray dirs break naive `cp *` chains), commit.

5. **AUR**: `makepkg --printsrcinfo > .SRCINFO`; copy PKGBUILD, .SRCINFO and
   the local source files (service, tmpfiles, setup, install, udev rules, driver hook + note) to
   the AUR clone at `~/repos/waydroid-nvidia-bin-aur`, commit, push.

Verify any asset (with a GitHub login, or offline from the `<asset>.sigstore.json` the release attaches next to
each asset; the cosign form needs no account, docs/install-manual.md):
```sh
gh attestation verify <asset> --repo Shiro836/waydroid-nvidia
gh attestation verify <asset> --bundle <asset>.sigstore.json --repo Shiro836/waydroid-nvidia
cosign verify-blob-attestation --bundle <asset>.sigstore.json --type slsaprovenance1 \
  --certificate-identity https://github.com/Shiro836/waydroid-nvidia/.github/workflows/build.yml@refs/tags/<tag> \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com <asset>
```
