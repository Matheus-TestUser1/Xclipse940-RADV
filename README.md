# Xclipse940 RADV

Experimental Mesa RADV / ACO support for Samsung Xclipse940 graphics hardware.

[Source](https://github.com/Matheus-TestUser1/Xclipse940-RADV) · [Report an issue](https://github.com/Matheus-TestUser1/Xclipse940-RADV/issues) · [Mesa](https://www.mesa3d.org/)

This project adapts RADV's Vulkan userspace driver and the ACO shader compiler to the **MGFX2 hardware used by Xclipse940**, with Samsung's SGPU kernel driver. Development and hardware testing currently target the **Galaxy S24 FE, SM-S721B, Exynos2400e**.

**Status: active development.** Compiled compute shaders have passed SSBO read/write and arithmetic readback tests. The minimal graphics path can complete draws, but two-vertex LINE_LIST submissions still exhibit intermittent GPU hangs. General rendering, presentation and application compatibility are not yet validated.

## Scope

The work covers device identification, GPU memory and submission integration, shader instruction encoding, NGG shader resources and calling conventions, and MGFX2 register emission.

The port currently uses a GFX10.3 compatibility path with targeted Xclipse940 changes. Generic AMD register layouts and instruction encodings cannot be assumed to match this GPU. The Samsung Vulkan/PAL driver is used as a hardware reference through command-stream captures and shader analysis.

## Hardware and test environment

| Component | Tested configuration |
| --- | --- |
| Device | Samsung Galaxy S24 FE, SM-S721B |
| SoC / GPU | Exynos2400e / Xclipse940, MGFX2 |
| Architecture | Android ARM64 / AArch64 |
| Kernel driver | Samsung SGPU, DRM render node |
| Latest collected firmware | Android 14, `S721BXXS3AYB8` |
| Latest collected kernel | `6.1.75-android14-11` |
| Android toolchain | Android NDK r29; API 34 compiler used for probes |
| Build system | Meson / Ninja |
| Test deployment | ADB, `/data/local/tmp/xclipse940` |

These are the collected test configurations, not a compatibility guarantee for other firmware, devices or Xclipse models.

## Functional status

The table records observed probe results. Historical compute passes have not yet been rerun against every subsequent graphics experiment.

| Feature | Status | Evidence / limitations |
| --- | --- | --- |
| Vulkan HAL, instance and device creation | Verified in probes | Driver loads and discovers the target GPU. |
| Queue setup, command buffers and submission | Verified in probes | Working compute and control submissions complete. |
| Host-visible buffer allocation and mapping | Verified in probes | Supports the functional readback tests below. |
| Compute dispatch without memory access | Verified | Simple dispatch completes. |
| SSBO load → dependent store | Verified | Readback `0x1a2b3c4d`, fence success and exit code 0. |
| SSBO load → arithmetic → dependent store | Verified | Input `0x1a2b3c4d`; readback `0x1a2b3c4e`. |
| Explicit Vulkan pipeline cache | Verified in a probe | Correct results on cache miss and subsequent application cache hit. |
| CP-DMA transfer | Verified in probes | Includes a 4096-byte test. Shader-based meta transfers are separate. |
| Timeline semaphore ordering | Verified in a limited probe | Zero-command-buffer wait/signal test completed; final counter 1. |
| Minimal graphics pipeline creation | Verified | Vertex-only pipeline, rasterizer discard, empty render pass and framebuffer. |
| Zero-vertex draw control | Repeated passes | Draw packet and userdata remain present; count is forced to zero. |
| One-vertex LINE_LIST draw | Observed pass | Fence completes, but one vertex does not form a complete line. |
| Two-vertex LINE_LIST draw | Intermittent | Successful runs and GPU hangs occur with identical shader / PM4 captures. |
| Three-vertex graphics test | Failed in an earlier probe | Submission succeeded but fence timed out; no current-profile pass established. |
| Buffer device address | API path observed; memory operation unverified | Earlier global-store readback failed. Functional SSBO results do not validate BDA stores. |
| Attachments, textures, fragment output and pixel correctness | Not validated | The current graphics reproducer discards rasterization. |
| Swapchain / presentation, games and Vulkan conformance | Not validated | No support claim at this stage. |

## Current development baseline

Status snapshot: **2026-10-02**, source baseline [`9656f49`](https://github.com/Matheus-TestUser1/Xclipse940-RADV/commit/9656f49).

- **V22:** corrected selected WAIT, SOPP control, S_MOV_B32 and S_BFM_B64 encodings. Three complete graphics runs passed, followed by a later hang using the same captured ISA and PM4.
- **V23:** injected an exact Samsung GS shader for a controlled comparison. A successful run was followed by a run whose RADV fence completed but whose observation period saw a global GPU reset.
- **V25:** added named MGFX2 register mappings, corrected null-MRT register stepping and a reserved bit, and introduced an exact-stock-shader resource profile. The library built and was deployed.
- **V25.1:** corrects two expectations in the V25 verification runner: preamble alignment padding and the second centroid register address. It does not change the driver. Hardware execution results are pending.

The V25 capture stopped during a record-only preflight, before any GPU submission. Its verifier failure is not evidence of a new GPU hang. Correcting the verifier also does not establish graphics stability.

Versioned diagnostic runners and probes may be supplied separately from the driver source. Generated captures, bugreports, build outputs and source backups are not required source dependencies.

## Graphics blocker

The active reproducer uses an **empty vertex shader, LINE_LIST topology, two vertices, rasterizer discard enabled, no attachments and an empty 1×1 framebuffer**. It tests execution of the first complete line primitive without relying on framebuffer rendering.

Two failure patterns have been observed:

1. **The RADV job itself hangs.** A V22 bugreport attributes the timeout to `radv_graphics_l`, with `GRBM_STATUS=0xa840302c` and SPI_BUSY set. The fence failed to complete and a later KMD query returned `-125`.
2. **A reset occurs after the RADV fence completes.** In V23 round 2, the RADV fence completed and the raw fence query reported `query=0, expired=1`. The later timeout was attributed to WhatsApp's RenderThread, with `GRBM_STATUS=0xa800302c`. Whether this was independent or involved shared GPU state remains unresolved.

Global `gpu_info` counters alone do not identify the offending job. A fence pass or several consecutive successful runs are insufficient to establish stability. The lifecycle probe keeps resources alive during observation and also checks the period around teardown.

Investigation currently focuses on:

- Remaining MGFX2 register offsets, bitfields and register sequences that still assume AMD layout.
- NGG shader input conventions, userdata and resource allocation.
- Context initialization and restoration, including differences between the Samsung LOAD-register tables and RADV's explicit preamble.
- Consistency between the diagnostic CLEAR_STATE replacement and the defaults RADV assumes the hardware restores.
- Fence error reporting and attribution of resets across GPU contexts.

The CLEAR_STATE initialization concern is a hypothesis, not a confirmed root cause. Replacing a packet with NOP must be evaluated together with the effective `has_clear_state` configuration and the initialization paths it controls.

## Source areas

| Area | Files / directory |
| --- | --- |
| ACO instruction encoding | [`src/amd/compiler/aco_assembler.cpp`](src/amd/compiler/aco_assembler.cpp) |
| NIR / NGG lowering | [`src/amd/common/ac_nir_lower_ngg.c`](src/amd/common/ac_nir_lower_ngg.c) |
| Named MGFX2 register mappings | [`src/amd/common/ac_x940_reg_v25.h`](src/amd/common/ac_x940_reg_v25.h) |
| Graphics preamble | [`src/amd/common/ac_cmdbuf.c`](src/amd/common/ac_cmdbuf.c), [`src/amd/vulkan/radv_queue.c`](src/amd/vulkan/radv_queue.c) |
| Draw and graphics-state emission | [`src/amd/vulkan/radv_cmd_buffer.c`](src/amd/vulkan/radv_cmd_buffer.c) |
| Shader resources and diagnostic profiles | [`src/amd/vulkan/radv_shader.c`](src/amd/vulkan/radv_shader.c) |
| Compute and CP-DMA | [`src/amd/vulkan/radv_pipeline_compute.c`](src/amd/vulkan/radv_pipeline_compute.c), [`src/amd/vulkan/radv_cp_dma.c`](src/amd/vulkan/radv_cp_dma.c) |
| Device and memory integration | [`src/amd/vulkan/radv_physical_device.c`](src/amd/vulkan/radv_physical_device.c), [`src/amd/vulkan/winsys/amdgpu/`](src/amd/vulkan/winsys/amdgpu/) |

Hardware references include Samsung's `gc_10_4_0_offset_m2.h`, `gc_10_4_0_sh_mask_m2.h` and `vangogh_lite_ip_offset.h`. Register sequences need structural review: changing the first address does not fix a packet whose subsequent registers are no longer consecutive.

## Building

The commands below document the **existing, configured Android cross-build** used for hardware testing. A complete clean-checkout setup, including the Meson cross file, Android dependency sysroot and exact setup options, still needs to be published and verified.

Requirements include a Linux development environment, Git, Python 3, Meson, Ninja, an Android ARM64 NDK toolchain, matching Android dependencies such as libdrm, and ADB for device testing.

Clone the source:

```bash
git clone https://github.com/Matheus-TestUser1/Xclipse940-RADV.git
cd Xclipse940-RADV
```

After configuring the Android build directory and installation prefix, rebuild and install:

```bash
ninja -C build-xclipse940-android &&
ninja -C build-xclipse940-android install
```

The development configuration installs the driver at:

```text
out-xclipse940-android/lib/libvulkan_radeon.so
```

Deploy the library to the isolated test directory:

```bash
adb shell 'mkdir -p /data/local/tmp/xclipse940' &&
adb push out-xclipse940-android/lib/libvulkan_radeon.so \
  /data/local/tmp/xclipse940/libvulkan_radeon.so
```

This deployment is for probes that explicitly load the library. It does not install or select a system-wide Android Vulkan driver.

## Testing and diagnostics

Use the matching probe and its documented environment for each comparison. Versioned experiments use different compiler and graphics-state overrides; results must retain the exact flags and library identity.

The successful compute SSBO probes used:

```bash
export RADV_X940_ACO_NULL_M0=1
export RADV_X940_ACO_GFX11_WAIT=1
export RADV_X940_ACO_GFX11_MUBUF=1
```

These settings describe the tested compute profile. The current graphics profile has different overrides, including a diagnostic stock-shader path; the compute flags should not be combined with it without a controlled test.

For shader comparisons, development runners disable the Mesa shader cache and configure the test library search path:

```bash
export LD_LIBRARY_PATH=/data/local/tmp/xclipse940
export XDG_CACHE_HOME=/data/local/tmp/xclipse940/cache
export MESA_SHADER_CACHE_DISABLE=1
```

Use before/after external counters where available:

```bash
adb shell 'cat /sys/kernel/gpu/gpu_info'
```

The current device does not permit shell access to dmesg. A bugreport can provide kernel diagnostics:

```bash
adb bugreport x940_bugreport.zip
```

The matching lifecycle runner verifies shader bytes and PM4, waits for the initial fence, checks the raw KMD fence, observes four lifecycle phases, and compares external counters. Stop a repeated test at the first failure and preserve that run's evidence.

## Contributing

Targeted contributions are welcome, particularly in MGFX2 register emission, ACO encoding, NGG ABI, context handling and SGPU submission diagnostics.

An actionable issue or test report should include:

- Source commit, device model, firmware and kernel version.
- Build configuration, deployed library SHA-256, probe version and all environment overrides.
- The smallest reproducer: topology, vertex count, shaders, attachments and discard state.
- Exit code, Vulkan return values, raw fence results and counters before/after the test.
- Loaded shader ISA, preamble and main IB captures when relevant.
- Kernel timeout/reset attribution from the matching bugreport, distinguishing current events from historical LAST_KMSG entries.

Keep code changes scoped to the affected hardware paths. Record-only comparisons establish generated-code or command-stream differences; they do not establish GPU execution success. Diagnostic stock-shader injection is a controlled experiment, not a general shader compiler solution.

Review staged paths before committing. Build directories, probe executables, captures, bugreports, Python caches and source backups should stay outside source commits.

## Roadmap

- Complete the V25.1 hardware comparison with and without the exact stock resource profile.
- Resolve intermittent graphics hangs using job-attributed diagnostics and controlled context-state comparisons.
- Finish MGFX2 register sequences and field mappings in active graphics paths.
- Retest the functional compute baseline after graphics changes; independently validate BDA and shader-based meta transfers.
- Establish attachment, fragment-shader and pixel-readback tests before claiming rendering support.
- Publish a reproducible Android cross-build configuration and an organized probe suite.
- Expand to presentation and application testing once the minimal graphics path is stable.

## Credits and licensing

Port development and hardware testing: [Matheus-TestUser1](https://github.com/Matheus-TestUser1).

This project builds on Mesa, RADV, ACO and the work of their upstream contributors. Samsung's SGPU sources and hardware definitions provide platform references.

Upstream copyright notices and per-file license headers remain applicable. Consult those headers and the repository's license files for the terms governing each component.
