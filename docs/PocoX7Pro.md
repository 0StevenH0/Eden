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


## Mali stability patch

The vertex-input path now queries and enables the divisor feature bits, and checks
`maxVertexAttribDivisor`. Ordinary instancing (divisor 1) uses core Vulkan without
attaching `VK_EXT_vertex_attribute_divisor` state. Only bindings referenced by
active attributes are emitted. Zero divisors remain instance-rate bindings and
require both divisor feature bits; they are not treated as per-vertex input.
Unsupported static pipelines and dynamic draws are skipped with an error before
submitting invalid vertex-input state. Repeated rejected dynamic draws remain
invalid until the guest changes their input state.

This is a compatibility guard, not software emulation of missing instancing
features. Games requiring unsupported zero or larger divisors can still have
missing geometry or fail to render. The existing capability warnings remain;
this patch does not supply Mali's other missing Vulkan features. The supplied
logs did not contain a crash stack, so this does not establish the crash cause.

GPU model and system-driver queries now read and cache physical-device metadata
without creating an Android surface, input subsystem, or renderer device. This
also removes the leaked window from the old GPU-model query. Failed queries
return a fallback and can be retried; successful queries are cached per process.
No FPS or device-memory improvement has been measured.

On Android 11/API 30 and newer, startup collects the four most recent Android
process-exit records on an IO worker. After a crash, reopen the app and choose
**Share log**. It packages the selected Eden log, `android_exit_info.txt`, and any
available traces into `eden_diagnostics.zip`. Native traces on Android 12 and newer
are protobuf `.pb` tombstones; other traces are text. Each trace is limited to
8 MiB. Oversized traces are omitted and labelled, rather than truncated into an
invalid protobuf. Older trace files are pruned. Android may have no trace available;
exit reasons and sampled PSS/RSS remain useful but do not prove the cause by
themselves. The report identifies its collection time and build. Startup and game transitions
also attach the exact Git revision, phase, and title ID to Android's exit record;
older builds may have no such session metadata. No per-frame logging is added.

Validation of this patch in the development container: extracted production
vertex-input and metadata-query code passed host C++ harness checks with address
and undefined-behavior sanitizers (Vulkan/engine calls mocked; leak detection was
unavailable in the container). Cases cover ordinary and zero divisors, capability
limits, unused bindings, repeated rejection and recovery, driver-properties
fallbacks, caching, and cleanup after failed queries. This does not compile the
whole app or validate JNI/Android behavior. The Android release build could not
start because the Gradle distribution, SDK, and NDK were unavailable and the
Gradle download failed with `Network is unreachable`. Kotlin compilation,
Android exit collection/export, and the actual Mali driver still need validation.

Build the same `assemblePocoX7ProRelease` target before installing. Reproduce the
same failing scene with the existing settings first, reopen after a failure, and
export the diagnostic ZIP. Then compare Accurate and Fast GPU modes separately,
and reactive flushing separately, keeping resolution and caches consistent. A
missing-divisor message calls for a renderer fallback; a native crash trace or
Android low-memory exit points to a different next investigation.


GitHub Actions also retains `eden-poco-debug-<commit>` for 90 days. It contains
native function symbols (`SYMBOL_TABLE`), the Java/R8 mapping, and a build manifest
with APK checksums and the native build ID. The collection step verifies that the
symbol file and the APK have the same native build ID. Keep this artifact for any
APK used in extended testing: stripped release APKs alone cannot recover function
names. These symbols support tombstones; source file/line information is not
promised by this symbol level. The release APK remains optimized and non-debuggable.

## Kernel TLS and migrating CPU fibers

The October 17 native traces exposed a CPU-runtime hazard. In the matched
`a9b74e3` release library, optimized SVC code retains a host thread-local-storage
(TLS) pointer across a wait that can move the guest fiber to another host thread.
An older trace faults on a TLS-relative load after a wait. The latest trace fails
return-address authentication in `std::mutex::lock`; a connection between that
failure and stale TLS has not been established.

Kernel TLS now resolves through a non-inlined accessor with a compiler fence. This
prevents the compiler from reusing a lookup across scheduling operations while
preserving NCE, multicore execution, Cortex-A725 tuning, LTO, and the existing GPU
settings. The compiler fence is not a hardware memory fence. The accessor adds a
small call cost; its effect on game frame times has not been measured.

A Linux host harness extracted the production TLS layout and accessor, replacing
only the unused kernel-thread payload with a stand-in. With `-O3 -flto`, a fiber
migrated between two OS threads returned stale host state with the original
lookup and the correct state with the new accessor. The repository also includes
`Kernel TLS follows a migrating guest fiber`, which checks host and physical-core
IDs using the real kernel and fiber APIs. The full test executable and Android
release have not been built in this environment; the host harness is not proof
that either native crash is resolved on the phone.

Build `assemblePocoX7ProRelease`, retain the matching debug artifact, and repeat
the same failing scene with NCE and the existing game settings. Compare sustained
frame times and time to failure against the previous release at the same
resolution and similar phone temperature, testing cold and warm caches
separately. Export a new diagnostic ZIP after any crash. If return-address
authentication still fails, investigate that trace independently instead of
disabling authentication or broad CPU optimizations.

The subsequent `6555ede` Android release built successfully in GitHub Actions.
Its optimized ARM64 library retains the TLS accessor across the inspected wait
path, and the user reports that Unicorn Overlord now reaches playable gameplay.
This is a limited device result, not a sustained performance benchmark or proof
that all crash paths are fixed.

## Audio delivery and crackling

Unicorn Overlord's reported crackling prompted fixes to the audio sink. The
producer now publishes a buffer descriptor only after its samples and queue count
are ready. Short reads advance playback only by the frames actually copied, and
underruns hold the most recently delivered frame rather than a previous callback's
frame. These changes prevent premature consumption and stale output samples.

The SDL callback supplies complete PCM frames only when SDL requests additional
data. It reuses storage across callbacks, growing it only for larger requests.
This avoids repeated temporary allocations without increasing the audio queue or
changing the Android driver, audio gain, NCE, LTO, or Cortex-A725 tuning. Phone CPU
cost and audible improvement still require device testing.

Run the focused host regression checks with:

```sh
python3 .ci/android/test-audio-sink.py --sanitize
```

The harness compiles the production sink methods and real queue containers with
Core/SDL stand-ins. It exercises a producer/consumer publication boundary, short
reads, underrun padding, SDL byte requests, storage reuse, pausing, and capture.
The original code fails six checks; the patched code passes all eight, including
AddressSanitizer and UndefinedBehaviorSanitizer. This does not test Android audio
driver timing. The POCO workflow runs the host checks before building the APK.

Compare the same scene and settings on the phone. After reproducing crackling,
exit to the game list and choose **Share log**. Stopping an SDL output stream now
logs cumulative `underrun frames` and `short reads`; no callback logging is added.
Underruns can also occur during startup or slow emulation, so their totals alone
do not establish the cause of a particular pop. A recording and the corresponding
log help separate delivery starvation from other mixing or decoding problems.

The `f9860e3` diagnostic session at 100% speed recorded 46,560 missing frames
(970 ms at 48 kHz), including a 5 ms startup underrun, over 101 seconds of reports.
The largest submission gap was 549,899 us. Recorded underruns had no pending
buffers while callbacks continued. This establishes delivery starvation, but does
not locate the upstream stall. Near-full-scale and large-step PCM counters are
heuristics, not proof of malformed audio. The session's ending guest panic and
native crash have not been linked to the audio dropouts.

AudioOut now copies guest samples into its existing reusable scratch buffer before
the sink performs in-place downmixing or gain changes. The old direct guest-memory
span allowed those operations to overwrite the game's original samples. Two host
regressions reproduce that overwrite and verify unchanged guest data, repeatable
playback, and storage reuse with the correction; all eleven host checks pass with
address/undefined-behavior sanitizers. This fixes a demonstrated memory ownership
bug, not a confirmed cause of the observed starvation or waveform flags.

For Unicorn Overlord, `AudioOut upstream` reports long gaps between append,
manager, and released-buffer collection calls, separating time waiting for the
manager mutex from time doing work. Each operation reports at most once per
250 ms, only after a gap of 40 ms or a wait/work duration of 20 ms. These reports
run on service/manager threads, not the playback callback. Compare their host
timestamps with underrun events. Other titles do not take these extra timestamps.
The 25 ms lead, output backend, gains, and CPU/GPU settings are unchanged; audible
improvement and the cost of the extra copy/timestamps need phone validation.

The subsequent `1351ed0` phone run was reported as much less noisy. Its 140.8
seconds of audio reports contain zero near-full-scale or large-step PCM flags,
versus 186 and 151 in the earlier run. These are different sessions, not a
controlled sound-quality benchmark. Remaining underruns total 1,310 ms, with
1,030 ms concentrated in two early reporting windows. The final roughly 82
seconds have no reported underruns. Submission gaps of 77--583 ms overlap audio
manager gaps, while the traced append work takes only 9--40 us. This points the
next investigation toward upstream scheduling; it does not prove that rendering
causes the audio stalls. The exported crash trace belongs to the older `f9860e3`
run, not a new crash in this session.

The POCO build now adds focused diagnostic points: `Timing stall` identifies
late timer events, slow callbacks, and waits to reacquire the event-queue lock;
`Frame timing gap` reports emulator system-frame gaps of at least 50 ms; and
`Graphics pipeline wait` reports waits of at least 20 ms for an unfinished shader
pipeline. `Graphics pipeline creation` covers synchronous shader translation and
setup on a runtime cache miss taking at least 20 ms, before that wait can begin.
Host timestamps allow correlation with audio events. Timer reports are
limited to four per second for each of slow work and late dispatch, and frame
reports to four per second. Timer measurements add two clock reads per executed
event; frame reports reuse the existing timestamp, and shader timing is taken
only on runtime cache misses and the pipeline wait path. This build does not
change scheduling, shader asynchrony, graphics accuracy, or buffering. Phone
overhead and the source of the remaining stalls still need measurement. Startup
and intentional pauses can
produce gaps; late timer deadlines spanning a known pause are excluded.

## GPU wait failure accounting

The `429e1db` Unicorn Overlord run contains 965 ms of missing audio over 70.64
seconds of reporting, with 850 ms concentrated in two windows. The near-full-scale
and large-step PCM counters remain zero. Frame gaps of 869, 652, and 436 ms overlap
audio submission gaps, while the corresponding append work takes 19--36 us.
There are no timer-stall or slow-pipeline reports. Other frame gaps occur without
audio starvation, so one correction cannot yet be claimed to resolve all lag.

Source and release-binary inspection identify an internal pause path in NV event
waiting: after three recorded failures, the fallback pauses guest execution and
CoreTiming while waiting for the actual host GPU fence. Audio callbacks continue
during this pause. The existing timer diagnostics intentionally exclude deadlines
spanning a pause, so they cannot confirm or rule out this path in that phone run.

Two failure-accounting errors are reproduced on the host: successful asynchronous
completion does not reset the counter, and clearing an already completed or idle
event increments it. These errors can make old or nonexistent failures trigger a
later application pause. The next wait now resets the counter when the event is
signalled, under the existing event mutex, and clearing counts a failure only
when it cancels a pending wait. Three consecutive pending cancellations still
enter the existing fallback and wait for the requested fence. No GPU completion
is fabricated, and scheduling, graphics accuracy, and audio buffering are unchanged.

Run the focused checks with:

```sh
python3 .ci/android/test-nvhost-events.py --sanitize
```

The harness compiles the production wait and clear methods with controlled event,
syncpoint, and system stand-ins. Four cases fail against the previous code. All
seven pass with the correction and address/undefined-behavior sanitizers, including
checks that retain the fallback, requested fence, pause/resume, ready-fence fast
path, and zero-timeout behavior. These checks do not model ioctl packing, Android
scheduling, or Mali performance. The POCO workflow runs them before the APK build.

The POCO build also logs `NV syncpoint fallback` when this path takes at least
20 ms, separating pause acquisition from the host fence wait and total duration.
Its clock reads occur only when entering that fallback. Test the same scene and
settings and export diagnostics after a hitch. This is a demonstrated accounting
fix; whether this path caused the observed phone stalls, and the amount of any
audio or frame-time improvement, still require device measurements.

## Remaining GPU and kernel timer stalls

The subsequent `073654e` Unicorn Overlord log confirms two fallback host waits of
566 ms and 332 ms. The pause-acquisition work takes only 49 us and 82 us. The two
overlapping audio-report windows account for 865 ms of the run's 1,000 ms of missing
audio over 68.393 seconds. This confirms the fallback contributes to the dropouts;
it does not establish whether GPU execution, queued submissions, cache readback,
host scheduling, or completion callbacks are delaying the signal. There are no
slow shader compilation reports or PCM clipping/large-step flags in this run.

A separate `KHardwareTimer::Callback` takes 104 ms and overlaps another 90 ms of
missing audio. The following HID event is late, but its own work takes only 7 us.
The exported exit history has no new native crash record for this build. The
ending guest panic and memory-unmap assertion remain unresolved; they cannot be
classified as a spontaneous gameplay crash without knowing how the session ended.

The next POCO diagnostics separate these stages without changing fence completion,
fallback pauses, cache flush ordering, audio buffering, or scheduler policy:

| Log marker | Measured stages |
| --- | --- |
| `Vulkan submission stall` | Time from recording a submission callback to executing it, command-buffer preparation and submit callback, queue-submit mutex acquisition, and driver submission |
| `GPU fence enqueue` | Flush checks, cache commit, fence queueing and command flushing, and cache invalidation |
| `GPU fence release` | Fence creation to dequeue, completion wait, cache release, host signalling callbacks, and deferred retirement |
| `GPU cache release` | Buffer/texture cache lock acquisition, texture readback, buffer readback, and query release |
| `Kernel timer stall` | Scheduler lock acquisition, timer lock acquisition, expired-task handling, and scheduler unlock/rescheduling |

Each category reports durations of at least 20 ms, at most four times per second.
`host_us` is the monotonic start timestamp; add `total_us` to obtain the end. A fence's
`queued_us` includes backend queueing and time behind earlier fences. A submission's
`queued_us` includes earlier commands in the same worker chunk and the worker queue.
Neither field is a measurement of physical GPU utilization. Cache `query_us` also
includes releasing the buffer/texture locks; timer `tasks_us` includes releasing the
timer lock. Reports are emitted after those measured locks are released.

All new sampling is compiled only into the POCO flavor. It adds five clock reads per
kernel timer callback, one per fence creation, five per asynchronous fence release,
five per cache release, and five per queue submission. The stage arrays have fixed
size and do not allocate. Fence enqueueing adds another five clock reads, covering
cache work before fence creation. Device overhead and any change in frame times still need
measurement; this is an attribution build, not a demonstrated performance fix.

For the next same-scene comparison, first set **GPU Clocks: Normal** in the game's
settings. The logged Boost setting changes the emulated GPU timestamp rate; it does
not set the phone's physical GPU frequency. Then compare **0.75x resolution** with
1x, changing one setting at a time. Keep Handheld, Vulkan, NCE, 100% speed, caches,
Accurate GPU mode, and default fence/DMA behavior for the baseline. Fast GPU mode or
disabled reactive flushing can be tested separately, reverting if rendering or
stability changes. The prior log does not justify enabling asynchronous shaders or
changing the Switch firmware to fix these particular stalls.

