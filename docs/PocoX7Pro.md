<!--
SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# POCO X7 Pro

This fork prioritizes one device: the POCO X7 Pro, with its Dimensity 8400-Ultra
(MT6899), Cortex-A725 CPU, Mali-G720 GPU, and stock HyperOS Vulkan driver. Both
8 GB and 12 GB RAM variants are in scope. Other build targets remain as references;
new optimization decisions and the Android CI build target this phone.

Hardware references: [Xiaomi specifications](https://www.mi.com/global/product/poco-x7-pro/specs/),
[Xiaomi CPU details](https://www.mi.com/global/support/faq/details/KA-527968/), and
[Arm Mali-G720 documentation](https://support.arm.com/compute-ip/mali-g720).

## Build

The default Android flavor is `pocoX7Pro`, and the default build type is `Release`.
It requires Android 15/API 35 or later and produces only `arm64-v8a` binaries.
The app is labelled **Eden POCO X7 Pro**, with package `dev.steven.eden.pocox7pro`.
It can be installed alongside upstream Eden; its settings, games list, and app data
are separate. Import your data through the existing app import/export tools if needed.

Use JDK 17, Android SDK 36, NDK `29.0.14206865` (r29), and CMake `3.31.6`.

Install a host `glslangValidator` for shader generation as well. On Ubuntu this is
provided by `sudo apt-get install glslang-tools`; the Android cross compiler cannot
replace this host tool.

Build from the repository root:

```sh
.ci/android/build.sh
```

The APK and AAB are copied into `artifacts/`. The equivalent explicit target is:

```sh
.ci/android/build.sh -t poco -b Release
```

For a direct Gradle build, run from `src/android`:

```sh
./gradlew assemblePocoX7ProRelease
```

The **POCO X7 Pro Android** Actions workflow builds this same release on source
changes or manual dispatch. Download its `eden-poco-x7-pro-<commit>` artifact.
The workflow uses the repository's existing development signing key unless a local
build supplies signing credentials. It does not publish to a store or release page.

## Device profile

The flavor enables `EDEN_POCOPHONE_X7_PRO` across native libraries and enables LTO.
The native profile rejects non-Android and non-arm64 targets and checks that both
C and C++ compilers support the tuning flags. It uses Cortex-A725 scheduling with
`-march=armv8.2-a+fp16+dotprod+lse+rcpc -mtune=cortex-a725`, retaining NEON without
assuming that the Android kernel exposes SVE/SME. LTO and CPU tuning require device
benchmarks before attributing any speed improvement to them.

The profile opens Android's system `libvulkan.so` directly for rendering and GPU
queries. Custom Adreno/Turnip driver loading is disabled for this flavor; those
drivers target a different GPU. It retains Vulkan feature and format checks. The
upstream update checker is disabled so it cannot replace this build with an
upstream APK.

On native ASTC devices, texture-cache startup skips creation of the unused ASTC
compute decoder. Native compressed textures remain compressed, and devices without
native ASTC keep the GPU decoding fallback. This removes unnecessary pipeline
creation; it does not add missing Vulkan features or establish that this was the
cause of any particular game's failure.

CPU topology detection falls back to maximum clock speeds when capacities are
uniform, even when all CPU identification registers match. The POCO profile's
fallback performance group uses cores within 90% of the fastest core, with at least
four allowed cores. With the documented 3.25/3.0/2.1 GHz clusters this selects the
four fastest cores for latency-sensitive work and leaves the slower four for
background work. Android performance hints remain preferred; affinity grouping is
only the fallback. CPU numbers are discovered rather than hard-coded, and Android's
allowed CPU set is respected. If topology cannot be read, placement stays unrestricted.

Compute-pipeline errors report the actual GPU model instead of labelling every
driver failure as an Adreno error.

## Starting settings

Defaults favor a working rendering baseline. They are normal settings and can be
changed globally or per game; explicit saved overrides are preserved.

| Setting | Default | Reason |
| --- | --- | --- |
| CPU backend | NCE for supported titles | Existing native execution path |
| Graphics API | Vulkan | Existing Android backend |
| Console mode | Handheld | Existing lower-resolution workload |
| Resolution | 1x | Baseline for comparing game compatibility |
| GPU accuracy | High | Correct synchronization before testing lower accuracy |
| Reactive flushing | On | CPU/GPU readback correctness baseline |
| ASTC recompression | Uncompressed | Retain native ASTC where supported; avoid BC recompression assumptions |
| VRAM usage | Conservative | Existing conservative cache policy |
| Force maximum GPU clocks | Off | Avoid extra GPU work from the clock-boost path |
| Shader and driver pipeline caches | On | Existing persistent compilation caches |

Only GPU accuracy and reactive flushing change from the upstream Android defaults.
High accuracy can cost FPS; it is a correctness choice, not a claimed speedup.
For a GPU-bound title, compare 0.75x and 0.5x resolution before enabling unsafe CPU
settings. Leave memory expansion, driver workarounds, and unsupported feature
overrides out of the baseline comparison.

## Validation on the phone

No phone benchmark or Mali driver validation is available from the development
container. Do not describe this profile as measured faster or broadly compatible
until the following tests have been recorded.

For each test, record game name/version, Eden commit, phone RAM, HyperOS/Android
version, Vulkan driver version, settings, and whether shader caches were cold or
warm. Compare the same save/scene against the unmodified fork. Check sustained
behavior for at least 15 minutes, including frame times, temperature, artifacts,
memory pressure, and whether the app is killed or crashes. Test NCE and JIT titles
separately, as NCE is not available for every guest binary.

| Case | Required check | Device result |
| --- | --- | --- |
| Cold launch | App opens and reports Mali-G720/system driver | Pending |
| Native ASTC game | Loads textures correctly without the unused decoder pipeline | Pending |
| Previously failing game | Boot, load, gameplay, and exported error log | Pending |
| CPU-heavy scene | Sustained timing and placement with/without usable Android hints | Pending |
| GPU-heavy scene | 1x/0.75x/0.5x comparison without changing accuracy simultaneously | Pending |
| 8 GB model | Extended play and memory pressure | Pending |
| Per-game override | Save/relaunch preserves explicit accuracy and flushing choices | Pending |

Export the existing Eden log after a failing run. A game name plus the log is needed
to distinguish a shader compiler failure, an unsupported feature, memory exhaustion,
or a CPU/emulation issue. This profile cannot promise to fix all games merely by
changing the build target.
