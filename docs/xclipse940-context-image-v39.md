# MGFX2 context image diagnostic (V39)

The current graphics blocker is intermittent failure of a two-vertex
`LINE_LIST` draw with rasterizer discard. The previous RADV failure was
reported against the probe itself, before resource destruction. A V38 stock
control completed the same draw, its fence, four lifecycle phases, and six
seconds of post-exit counter checks without a counter increase. That single
control does not establish long-term stability or prove the hang's cause.

V39 tests one difference: the initialized per-context image that Samsung loads
before drawing. It is **opt-in**, with
`RADV_X940_DIAG_CONTEXT_IMAGE_V39=1`; normal AMD and X940 operation retain their
previous packets when the option is unset or zero.

## Evidence and scope

The V38 raw stream contains a `LOAD_CONTEXT_REG` with 15 ranges covering 614
DWORD positions. Two full shadow-BO snapshots and their extracted windows
were checked against each other. The initialized context snapshot has 53
nonzero DWORDs; the rest are zero. These are CPU-visible snapshots immediately
before the CS ioctl, not a readback of hardware registers.

The profile is recorded in `bin/x940_context_v39_reference.json`. Its source
context-window SHA256 is
`6029e1cb61e43a9e61ddf14eebf9234114fc5cbcb680dfd893ae2c0dfd8b7847`.
The stock HAL SHA256 is
`e3eacfdf52904bbb81d1dbbbd343bc352ee6d1c62d8eae71618615908d25cd3a`.
The tested device uses kernel `6.1.75-android14-11`, build `S721BXXS3AYB8`,
and boot parameter `sgpu.mcbp=0`.

Names and addresses are checked against Samsung `gc_10_4_0_offset_m2.h`
and `vangogh_lite_ip_offset.h`, previously retrieved from Elchanz3's kernel
commit `6946920b36b6d6c99acad76aec6139004e1137c7`. A header tells us addresses
and fields; it does not establish a reset value. The defaults are a captured
software profile for this control, not a guarantee for every firmware/device.

No captured GPU virtual address is included in the image. It contains no SH
or UCONFIG state, so it does not reset global GL2 workarounds, harvesting
masks, shader resources, or descriptor pointers. All shader and framebuffer
resources continue to be programmed by RADV. Its existing graphics defaults
and per-draw dynamic state run after the LOAD and can override the image.

## Implementation

`ac_mgfx2_context.h` keeps the MGFX2 ranges and defaults separate from Mesa's
generic GFX10.3 shadow-register defaults. The table is an aperture image:
each DWORD is indexed by `(register - 0x28000) / 4`, including gaps. LOAD
uses only the observed ranges. A 4 KiB table covers the final range; bytes
outside it in the captured 32 KiB window were verified zero.

Device creation uploads the immutable image through the existing mapped-BO
path. The BO is CPU-accessible GTT, aligned to 4096, GPU read-only, and not
shared across processes. Allocation/map failures propagate to device creation;
cleanup releases only this BO. No internal GPU submission initializes it.
The BO remains owned by the device until queue cleanup finishes.

| Queue preamble | V39 behavior |
| --- | --- |
| `initial-full-flush` | Retain image BO; VS/PS partial flush and VGT flush; context LOAD; original graphics init |
| `initial-normal` | Same context initialization before the original graphics init |
| `continue` | Original behavior; no image LOAD |

Loading a full image in `continue` could discard state needed by a continuation
of the same command buffer. The image is therefore loaded only at the initial
submission boundary. Extra command-buffer space is reserved for its 42 DWORDs.

The diagnostic emits `CONTEXT_CONTROL` with load word `0x80000002` and
shadow word `0x80000000`: only per-context LOAD is enabled, and shadow writes
are disabled. The subsequent original graphics init retains its existing
control words. This experiment initializes defaults; it **does not implement
PAL's complete context save/restore protocol**. The mode rejects devices
using the generic register-shadowing path rather than mixing its AMD ranges
with MGFX2. It does not enable preemption.

With `RADV_X940_DIAG_DUMP_GFX_PREAMBLE`, device creation prints the image's
actual CPU-mapped contents after copying, plus its VA and size. This makes
the LOAD address resolvable in the recorder. It is not GPU readback.

## Validation

Run the new host check from the project root:

```sh
python3 -B bin/x940_validate_context_image.py
```

It compiles the real uploader, cleanup, LOAD emitter and graphics-init helper
with dependency mocks. It checks all 1024 image DWORDs, exact PM4/ranges/VA,
the 614 covered positions, opt-in and AMD/shadow guards, allocation/map errors,
cleanup, and BO retention. Optionally compare the independent capture itself:

```sh
python3 -B bin/x940_validate_context_image.py \
  --stock-folder /path/to/extracted/v38/round_1
```

The existing emitter and framebuffer checks also passed locally: ten AMD
common-preamble cases remain byte-identical; MGFX2 named-address and retained
field checks, 2048 CB cases, DB and fast-clear checks passed. The register
audit checked 137 named addresses against the M2/IP headers with no errors.

Full Android compilation and device execution remain pending. The separate
`run_v39_context_record.py` delivered with this change uses the existing V36
probe/monitor and a previous approved V29.1 baseline. It records the two
initial preambles and `continue` under `RADV_X940_PREAMBLE_RECORD_ONLY`.
It creates no Vulkan fence and validates that no KMD submission is observed.
It checks the exact table contents and LOAD ranges/address/order, unchanged
graphics init, main IB and stock ISA, all three queue preambles, and counters.

After building and pushing the driver, first run only this recording:

```sh
python3 -B run_v39_context_record.py \
  --baseline "$HOME/xclipse-diag/v29_1_fb_record_clr4nx6d"
```

Keep that **pre-V39** baseline; a different V39 library is expected and is
checked against the installed device library. A real draw test is a later
step, after this recording is reviewed. No hang fix is claimed by this PR.
