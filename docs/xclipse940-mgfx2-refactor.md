# MGFX2 register emission audit

## Source and limits

The initial refactor used public `main` at
`75db695acee70d0c4a4fa8228c2a323919c2a548`. It is now reconciled with the
owner's source archive `x940-projeto-fontes-20261003-213344.tar.xz`
(SHA256 `4fb8cff4814709db37f9ad4243111fe74511c2991caf4e17ee4253726db05a1a`).
That archive added six newer AMD source/header files relative to the public
base. The V26 context-default register values, V27 CU-mask profile, V29 framebuffer
layout, V30 opt-in user-fence experiment, and V32 lifecycle monitor are retained.
Context defaults are now selected automatically for X940; the V16/V26 flags
no longer select its context initialization method.
The later device runners remain local diagnostic tools; generated captures,
probe binaries, archives and backup sources are not part of this refactor.

The hardware references are Samsung's `gc_10_4_0_offset_m2.h`,
`gc_10_4_0_sh_mask_m2.h` and `vangogh_lite_ip_offset.h`. Addresses are derived
as `(GC_BASE__INST0_SEG[BASE_IDX] + mmREGISTER) * 4`. Comparing a DWORD offset
directly with a PM4 byte address gives an invalid result.

The audit examines AMD C, C++ and header files. A textual mismatch is a review
candidate: a GFX6/GFX9/GFX12 branch may never execute on X940. Absence of a
name in the header is not, by itself, proof that a register cannot exist.
The port avoids the unverified legacy VS path and legacy GS ring-size writes.

## Confirmed issues and changes

| Writer / role | AMD layout used before | MGFX2 layout used now |
| --- | --- | --- |
| NGG program metadata, including separately compiled shaders | LO `0x00b320`, RSRC1/2 `0x00b228/0x00b22c` | ES LO/HI `0x00b218/0x00b21c`, GS RSRC1/2 `0x00b210/0x00b214` |
| Hull program metadata | LS LO `0x00b520`, HS RSRC1/2 `0x00b428/0x00b42c` | LS LO/HI `0x00b418/0x00b41c`, HS RSRC1/2 `0x00b410/0x00b414` |
| Tessellation factor address high | `0x030984`, aliases MGFX2 `GE_USER_VGPR_EN` | `0x030988` |
| Viewport transforms and depth ranges | Six transform values from `0x02843c`; separate Z array at `0x0282d0` | Eight values per viewport from `0x028444`, stride `0x20`; ZMIN/ZMAX at `0x02845c/0x028460` |
| Pixel shader input array | `0x028644` | `0x028664`, including its consecutive input range |
| Pixel shader input enable/address | `0x0286cc/0x0286d0` | `0x02865c/0x028660` |
| Pixel shader input control | `0x0286d8` | `0x028710` |
| Shader Z export format, including epilogs | `0x028710`, aliases MGFX2 input control | `0x028650` |
| Pixel shader control | `0x028c40`, aliases MGFX2 raster mode control | `0x028c58` |
| Clip and raster mode control | Correct mapping depended on a diagnostic environment variable | Always map X940 to `0x028814/0x028810` |
| Guardband packet | Four values from `0x028be8` | Four values from `0x028434` |
| Color buffer control | `0x028808` | `0x028de0`; retained MODE/ROP3/DISABLE_DUAL_QUAD fields verified |
| Framebuffer, retained from V29 | AMD CB/DB ranges and field assumptions | Nine CB registers per MRT with `0x24` stride; separate INFO/EXT/clear arrays; 17 DB writes with separate high addresses |
| Render target write mask | `0x028238` | `0x028de8` |
| Conservative rasterization control | `0x028c4c` | `0x028c54` |
| Tessellation parameter control | `0x028b6c` | `0x028aa8` |
| Alpha to coverage control | `0x028b70`, with AMD dithering fields | `0x028070`; retain only MGFX2's defined enable bit |
| Stencil operation control | `0x02842c` | `0x028804` |
| Front/back stencil reference and masks | `0x028430/0x028434` | `0x0283e8/0x0283ec` |

Additional structural changes:

- Introduce `ac_mgfx2_regs.h`: an unversioned, named layout shared by writers
  and shader metadata. Keep `ac_x940_reg_v25.h` as a forwarding compatibility
  header for local bring-up packages. The V29 framebuffer compatibility header
  likewise forwards to the shared addresses and field helpers.
- Skip legacy VS initialization on X940, retaining PS/HS setup. Keep NGG
  enabled on X940 so `RADV_DEBUG=nongg` cannot select the unverified VS block.
- Initialize ES program address high in the preamble. Separately compiled
  shaders and prologs write LO through metadata and depend on that setup.
- Select the descriptor pointer table once by hardware. X940 uses PS, GS and
  HS; omit legacy VS. Those three addresses already matched Samsung, so their
  alleged offset discrepancy was not confirmed.
- Reject legacy ESGS/GSVS ring-size requests before submission. The generic
  two-register packet at `0x030900` also writes `0x030904`, MGFX2's
  `VGT_GS_OUT_PRIM_TYPE`. This is a feature restriction until the ring protocol
  is verified, not an implementation of legacy GS support.
- Correct GDS/OA cleanup to unresident the newly allocated BO, rather than
  the old queue BO. Track successful residency so a failed residency call
  does not trigger an unmatched removal.
- Return and propagate preamble allocation failures. Discard the failed CS
  instead of publishing a preamble with missing initialization.
- Stop advertising fragment shader interlock on X940: the required
  `PA_SC_SHADER_CONTROL.LOAD_COLLISION_WAVEID` bit is reserved in MGFX2's
  header. Its protocol needs separate implementation and validation.

## Context initialization

The incremental context change is based on merged `main` at
`a0c47a8f59d8f803cd4299206c4e45cc12f18442`. Previously, X940 inherited
`has_clear_state = true` from its GFX10.3 compatibility level. The tested
V26 profile then replaced CLEAR_STATE with a NOP and explicitly initialized
the defaults through an environment variable. Without that variable the
same chip took a different initialization path.

Samsung's public SGPU source skips the generic clear-state block for MGFX
in `gfx_v10_0_cp_async_gfx_start`; its MGFX RLC initialization also skips
the generic AMD clear-state buffer setup. Reference:
[gfx_v10_0.c at 6946920](https://github.com/Elchanz3/android_kernel_samsung_exynos2400/blob/6946920b36b6d6c99acad76aec6139004e1137c7/kernel/drivers/gpu/drm/samsung/gpu/sgpu/gfx_v10_0.c).
This supports avoiding an assumed AMD context baseline. It does not establish
that the opcode is physically absent, or that this custom kernel's runtime
configuration matches the installed Samsung kernel.

The capability now excludes X940, and
`radv_emit_graphics_context_defaults()` emits the existing 34 default writes:
ZMIN/ZMAX for 16 viewports at MGFX2's addresses and stride, the retained RADV
edge rule, and zero hardware screen offset. Other AMD chips retain their
previous capability and fallback behavior. The helper does not reconstruct
a full PAL register shadow table or zero undocumented registers.

`RADV_X940_NOP_CLEAR_STATE_V16` and `RADV_X940_DIAG_CONTEXT_DEFAULTS_V26`
have no effect on this path; existing runners may still export them.
The new `x940-context-init` marker reports the selected method. Two NOP
packets used by the old diagnostic profile are removed. Normal final IB
alignment remains the winsys's responsibility, so on-device captures must
be regenerated rather than requiring their old lengths or hashes.

This promotes a previously tested initialization profile. That profile also
exhibited an intermittent hang, so this change alone is not evidence that
the hang is resolved. Full context coverage and restoration remain open.

## Validation performed

`bin/x940_regmap_audit.py` verifies all 85 named addresses in the new layout
against the kernel headers and records other source references for review.
On this snapshot it scans 465 files and derives 3,752 named GC registers.
Its candidate counts do **not** count confirmed runtime bugs.

`bin/x940_validate_emitters.py` compiles actual common preamble/PM4 source
and extracts selected RADV functions unchanged into a host harness. Small
dependency mocks capture their writes. Checks cover:

- Ten common AMD preamble cases, GFX10 through GFX12 with cache policy on/off:
  byte-identical to the baseline.
- MGFX2 preamble: no legacy VS writes; PS/HS retained; correct ES high address.
- Descriptor pointers and merged/separately compiled NGG and hull metadata.
- Fragment writes, viewport counts 1–16, guardband, conservative rasterization
  and alpha to coverage; 128 color-control combinations; non-X940 output
  matches the baseline.
- Sixty retained field macros, testing each input bit against Samsung
  masks and shifts. GFX11-only `PRIM_ATTR` is excluded from MGFX2 validation.
- Compute and graphics preamble allocation failures return errors. The graphics
  caller discards any partial control-only CS.
- Eighteen capability combinations check the actual assignment across
  GFX6–GFX12, with and without X940 identification.
- The actual queue graphics emitter initializes all 16 depth ranges on X940
  with every combination of the retired V16/V26 flags, without CLEAR_STATE.
  Register values and executable control packets match the baseline V26
  profile after removing exactly four NOP DWORDs. Non-X940 GFX10–GFX12 output
  matches the baseline, with and without CLEAR_STATE.
- The actual preamble failure cleanup block: only newly resident GDS/OA BOs
  are unresidented; old queue BOs are preserved.

`bin/x940_validate_framebuffer.py` compiles the actual framebuffer branches
and fast-clear functions against independently supplied Samsung headers:

- 2,048 CB packets: all 256 moved-field combinations across eight MRTs.
- Address/value order, separate INFO and EXT arrays, packet lengths, ATTRIB3
  reserved bits, and retained VIEW/ATTRIB2/DB field masks.
- Seventeen depth/stencil writes in 41 dwords, including all high-address
  registers; DEPTH_SIZE_XY is not overwritten by AMD DEPTH_INFO.
- Fast-clear write, LOAD and COPY paths with guards, including AMD regression.

This is host validation of emission and cleanup. The complete Android driver
has **not** been built in this environment, and no GPU submission was made.
It does not demonstrate that the intermittent hang is fixed.

Example, with the Samsung headers available locally:

```sh
python3 -B bin/x940_regmap_audit.py \
  --offset-header "$OFFSET_HEADER" --ip-header "$IP_HEADER" \
  --output "$HOME/xclipse-diag/mgfx2-audit.json"

python3 -B bin/x940_validate_emitters.py \
  --offset-header "$OFFSET_HEADER" --ip-header "$IP_HEADER" \
  --mask-header "$MASK_HEADER"

python3 -B bin/x940_validate_framebuffer.py \
  --offset-header "$OFFSET_HEADER" --ip-header "$IP_HEADER" \
  --mask-header "$MASK_HEADER"
```

## Remaining work before GPU tests

1. Apply the incremental patch prepared for the supplied archive, or inspect
   this PR branch. Do not apply the original public-base patch to newer local
   files. The old `test_v29_framebuffer.py` uses exact versioned source-string
   matches; use the new framebuffer validator after refactoring.
2. Build the full Android driver. Record the library hash used on the device.
3. Record command buffers without submission for the existing zero-draw and
   0/1/2/3-vertex cases. Decode the full preamble and main IB; review the
   intentional changes rather than requiring equality to an obsolete capture.
   Run V29.1 again for the rebuilt library before passing its new output
   directory to V37/V36; their old library-hash baseline cannot be reused.
4. Run controlled execution with external GPU counters and the existing
   lifecycle log. Check completion, delayed resets and teardown separately.
   Stop the series on a counter increase or failed fence and capture bugreport.
5. Audit remaining enabled features by their actual MGFX2 path: actual rendering/fast clear, queries/streamout, tessellation, scratch, shader resources,
   context restoration and queue synchronization. A matching address alone
   does not validate fields, range strides, shader ABI or packet semantics.

`SPI_BUSY` and an incomplete primitive threshold help narrow the investigation.
Remaining source candidates also include SPM/SQTT profiling paths and
registers belonging to other AMD generations. Those paths need explicit guard
and packet reviews before enabling them on MGFX2. This refactor does not claim
to have completed the entire hardware port.

They do not prove that a one-vertex draw never launches a shader, that userdata
is the cause, or that the process named in a later reset caused the original
fault. Early fence signaling and leaked global state remain hypotheses until
the KMD protocol and captured event order establish them. No change here
forces all CU mask bits, kills Android graphics services or adds a blind
register-status polling wait.
