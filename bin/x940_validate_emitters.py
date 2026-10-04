#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host checks of actual MGFX2 emitters; no GPU access or Android execution.

Compile the real common preamble and PM4 sources. Selected RADV functions
are extracted unchanged and compiled with small dependency mocks. The mocks
capture register writes; they do not implement the emitters being tested.
Compare non-X940 output with a Git baseline, and MGFX2 addresses with the
Samsung headers. This is not a replacement for building the complete driver.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from x940_regmap_audit import kernel_map, numeric_defines


def function(source, name):
    match = re.search(r"(?m)^(?:static )?(?:void|VkResult|uint32_t|unsigned)\s+" + re.escape(name) + r"\(", source)
    if not match:
        raise ValueError(f"Function not found: {name}")
    start = source.index("{", match.end())
    # Braces in comments/strings must not change the function boundary.
    from x940_regmap_audit import strip_comments_and_strings
    clean = strip_comments_and_strings(source)
    depth = 1
    end = start + 1
    while depth:
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    return source[match.start():end]


def enum(source, name):
    return re.search(r"enum " + name + r"\s*\{.*?\};", source, re.S)[0]


def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)


def generate_amd_header(root):
    meson = (root / "src/amd/common/meson.build").read_text()
    json_list = re.search(r"amd_json_files\s*=\s*\[(.*?)\n\]", meson, re.S)[1]
    json_files = re.findall(r"'([^']+\.json)'", json_list)
    return run([sys.executable, "-B", str(root / "src/amd/registers/makeregheader.py"),
         "--sort", "address", "--guard", "AMDGFXREGS_H",
         *(str(root / "src/amd/common" / name) for name in json_files)],
         env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1")).stdout


DEPENDENCIES = r'''
#include "ac_gpu_info.h"
#include "ac_hw_stage.h"
#include "ac_cmdbuf.h"
#include "ac_debug.h"
#include "ac_mgfx2_regs.h"
#include "ac_x940_reg_v25.h" /* Baseline functions still use compatibility names. */
#include "sid.h"
#include "util/u_math.h"
#include "compiler/shader_enums.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
const char *ac_get_register_name(enum amd_gfx_level g, enum radeon_family f, unsigned r) {
   (void)g; (void)f; (void)r; return "";
}
void ac_get_raster_config(const struct radeon_info *i, uint32_t *a, uint32_t *b, uint32_t *c) {
   (void)i; (void)a; (void)b; (void)c; abort();
}
void ac_get_harvested_configs(const struct radeon_info *i, unsigned a, unsigned *b, unsigned *c) {
   (void)i; (void)a; (void)b; (void)c; abort();
}
'''

COMMON_MAIN = r'''
int main(int argc, char **argv) {
   const enum amd_gfx_level levels[] = {GFX10, GFX10_3, GFX11, GFX11_5, GFX12};
   const bool x940 = argc > 1;
   (void)argv;
   for (unsigned j = 0; j < (x940 ? 1 : ARRAY_SIZE(levels)); ++j) {
      for (unsigned cache = 0; cache < 2; ++cache) {
         struct radeon_info info = {
            .gfx_level = x940 ? GFX10_3 : levels[j], .is_xclipse940 = x940,
            .family = CHIP_VANGOGH, .address32_hi = 0x8000,
            .spi_cu_en = 0x3f, .min_good_cu_per_sa = 6, .max_good_cu_per_sa = 6,
            .max_se = 2, .num_se = 2, .max_sa_per_se = 2, .num_rb = 2,
            .max_render_backends = 2,
         };
         struct ac_preamble_state pre = {
            .border_color_va = 0x800001234500ull, .gfx10.cache_rb_gl2 = cache,
            .gfx11.compute_dispatch_interleave = 64,
         };
         struct ac_pm4_state *pm4 = ac_pm4_create_sized(&info, false, 1024, false);
         assert(pm4);
         ac_init_graphics_preamble_state(&pre, pm4);
         ac_init_compute_preamble_state(&pre, pm4);
         ac_pm4_finalize(pm4);
         printf("CASE %u %u %u\n", j, cache, pm4->ndw);
         for (unsigned i = 0; i < pm4->ndw; ++i)
            printf("%08x\n", pm4->pm4[i]);
         ac_pm4_free_state(pm4);
      }
   }
}
'''

RADV_MODEL = r'''
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
typedef struct {float x, y, width, height, minDepth, maxDepth;} VkViewport;
enum radv_depth_clamp_mode {
   RADV_DEPTH_CLAMP_MODE_VIEWPORT, RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE,
   RADV_DEPTH_CLAMP_MODE_USER_DEFINED,
};
enum {RADV_TRACKED_SPI_PS_INPUT_ENA, RADV_TRACKED_SPI_PS_IN_CONTROL,
      RADV_TRACKED_SPI_SHADER_Z_FORMAT, RADV_TRACKED_PA_SC_SHADER_CONTROL};
struct radv_physical_device {struct radeon_info info;};
struct radv_instance {unsigned debug_flags;};
#define RADV_DEBUG_NO_ATOC_DITHERING 1
#define RADV_CMD_DIRTY_GUARDBAND 1
#define MAX_RTS 8
#define VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT 0
#define VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT 1
struct radeon_winsys_bo {uint64_t va;};
struct radv_shader_info {
   bool merged_shader_compiled_separately, is_ngg;
   gl_shader_stage stage, next_stage;
   enum ac_hw_stage requested_hw_stage;
   struct {
      uint32_t pgm_lo, pgm_rsrc1, pgm_rsrc2, pgm_rsrc3;
      struct {uint32_t spi_ps_in_control, spi_shader_z_format,
              pa_sc_hisz_control, pa_sc_shader_control;} ps;
   } regs;
   struct {bool reads_fully_covered;} ps;
};
struct radv_shader {
   uint64_t va;
   struct radv_shader_info info;
   struct {uint32_t rsrc1, rsrc2, spi_ps_input_ena, spi_ps_input_addr;} config;
};
struct radv_device {
   struct radv_physical_device *pdev;
   bool uses_shadow_regs;
   void *ws;
   struct {struct radeon_winsys_bo *bo;} border_color_data;
   struct radeon_winsys_bo *tma_bo;
   struct radv_shader *trap_handler_shader;
};
struct radeon_cmdbuf {
   uint32_t addresses[2048], values[2048], raw[4096]; unsigned count, next, remaining, raw_count;
};
struct test_cb_attachment {unsigned write_mask; bool dual_src;};
struct radv_dynamic_state {
   struct {struct {
      unsigned viewport_count;
      bool depth_clip_negative_one_to_one;
      VkViewport viewports[16];
      struct {float minDepthClamp, maxDepthClamp;} depth_clamp_range;
   } vp;
   struct {unsigned polygon_mode, conservative_mode; struct {float width;} line;} rs;
   struct {bool alpha_to_coverage_enable;} ms;
   struct {bool logic_op_enable; unsigned logic_op; struct test_cb_attachment attachments[8];} cb;
   } vk;
   struct {struct {float scale[3], translate[3];} xform[16];} hw_vp;
};
struct radv_cmd_buffer {
   struct radv_device *device; struct radeon_cmdbuf *cs;
   enum radv_depth_clamp_mode clamp;
   struct {struct radv_dynamic_state dynamic; struct radv_shader *shaders[16]; unsigned dirty, custom_blend_mode;} state;
};
static struct radv_physical_device *radv_device_physical(const struct radv_device *d) {return d->pdev;}
static struct radv_device *radv_cmd_buffer_device(const struct radv_cmd_buffer *c) {return c->device;}
static struct radv_instance instance;
static const struct radv_instance *radv_physical_device_instance(const struct radv_physical_device *p) {(void)p; return &instance;}
static bool radv_can_enable_dual_src(const struct test_cb_attachment *a) {return a->dual_src;}
static unsigned radv_get_rasterization_prim(struct radv_cmd_buffer *c) {(void)c; return 2;}
static bool radv_rast_prim_is_point(unsigned p) {return p == 1;}
static bool radv_rast_prim_is_line(unsigned p) {return p == 2;}
static bool radv_polygon_mode_is_point(unsigned p) {return p == 1;}
static bool radv_polygon_mode_is_line(unsigned p) {return p == 2;}
static uint64_t radv_buffer_get_va(struct radeon_winsys_bo *b) {return b->va;}
static uint64_t radv_shader_get_va(const struct radv_shader *s) {return s->va;}
static enum ac_hw_stage radv_select_hw_stage(const struct radv_shader_info *i, enum amd_gfx_level g) {
   (void)g; return i->requested_hw_stage;
}
static enum radv_depth_clamp_mode radv_get_depth_clamp_mode(const struct radv_cmd_buffer *c) {return c->clamp;}
static void radv_cs_add_buffer(void *w, struct radeon_cmdbuf *c, struct radeon_winsys_bo *b) {(void)w;(void)c;(void)b;}
static void write_reg(struct radeon_cmdbuf *c, unsigned a, uint32_t v) {
   assert(c->count < ARRAY_SIZE(c->values));
   c->addresses[c->count] = a; c->values[c->count++] = v;
}
static void radeon_set_context_reg_seq(struct radeon_cmdbuf *c, unsigned a, unsigned n) {
   assert(!c->remaining); c->next = a; c->remaining = n;
}
#define radeon_set_sh_reg_seq radeon_set_context_reg_seq
#define radeon_set_context_reg write_reg
#define radeon_set_sh_reg write_reg
static void radeon_set_sh_reg_idx(const struct radeon_info *i, struct radeon_cmdbuf *c,
                                 unsigned a, unsigned index, uint32_t v) {
   (void)i; (void)index; write_reg(c, a, v);
}
static void radeon_emit(struct radeon_cmdbuf *c, uint32_t v) {
   if (c->remaining) {
      write_reg(c, c->next, v); c->next += 4; c->remaining--;
   } else {
      assert(c->raw_count < ARRAY_SIZE(c->raw)); c->raw[c->raw_count++] = v;
   }
}
static void radv_emit_shader_pointer(struct radv_device *d, struct radeon_cmdbuf *c, unsigned a, uint64_t v, bool global) {
   (void)d; assert(global); write_reg(c, a, v); write_reg(c, a + 4, v >> 32);
}
static void radeon_opt_set_context_reg(struct radv_cmd_buffer *c, unsigned a, unsigned track, uint32_t v) {
   (void)track; write_reg(c->cs, a, v);
}
static void radeon_opt_set_context_reg2(struct radv_cmd_buffer *c, unsigned a, unsigned track, uint32_t x, uint32_t y) {
   radeon_opt_set_context_reg(c, a, track, x); write_reg(c->cs, a + 4, y);
}
static void radeon_emit_array(struct radeon_cmdbuf *c, const uint32_t *dw, unsigned n) {
   assert(c->raw_count + n <= ARRAY_SIZE(c->raw));
   memcpy(c->raw + c->raw_count, dw, n * sizeof(*dw)); c->raw_count += n;
   for (unsigned pos = 0; pos < n;) {
      unsigned h = dw[pos], size = ((h >> 16) & 0x3fff) + 2, op = (h >> 8) & 255;
      assert(h >> 30 == 3 && pos + size <= n);
      unsigned base = op == 0x68 ? 0x8000 : op == 0x69 ? 0x28000 :
                      (op == 0x76 || op == 0x9b) ? 0xb000 :
                      (op == 0x79 || op == 0x7a) ? 0x30000 : 0;
      if (base)
         for (unsigned j = 2; j < size; j++)
            write_reg(c, base + ((dw[pos + 1] & 0xffff) + j - 2) * 4, dw[pos + j]);
      pos += size;
   }
}
static struct ac_pm4_state *fault_pm4_create(const struct radeon_info *i, bool s, unsigned n, bool c) {
   (void)i;(void)s;(void)n;(void)c; return NULL;
}
struct cleanup_winsys {
   void (*cs_destroy)(struct radeon_cmdbuf *);
   VkResult (*buffer_make_resident)(struct cleanup_winsys *, struct radeon_winsys_bo *, bool);
};
struct cleanup_queue {
   struct radeon_winsys_bo *descriptor_bo, *scratch_bo, *compute_scratch_bo,
      *esgs_ring_bo, *gsvs_ring_bo, *tess_rings_bo, *task_rings_bo,
      *attr_ring_bo, *gds_bo, *gds_oa_bo;
};
static struct radeon_winsys_bo *released[4], *destroyed[4];
static unsigned release_count, destroy_count;
static VkResult cleanup_resident(struct cleanup_winsys *w, struct radeon_winsys_bo *b, bool resident) {
   (void)w; assert(b && !resident); released[release_count++] = b; return VK_SUCCESS;
}
static void radv_bo_destroy(struct radv_device *d, void *alloc, struct radeon_winsys_bo *b) {
   (void)d;(void)alloc; assert(b); destroyed[destroy_count++] = b;
}
#define vk_error(queue, result) (result)
'''

RADV_MAIN = r'''
int main(void) {
   check_fields();
   struct radv_physical_device pdev = {.info = {.gfx_level = GFX10_3, .is_xclipse940 = true}};
   struct radv_device device = {.pdev = &pdev};
   struct radeon_cmdbuf cs = {0}, baseline_cs = {0};
   struct radv_cmd_buffer cmd = {.device = &device, .cs = &cs};
   struct radv_cmd_buffer baseline_cmd = {.device = &device, .cs = &baseline_cs};
   struct radeon_winsys_bo bo = {.va = 0x800011223300ull};
   radv_emit_graphics_shader_pointers(&device, &cs, NULL);
   assert(!cs.count);
   radv_emit_graphics_shader_pointers(&device, &cs, &bo);
   assert(cs.count == 6);
   assert(cs.addresses[0] == K_SPI_SHADER_USER_DATA_PS_0);
   assert(cs.addresses[2] == K_SPI_SHADER_USER_DATA_ADDR_LO_GS);
   assert(cs.addresses[4] == K_SPI_SHADER_USER_DATA_ADDR_LO_HS);
   struct radv_shader ps = {.va = 0x800012345600ull,
      .config = {.rsrc1 = 0x11, .rsrc2 = 0x22, .spi_ps_input_ena = 0x33, .spi_ps_input_addr = 0x44},
      .info.regs.ps = {.spi_ps_in_control = 0x55, .spi_shader_z_format = 0x66, .pa_sc_shader_control = 0}};
   cmd.state.shaders[MESA_SHADER_FRAGMENT] = &ps;
   ps.info.requested_hw_stage = AC_HW_PIXEL_SHADER;
   radv_precompute_registers_pgm(&device, &ps.info);
   cs = (struct radeon_cmdbuf){0};
   radv_emit_fragment_shader(&cmd);
   const unsigned ps_regs[] = {K_SPI_SHADER_PGM_LO_PS, K_SPI_SHADER_PGM_HI_PS,
      K_SPI_SHADER_PGM_RSRC1_PS, K_SPI_SHADER_PGM_RSRC2_PS,
      K_SPI_PS_INPUT_ENA, K_SPI_PS_INPUT_ADDR, K_SPI_PS_IN_CONTROL,
      K_SPI_SHADER_Z_FORMAT, K_PA_SC_SHADER_CONTROL};
   assert(cs.count == ARRAY_SIZE(ps_regs));
   for (unsigned i = 0; i < cs.count; ++i) assert(cs.addresses[i] == ps_regs[i]);
   for (unsigned hull = 0; hull < 2; ++hull) {
      for (unsigned separate = 0; separate < 2; ++separate) {
         struct radv_shader_info info = {.requested_hw_stage = hull ? AC_HW_HULL_SHADER : AC_HW_NEXT_GEN_GEOMETRY_SHADER,
            .stage = MESA_SHADER_VERTEX, .next_stage = hull ? MESA_SHADER_TESS_CTRL : MESA_SHADER_GEOMETRY,
            .merged_shader_compiled_separately = separate, .is_ngg = true};
         radv_precompute_registers_pgm(&device, &info);
         assert(info.regs.pgm_lo == (hull ? K_SPI_SHADER_PGM_LO_LS : K_SPI_SHADER_PGM_LO_ES));
         assert(info.regs.pgm_rsrc1 == (hull ? K_SPI_SHADER_PGM_RSRC1_HS : K_SPI_SHADER_PGM_RSRC1_GS));
         assert(info.regs.pgm_rsrc2 == (hull ? K_SPI_SHADER_PGM_RSRC2_HS : K_SPI_SHADER_PGM_RSRC2_GS));
      }
   }
   for (unsigned n = 1; n <= 16; ++n) {
      cmd.state.dynamic.vk.vp.viewport_count = n;
      for (unsigned i = 0; i < n; ++i) {
         cmd.state.dynamic.vk.vp.viewports[i].minDepth = 0.25f;
         cmd.state.dynamic.vk.vp.viewports[i].maxDepth = 0.75f;
         for (unsigned k = 0; k < 3; ++k) {
            cmd.state.dynamic.hw_vp.xform[i].scale[k] = 100.0f * i + k + 1;
            cmd.state.dynamic.hw_vp.xform[i].translate[k] = 100.0f * i + k + 10;
         }
      }
      cs = (struct radeon_cmdbuf){0};
      radv_emit_viewport(&cmd);
      assert(!cs.remaining && cs.count == n * 8);
      for (unsigned i = 0; i < n; ++i) {
         const float *s = cmd.state.dynamic.hw_vp.xform[i].scale;
         const float *t = cmd.state.dynamic.hw_vp.xform[i].translate;
         const uint32_t values[] = {fui(s[0]), fui(t[0]), fui(s[1]), fui(t[1]), fui(s[2]), fui(t[2]), fui(0.25f), fui(0.75f)};
         for (unsigned k = 0; k < 8; ++k) {
            assert(cs.addresses[i*8+k] == K_PA_CL_VPORT_XSCALE + i*0x20+k*4);
            assert(cs.values[i*8+k] == values[k]);
         }
      }
   }
   cs = (struct radeon_cmdbuf){0};
   radv_emit_guardband_state(&cmd);
   const unsigned guardband_regs[] = {K_PA_CL_GB_VERT_CLIP_ADJ, K_PA_CL_GB_VERT_DISC_ADJ,
                                     K_PA_CL_GB_HORZ_CLIP_ADJ, K_PA_CL_GB_HORZ_DISC_ADJ};
   assert(cs.count == 4);
   for (unsigned i = 0; i < 4; ++i) assert(cs.addresses[i] == guardband_regs[i]);
   for (unsigned mode = 0; mode < 3; ++mode) {
      cmd.state.dynamic.vk.rs.conservative_mode = mode;
      cs = (struct radeon_cmdbuf){0};
      radv_emit_conservative_rast_mode(&cmd);
      assert(cs.count == 1 && cs.addresses[0] == K_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL);
   }
   for (unsigned dither = 0; dither < 2; ++dither) {
      instance.debug_flags = dither ? RADV_DEBUG_NO_ATOC_DITHERING : 0;
      for (unsigned enable = 0; enable < 2; ++enable) {
         cmd.state.dynamic.vk.ms.alpha_to_coverage_enable = enable;
         cs = (struct radeon_cmdbuf){0};
         radv_emit_alpha_to_coverage_enable(&cmd);
         assert(cs.count == 1 && cs.addresses[0] == K_DB_ALPHA_TO_MASK && cs.values[0] == enable);
      }
   }
   for (unsigned mode = 0; mode < 8; ++mode) {
      for (unsigned flags = 0; flags < 16; ++flags) {
         cmd.state.custom_blend_mode = mode;
         pdev.info.has_rbplus = flags & 1;
         cmd.state.dynamic.vk.cb.logic_op_enable = flags & 2;
         cmd.state.dynamic.vk.cb.logic_op = 0x5a;
         cmd.state.dynamic.vk.cb.attachments[0].dual_src = flags & 4;
         cmd.state.dynamic.vk.cb.attachments[7].write_mask = (flags & 8) ? 0xf : 0;
         cs = (struct radeon_cmdbuf){0};
         radv_emit_logic_op(&cmd);
         assert(cs.count == 1 && cs.addresses[0] == K_CB_COLOR_CONTROL);
         unsigned expected = S_028808_ROP3((flags & 2) ? 0x5a : V_028808_ROP3_COPY);
         expected |= S_028808_MODE(mode ? mode : ((flags & 8) ? V_028808_CB_NORMAL : V_028808_CB_DISABLE));
         if (flags & 1)
            expected |= S_028808_DISABLE_DUAL_QUAD((flags & 6) || mode == V_028808_CB_RESOLVE);
         assert(cs.values[0] == expected);
      }
   }
   const enum amd_gfx_level levels[] = {GFX8, GFX9, GFX10, GFX10_3, GFX11, GFX11_5, GFX12};
   pdev.info.is_xclipse940 = false;
   baseline_cmd.state = cmd.state;
   for (unsigned i = 0; i < ARRAY_SIZE(levels); ++i) {
      pdev.info.gfx_level = levels[i];
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_logic_op(&cmd); radv_emit_logic_op_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_graphics_shader_pointers(&device, &cs, &bo);
      radv_emit_graphics_shader_pointers_baseline(&device, &baseline_cs, &bo);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_viewport(&cmd); radv_emit_viewport_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_fragment_shader(&cmd); radv_emit_fragment_shader_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_guardband_state(&cmd); radv_emit_guardband_state_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      baseline_cmd.state.dynamic.vk.rs.conservative_mode = cmd.state.dynamic.vk.rs.conservative_mode;
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_conservative_rast_mode(&cmd); radv_emit_conservative_rast_mode_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      baseline_cmd.state.dynamic.vk.ms.alpha_to_coverage_enable = cmd.state.dynamic.vk.ms.alpha_to_coverage_enable;
      cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
      radv_emit_alpha_to_coverage_enable(&cmd); radv_emit_alpha_to_coverage_enable_baseline(&baseline_cmd);
      assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
   }
   cs = (struct radeon_cmdbuf){0};
   assert(radv_emit_compute_fault(&device, &cs, true) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(!cs.count);
   struct radeon_winsys_bo old_gds, old_oa, new_gds, new_oa;
   struct cleanup_queue queue = {.gds_bo = &old_gds, .gds_oa_bo = &old_oa};
   for (unsigned mask = 0; mask < 4; ++mask) {
      release_count = destroy_count = 0;
      assert(run_failure_cleanup(&queue, &device, &new_gds, &new_oa, mask) == VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(destroy_count == 2 && destroyed[0] == &new_gds && destroyed[1] == &new_oa);
      assert(release_count == ((mask & 1) != 0) + ((mask & 2) != 0));
      unsigned pos = 0;
      if (mask & 1) assert(released[pos++] == &new_gds);
      if (mask & 2) assert(released[pos++] == &new_oa);
   }
   release_count = destroy_count = 0;
   run_failure_cleanup(&queue, &device, &old_gds, &old_oa, 3);
   assert(!release_count && !destroy_count);
   puts("RADV: pointers, NGG/HS metadata, fragment writes, 1..16 viewports, guardband, conservative raster, alpha-to-mask, 128 color-control cases, AMD regression and allocation failure: OK");
   pdev.info.gfx_level = GFX10_3;
   pdev.info.is_xclipse940 = true;
   device.uses_shadow_regs = false;
   cs = (struct radeon_cmdbuf){0};
   assert(radv_emit_graphics_fault(&device, &cs) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(cs.count == 0); /* Callers discard any control-only failed preamble. */
   for (unsigned clear = 0; clear < 2; ++clear) {
      for (unsigned defaults = 0; defaults < 2; ++defaults) {
         pdev.info.has_clear_state = clear;
         if (defaults) setenv("RADV_X940_DIAG_CONTEXT_DEFAULTS_V26", "1", 1);
         else unsetenv("RADV_X940_DIAG_CONTEXT_DEFAULTS_V26");
         cs = (struct radeon_cmdbuf){0};
         assert(radv_emit_graphics(&device, &cs) == VK_SUCCESS);
         unsigned zregs = 0;
         for (unsigned j = 0; j < cs.count; ++j) {
            unsigned addr = cs.addresses[j];
            assert(!(addr >= 0xb100 && addr < 0xb200));
            for (unsigned v = 0; v < 16; ++v) {
               if (addr == K_PA_SC_VPORT_ZMIN_0 + v * 0x20) {
                  assert(cs.values[j] == 0); zregs++;
               }
               if (addr == K_PA_SC_VPORT_ZMAX_0 + v * 0x20) {
                  assert(cs.values[j] == fui(1.0f)); zregs++;
               }
            }
         }
         assert(zregs == ((!clear || defaults) ? 32 : 0));
      }
   }
   unsetenv("RADV_X940_DIAG_CONTEXT_DEFAULTS_V26");
   pdev.info.is_xclipse940 = false;
   for (unsigned i = 2; i < ARRAY_SIZE(levels); ++i) {
      pdev.info.gfx_level = levels[i];
      for (unsigned clear = 0; clear < 2; ++clear) {
         pdev.info.has_clear_state = clear;
         cs = (struct radeon_cmdbuf){0}; baseline_cs = (struct radeon_cmdbuf){0};
         assert(radv_emit_graphics(&device, &cs) == VK_SUCCESS);
         radv_emit_graphics_baseline(&device, &baseline_cs);
         assert(!memcmp(&cs, &baseline_cs, sizeof(cs)));
      }
   }
   puts("Queue graphics preamble: V26 defaults, allocation failure and AMD regression: OK");
   puts("GDS/OA failure cleanup: release only newly resident BOs; preserve old BOs: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--offset-header", type=Path, required=True)
    parser.add_argument("--ip-header", type=Path, required=True)
    parser.add_argument("--mask-header", type=Path, required=True)
    parser.add_argument("--baseline-ref", default="75db695acee70d0c4a4fa8228c2a323919c2a548")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    root = args.root.resolve()
    mapping = kernel_map(args.offset_header, args.ip_header)
    with tempfile.TemporaryDirectory(prefix="x940-emission-test-") as tmp:
        tmp = Path(tmp)
        (tmp / "amdgfxregs.h").write_text(generate_amd_header(root))
        masks = numeric_defines(args.mask_header)
        field_checks = []
        cmd_source = (root / "src/amd/vulkan/radv_cmd_buffer.c").read_text()
        shader_source = (root / "src/amd/vulkan/radv_shader.c").read_text()
        field_groups = {
            "0286D8": "SPI_PS_IN_CONTROL", "028644": "SPI_PS_INPUT_CNTL_0",
            "028810": "PA_CL_CLIP_CNTL", "028814": "PA_SU_SC_MODE_CNTL",
            "02842C": "DB_STENCIL_CONTROL", "028430": "DB_STENCILREFMASK",
            "028434": "DB_STENCILREFMASK_BF", "028C4C": "PA_SC_CONSERVATIVE_RASTERIZATION_CNTL",
            "028B6C": "VGT_TF_PARAM", "00B324": "SPI_SHADER_PGM_HI_ES",
            "030984": "VGT_TF_MEMORY_BASE_HI", "028808": "CB_COLOR_CONTROL",
        }
        inactive_fields = {("028644", "PRIM_ATTR")}  # GFX11 per-primitive path, not MGFX2/GFX10.3.
        for prefix, reg in field_groups.items():
            fields = sorted(set(re.findall(r"\bS_" + prefix + r"_([A-Z0-9_]+)\(", cmd_source + shader_source + (root / "src/amd/vulkan/radv_queue.c").read_text())))
            for field in fields:
                # Generation-specific field aliases are reviewed separately.
                if re.search(r"_GFX[0-9]", field) or (prefix, field) in inactive_fields:
                    continue
                key = reg + "__" + field
                if key + "_MASK" not in masks or key + "__SHIFT" not in masks:
                    raise ValueError(f"Retained field has no MGFX2 definition: S_{prefix}_{field} ({reg})")
                mask, shift = masks[key + "_MASK"], masks[key + "__SHIFT"]
                macro = f"S_{prefix}_{field}"
                field_checks.append(f"for (unsigned b = 0; b < 32; b++) assert({macro}(1u << b) == (((1u << b) & 0x{mask >> shift:x}u) << {shift}));")
        check_fields = "static void check_fields(void) {\n" + "\n".join(field_checks) + "\n}\n"
        util = (root / "src/amd/common/ac_shader_util.h").read_text()
        decls = '#define AC_SHADER_UTIL_H\n#include "ac_gpu_info.h"\n'
        for name in ("gfx12_load_temporal_hint", "gfx12_store_temporal_hint", "gfx12_speculative_data_read"):
            decls += enum(util, name) + "\n"
        decls += "uint32_t ac_apply_cu_en(uint32_t, uint32_t, unsigned, const struct radeon_info *);\n"
        (tmp / "decls.h").write_text(decls)
        helpers = DEPENDENCIES + function((root / "src/amd/common/ac_shader_util.c").read_text(), "ac_apply_cu_en")
        helpers += "\n" + function((root / "src/amd/common/ac_gpu_info.c").read_text(), "ac_gfx103_get_cu_mask_ps")
        (tmp / "common_test.c").write_text(helpers + COMMON_MAIN)
        baseline = run(["git", "show", f"{args.baseline_ref}:src/amd/common/ac_cmdbuf.c"], cwd=root).stdout
        (tmp / "baseline_cmdbuf.c").write_text(baseline)
        flags = [args.cc, "-std=c11", "-D_DEFAULT_SOURCE", "-DHAVE_ENDIAN_H",
                 "-Werror=implicit-function-declaration", "-I" + str(tmp), "-I" + str(root / "src"),
                 "-I" + str(root / "src/amd/common"), "-I" + str(root / "include"),
                 "-include", str(tmp / "decls.h")]
        common_sources = [str(root / "src/amd/common/ac_pm4.c"), str(tmp / "common_test.c")]
        for label, source in (("current", root / "src/amd/common/ac_cmdbuf.c"), ("baseline", tmp / "baseline_cmdbuf.c")):
            run(flags + [str(source), *common_sources, "-lm", "-o", str(tmp / label)])
        assert run([str(tmp / "current")]).stdout == run([str(tmp / "baseline")]).stdout, "AMD common preamble changed"
        print("AMD common preamble: 10 cases (GFX10..GFX12, cache on/off) byte-identical: OK")
        mgfx2 = run([str(tmp / "current"), "x940"]).stdout
        words = [int(line, 16) for line in mgfx2.split("CASE ")[1].splitlines()[1:]]
        writes = {}
        pos = 0
        while pos < len(words):
            h = words[pos]; size = ((h >> 16) & 0x3fff) + 2; opcode = (h >> 8) & 255
            assert h >> 30 == 3 and pos + size <= len(words)
            bases = {0x69: 0x28000, 0x76: 0xb000, 0x9b: 0xb000, 0x79: 0x30000, 0x7a: 0x30000}
            if opcode in bases:
                addr = bases[opcode] + (words[pos + 1] & 0xffff) * 4
                for i, value in enumerate(words[pos + 2:pos + size]): writes[addr + 4*i] = value
            pos += size
        assert not any(0xb100 <= addr < 0xb200 for addr in writes), "Legacy VS write on MGFX2"
        for name in ("SPI_SHADER_PGM_HI_ES", "SPI_SHADER_PGM_HI_LS", "SPI_SHADER_PGM_RSRC3_PS",
                     "SPI_SHADER_PGM_RSRC4_PS", "SPI_SHADER_PGM_RSRC3_HS", "SPI_SHADER_PGM_RSRC4_HS"):
            assert mapping[name] in writes, f"Lost required preamble write: {name}"
        assert writes[mapping["SPI_SHADER_PGM_HI_ES"]] == 0x80
        print("MGFX2 common preamble: no legacy VS; PS/HS setup retained; ES high address: OK")
        queue = (root / "src/amd/vulkan/radv_queue.c").read_text()
        cmd = (root / "src/amd/vulkan/radv_cmd_buffer.c").read_text()
        shader = (root / "src/amd/vulkan/radv_shader.c").read_text()
        test = helpers + RADV_MODEL + check_fields
        for name, text in (("radv_emit_graphics_shader_pointers", queue), ("radv_emit_fragment_shader", cmd),
                           ("radv_emit_viewport", cmd), ("radv_emit_guardband_state", cmd),
                           ("radv_emit_conservative_rast_mode", cmd), ("radv_emit_alpha_to_coverage_enable", cmd),
                           ("radv_emit_logic_op", cmd)):
            test += "\n" + function(text, name)
            old = run(["git", "show", f"{args.baseline_ref}:src/amd/vulkan/" + ("radv_queue.c" if text == queue else "radv_cmd_buffer.c")], cwd=root).stdout
            test += "\n" + function(old, name).replace(name + "(", name + "_baseline(", 1)
        # Helpers must be defined before the viewport emitter.
        viewport_helpers = function(cmd, "radv_get_viewport_zscale_ztranslate") + "\n" + function(cmd, "radv_get_viewport_zmin_zmax")
        test = test.replace(function(cmd, "radv_emit_viewport"), viewport_helpers + "\n" + function(cmd, "radv_emit_viewport"), 1)
        test += "\n" + function(shader, "radv_precompute_registers_pgm")
        test += "\n" + function(queue, "radv_pack_float_12p4")
        test += "\n" + function(queue, "radv_emit_compute")
        test += "\n" + function(queue, "radv_emit_graphics")
        test += "\n#define ac_pm4_create_sized fault_pm4_create\n"
        for name in ("radv_emit_compute", "radv_emit_graphics"):
            test += function(queue, name).replace(name + "(", name + "_fault(", 1) + "\n"
        test += "\n#undef ac_pm4_create_sized\n"
        old_queue = run(["git", "show", f"{args.baseline_ref}:src/amd/vulkan/radv_queue.c"], cwd=root).stdout
        test += "\n" + function(old_queue, "radv_emit_compute").replace("radv_emit_compute(", "radv_emit_compute_baseline(")
        test += "\n" + function(old_queue, "radv_emit_graphics").replace("radv_emit_graphics(", "radv_emit_graphics_baseline(").replace("radv_emit_compute(", "radv_emit_compute_baseline(")

        # Compile the real failure block, with fake BOs and residency callbacks.
        cleanup = function(queue, "radv_update_preamble_cs").split("\nfail:\n", 1)[1]
        test += r'''
static VkResult run_failure_cleanup(struct cleanup_queue *queue, struct radv_device *device,
   struct radeon_winsys_bo *gds_bo, struct radeon_winsys_bo *gds_oa_bo, unsigned resident_mask) {
   struct cleanup_winsys winsys = {.buffer_make_resident = cleanup_resident}, *ws = &winsys;
   bool new_gds_resident = resident_mask & 1, new_gds_oa_resident = resident_mask & 2;
   struct radeon_cmdbuf *dest_cs[3] = {0};
   struct radeon_winsys_bo *descriptor_bo = NULL, *scratch_bo = NULL, *compute_scratch_bo = NULL,
      *esgs_ring_bo = NULL, *gsvs_ring_bo = NULL, *tess_rings_bo = NULL, *task_rings_bo = NULL, *attr_ring_bo = NULL;
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
''' + cleanup
        test += "\n" + "\n".join(f"#define K_{name} 0x{addr:x}u" for name, addr in mapping.items()) + RADV_MAIN
        (tmp / "radv_test.c").write_text(test)
        run(flags + [str(tmp / "radv_test.c"), str(root / "src/amd/common/ac_pm4.c"),
                     str(root / "src/amd/common/ac_cmdbuf.c"), "-lm", "-o", str(tmp / "radv_test")])
        print(run([str(tmp / "radv_test")]).stdout.strip())
        print(f"Retained fields: {len(field_checks)} macros, each input bit checked against Samsung masks: OK")
    print("Host validation complete. Full Android build and GPU execution remain required.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stdout, file=sys.stderr)
        print(error.stderr, file=sys.stderr)
        raise SystemExit(error.returncode)
