# Explicit Rust profiles for the source-native stack

The default remains `dev` (Cargo's `debug/` output). Select `release` explicitly
for an optimized candidate. Both profiles preserve panic unwinding: worker
failure must still publish device loss and run its ownership cleanup. Neither
profile selection nor a successful build proves runtime correctness.

Build all three production targets with one profile:

```sh
for target in host android android32; do
  python3 nvidia-prolapse/driver-source/native/mesa/build.py \
    --target "$target" --rust-profile release
done
```

Each successful Rust + Mesa link writes `build/nvk-TARGET/rust-build.json` with
the profile, Cargo command, compiler, any inherited Rust flag environment, and
exact Rust/NVK artifact paths and SHA256 identities. It also snapshots Rust
sources, Cargo files, canonical bridge C/headers and build/profile tooling before
and after the build; a concurrent edit prevents stamping metadata. Gates reject
current source inputs that differ from that successful build. `dev` maps to `debug/`;
`release` maps to `release/`, under the existing target-specific Cargo directory.
Mesa build directories remain shared: rebuilding one profile replaces the
active target's metadata and linked Mesa artifact. A profile mismatch, missing
metadata or changed artifact is a named error requiring an explicit rebuild.
The gate never guesses a profile from files that happen to exist.

Check the selected runtime commands without executing them:

```sh
python3 tools/native-vulkan-regression.py --rust-profile release
```

After coordinating GPU use, add `--run` to execute the gate. Gate logs record
`rust-builds.json`. Staging uses the same explicit choice and runs the gate
before copying the verified bytes:

```sh
sudo -n dev/source-native stage /var/lib/waydroid/nvidia-source-native-release \
  --rust-profile release
```

`adopt` also accepts the selector, but recorded sources must match the selected
build paths. Existing deployed manifests/check/activate behavior is unchanged.
New staging manifests record profile and per-target build identities.

`tools/vast/vast.py bundle --rust-profile release` validates and packages the
selected production DSOs; `--build` first builds those profiles. Its separate
invariant/test executables remain explicitly labeled `dev` in the bundle
manifest. This command does not rent or run a remote machine.

Report optimized-candidate measurements distinctly from a dev baseline.
Changing Rust optimization is not a matched-compiler A/B comparison. Release
runtime gates remain mandatory, including worker-loss/cleanup behavior.
