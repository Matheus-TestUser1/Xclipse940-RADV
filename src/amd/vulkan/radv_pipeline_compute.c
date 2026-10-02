/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meta/radv_meta.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir/nir_serialize.h"
#include "nir/radv_nir.h"
#include "spirv/nir_spirv.h"
#include "util/disk_cache.h"
#include "util/mesa-sha1.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "radv_cs.h"
#include "radv_debug.h"
#include "radv_pipeline_binary.h"
#include "radv_pipeline_cache.h"
#include "radv_rmv.h"
#include "radv_shader.h"
#include "radv_shader_args.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"
#include "vk_render_pass.h"
#include "vk_util.h"

#include "util/u_debug.h"
#include "ac_binary.h"
#include "ac_nir.h"
#include "ac_shader_util.h"
#include "aco_interface.h"
#include "sid.h"
#include "vk_format.h"

uint32_t
radv_get_compute_resource_limits(const struct radv_physical_device *pdev, const struct radv_shader_info *info)
{
   unsigned threads_per_threadgroup;
   unsigned threadgroups_per_cu = 1;
   unsigned waves_per_threadgroup;
   unsigned max_waves_per_sh = 0;

   /* Calculate best compute resource limits. */
   threads_per_threadgroup = info->cs.block_size[0] * info->cs.block_size[1] * info->cs.block_size[2];
   waves_per_threadgroup = DIV_ROUND_UP(threads_per_threadgroup, info->wave_size);

   if (pdev->info.gfx_level >= GFX10 && waves_per_threadgroup == 1)
      threadgroups_per_cu = 2;

   return ac_get_compute_resource_limits(&pdev->info, waves_per_threadgroup, max_waves_per_sh, threadgroups_per_cu);
}

void
radv_get_compute_shader_metadata(const struct radv_device *device, const struct radv_shader *cs,
                                 struct radv_compute_pipeline_metadata *metadata)
{
   uint32_t upload_sgpr = 0, inline_sgpr = 0;

   memset(metadata, 0, sizeof(*metadata));

   metadata->wave32 = cs->info.wave_size == 32;

   metadata->grid_base_sgpr = radv_get_user_sgpr(cs, AC_UD_CS_GRID_SIZE);

   upload_sgpr = radv_get_user_sgpr(cs, AC_UD_PUSH_CONSTANTS);
   inline_sgpr = radv_get_user_sgpr(cs, AC_UD_INLINE_PUSH_CONSTANTS);

   metadata->push_const_sgpr = upload_sgpr | (inline_sgpr << 16);
   metadata->inline_push_const_mask = cs->info.inline_push_constant_mask;

   metadata->indirect_desc_sets_sgpr = radv_get_user_sgpr(cs, AC_UD_INDIRECT_DESCRIPTOR_SETS);
}

void
radv_compute_pipeline_init(struct radv_compute_pipeline *pipeline, const struct radv_pipeline_layout *layout,
                           struct radv_shader *shader)
{
   pipeline->base.need_indirect_descriptor_sets |= radv_shader_need_indirect_descriptor_sets(shader);

   pipeline->base.push_constant_size = layout->push_constant_size;
   pipeline->base.dynamic_offset_count = layout->dynamic_offset_count;
}

struct radv_shader *
radv_compile_cs(struct radv_device *device, struct vk_pipeline_cache *cache, struct radv_shader_stage *cs_stage,
                bool keep_executable_info, bool keep_statistic_info, bool is_internal, bool skip_shaders_cache,
                struct radv_shader_binary **cs_binary)
{
   struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_instance *instance = radv_physical_device_instance(pdev);

   struct radv_shader *cs_shader;

   /* Compile SPIR-V shader to NIR. */
   cs_stage->nir = radv_shader_spirv_to_nir(device, cs_stage, NULL, is_internal);

   radv_optimize_nir(cs_stage->nir, cs_stage->key.optimisations_disabled);

   /* Gather info again, information such as outputs_read can be out-of-date. */
   nir_shader_gather_info(cs_stage->nir, nir_shader_get_entrypoint(cs_stage->nir));

   /* Run the shader info pass. */
   radv_nir_shader_info_init(cs_stage->stage, MESA_SHADER_NONE, &cs_stage->info);
   radv_nir_shader_info_pass(device, cs_stage->nir, &cs_stage->layout, &cs_stage->key, NULL, RADV_PIPELINE_COMPUTE,
                             false, &cs_stage->info);

   radv_declare_shader_args(device, NULL, &cs_stage->info, MESA_SHADER_COMPUTE, MESA_SHADER_NONE, &cs_stage->args);

   cs_stage->info.user_sgprs_locs = cs_stage->args.user_sgprs_locs;
   cs_stage->info.inline_push_constant_mask = cs_stage->args.ac.inline_push_const_mask;

   /* Postprocess NIR. */
   radv_postprocess_nir(device, NULL, cs_stage);

   bool dump_shader = radv_can_dump_shader(device, cs_stage->nir, false);

   if (dump_shader) {
      simple_mtx_lock(&instance->shader_dump_mtx);
      nir_print_shader(cs_stage->nir, stderr);
   }

   /* Compile NIR shader to AMD assembly. */
   *cs_binary =
      radv_shader_nir_to_asm(device, cs_stage, &cs_stage->nir, 1, NULL, keep_executable_info, keep_statistic_info);

   /* Diagnostic only. The port reports GFX10_3 to ACO, which encodes
    * SOFFSET NULL as 125. RDNA3 encodes NULL as 124 and M0 as 125.
    * Touch only the exact S_LOAD_DWORD pair seen in the guarded probe.
    */
   if (pdev->info.is_xclipse940 && !is_internal && getenv("RADV_X940_TEST_SMEM_NULL") &&
       *cs_binary && (*cs_binary)->type == RADV_BINARY_TYPE_LEGACY) {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)*cs_binary;
      uint8_t *code = bin->data + bin->stats_size;
      bool replaced = false;
      for (unsigned i = 0; (i + 1u) * 4u < bin->code_size; ++i) {
         uint32_t instruction, offset;
         memcpy(&instruction, code + i * 4u, 4u);
         memcpy(&offset, code + (i + 1u) * 4u, 4u);
         if (instruction == 0xf4000000u && offset == 0xfa000000u) {
            offset = 0xf8000000u; /* SOFFSET: 125 (M0) -> 124 (NULL) */
            memcpy(code + (i + 1u) * 4u, &offset, 4u);
            fprintf(stderr, "x940-smem-null: patched word[%u] to 0x%08x\n", i + 1u, offset);
            replaced = true;
            break;
         }
      }
      if (!replaced)
         fprintf(stderr, "x940-smem-null: expected instruction not found; shader unchanged\n");
   }


   /* Diagnostic for the exact one-store SSBO v3 shader only. The shader
    * already loads the 4-DWORD descriptor into s[0:3]. Replace its MUBUF
    * write with: sign-extend descriptor VA high word (s1), set v1=0,
    * GLOBAL_STORE_B32 v1, s[0:1], v0, END_PGM. No other code is touched.
    */
   if (pdev->info.is_xclipse940 && !is_internal && getenv("RADV_X940_TEST_SSBO_AS_GLOBAL") &&
       *cs_binary && (*cs_binary)->type == RADV_BINARY_TYPE_LEGACY) {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)*cs_binary;
      static const uint32_t expected[14] = {
         0xb0038000u, 0xf4080001u, 0xf8000000u, 0x7e0002ffu,
         0x12345678u, 0xbf8cc07fu, 0xe0680000u, 0x80000080u,
         0xbf810000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
         0xbf9f0000u, 0xbf9f0000u,
      };
      static const uint32_t replacement[14] = {
         0xb0038000u, 0xf4080001u, 0xf8000000u, 0x7e0002ffu,
         0x12345678u, 0xbf8cc07fu,
         0xbe810f01u, /* s_sext_i32_i16 s1, s1: 0x00008001 -> 0xffff8001 */
         0x7e020280u, /* v_mov_b32 v1, 0 */
         0xdc6a0000u, /* GLOBAL_STORE_B32, SEG=global */
         0x00000001u, /* addr=v1, data=v0, saddr=s[0:1] */
         0xbf810000u, /* s_endpgm */
         0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
      };
      uint8_t *code = bin->data + bin->stats_size;
      if (bin->code_size == sizeof(expected) && !memcmp(code, expected, sizeof(expected))) {
         memcpy(code, replacement, sizeof(replacement));
         fprintf(stderr, "x940-ssbo-global: patched exact 56-byte shader; descriptor VA from s[0:1]\n");
      } else {
         fprintf(stderr, "x940-ssbo-global: shader signature differs (bytes=%u); unchanged\n",
                 bin->code_size);
      }
   }


   /* One exact 56-byte shader, one invocation. Return GPU-fetched descriptor
    * DW0 through a separate BDA target supplied by the probe itself.
    * Keep the test isolated from the normal buffer-store opcode.
    */
   if (pdev->info.is_xclipse940 && !is_internal &&
       getenv("RADV_X940_DIAG_BDA_VA") &&
       *cs_binary && (*cs_binary)->type == RADV_BINARY_TYPE_LEGACY) {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)*cs_binary;
      static const uint32_t expected[14] = {
         0xb0038000u, 0xf4080001u, 0xf8000000u, 0x7e0002ffu,
         0x12345678u, 0xbf8cc07fu, 0xe0680000u, 0x80000080u,
         0xbf810000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
         0xbf9f0000u, 0xbf9f0000u,
      };
      const char *addr_text = getenv("RADV_X940_DIAG_BDA_VA");
      char *end = NULL;
      unsigned long long addr = strtoull(addr_text, &end, 0);
      uint8_t *code = bin->data + bin->stats_size;
      uint32_t expected_alt[14];
      memcpy(expected_alt, expected, sizeof(expected_alt));
      expected_alt[6] = 0xe0700000u; /* observed baseline: only word[6] differs */
      if (!addr_text[0] || end == addr_text || *end || !addr || (addr & 3u)) {
         fprintf(stderr, "x940-desc-read: invalid BDA address; no patch\n");
      } else if (bin->code_size != sizeof(expected) ||
                 (memcmp(code, expected, sizeof(expected)) &&
                  memcmp(code, expected_alt, sizeof(expected_alt)))) {
         fprintf(stderr, "x940-desc-read: shader signature mismatch (%u bytes); no patch\n",
                 bin->code_size);
      } else {
         uint32_t replacement[14] = {
            0xb0038000u, /* s_movk_i32 s3, -32768: set VA high */
            0xf4080001u, 0xf8000000u, /* s_load_b128 s0:s3, s2:s3, 0 */
            0xbf8cc07fu, /* s_waitcnt lgkmcnt(0) */
            0x7e000200u, /* v_mov_b32 v0, s0: fetched descriptor DW0 */
            0xbe8000ffu, (uint32_t)addr, /* s_mov_b32 s0, BDA low */
            0xbe8100ffu, (uint32_t)(addr >> 32), /* s_mov_b32 s1, BDA high */
            0x7e020280u, /* v_mov_b32 v1, 0: offset */
            0xdc6a0000u, 0x00000001u, /* GLOBAL_STORE_B32 v1, v0, s[0:1] */
            0xbf810000u, 0xbf9f0000u, /* endpgm; nop */
         };
         if (getenv("RADV_X940_DIAG_INPUT_SGPR2")) {
            uint32_t sgpr2_code[14] = {
               0x7e000202u, /* v_mov_b32 v0, s2 (before any SMEM) */
               0xbe8000ffu, (uint32_t)addr, /* s_mov_b32 s0, BDA low */
               0xbe8100ffu, (uint32_t)(addr >> 32), /* s_mov_b32 s1, BDA high */
               0x7e020280u, /* v_mov_b32 v1, 0 */
               0xdc6a0000u, 0x00000001u, /* GLOBAL_STORE_B32 v1, v0, s[0:1] */
               0xbf810000u, /* s_endpgm */
               0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
               0xbf9f0000u, 0xbf9f0000u,
            };
            memcpy(code, sgpr2_code, sizeof(sgpr2_code));
            fprintf(stderr, "x940-desc-read: SGPR2 capture; output BDA=0x%016llx\n", addr);
         } else if (getenv("RADV_X940_DIAG_GPU_STORE_LOAD")) {
            if ((addr >> 32) != 0xffff8001ull) {
               fprintf(stderr, "x940-gpu-store-load: unsupported BDA high word; no patch\n");
            } else {
               /* RDNA3: store 7 to BDA[0]; wait for vector stores;
                * load BDA[0]; wait for vector loads; store to BDA[1].
                * OFFSET in GLOBAL_STORE_B32 is a signed byte offset.
                */
               uint32_t gpu_code[14] = {
                  0xbe8000ffu, (uint32_t)addr, /* s0 = BDA low */
                  0xb0018001u, /* s_movk_i32 s1, 0xffff8001 */
                  0x7e000287u, /* v_mov_b32 v0, 7 */
                  0x7e020280u, /* v_mov_b32 v1, 0 */
                  0xdc6a0000u, 0x00000001u, /* GLOBAL_STORE_B32 BDA[0] = 7 */
                  0xbc7d0000u, /* s_waitcnt_vscnt null, 0 */
                  0xdc520000u, 0x00000001u, /* GLOBAL_LOAD_B32 v0 = BDA[0] */
                  0xbf8c0000u, /* s_waitcnt vmcnt(0) */
                  0xdc6a0004u, 0x00000001u, /* GLOBAL_STORE_B32 BDA[1] = v0 */
                  0xbf810000u, /* s_endpgm */
               };
               memcpy(code, gpu_code, sizeof(gpu_code));
               fprintf(stderr, "x940-gpu-store-load: GPU writes 7, then reads BDA[0] into BDA[1]; VA=0x%016llx\n",
                       addr);
            }
         } else if (getenv("RADV_X940_DIAG_GPU_OFFSET4_ROUNDTRIP")) {
            if ((addr >> 32) != 0xffff8001ull || bin->code_size != 56) {
               fprintf(stderr, "x940-offset4: BDA/high word or code size unsupported; shader unchanged\n");
            } else {
               /* 56-byte shader, matching the existing BDA_SELF_LOAD's
                * v1=4 GLOBAL_LOAD_B32. Use a GPU write as the producer.
                */
               uint32_t offset4_code[14] = {
                  0xbe8000ffu, (uint32_t)addr, /* s0 = BDA low */
                  0xb0018001u, /* s1 = 0xffff8001 */
                  0x7e000287u, /* v0 = 7 */
                  0x7e020284u, /* v1 = byte offset 4 */
                  0xdc6a0000u, 0x00000001u, /* BDA[1] = 7 */
                  0xbc7d0000u, /* wait vscnt(0) */
                  0xdc520000u, 0x00000001u, /* v0 = BDA[1] via v1=4 */
                  0xbf8c0000u, /* wait vmcnt(0) */
                  0xdc6a1ffcu, 0x00000001u, /* BDA[0] = v0: v1=4 + (-4) */
                  0xbf810000u, /* s_endpgm, word[13] */
               };
               memcpy(code, offset4_code, sizeof(offset4_code));
               fprintf(stderr, "x940-offset4: GPU store BDA[1]=7, v1=4 load BDA[1], store BDA[0]; VA=0x%016llx\n", addr);
            }
         } else if (getenv("RADV_X940_DIAG_BDA_SELF_LOAD")) {
            /* Exact 56-byte shader: BDA[1] -> BDA[0].  Host preloads
             * BDA[0]=deadbeef, BDA[1]=1a2b3c4d before queue submit.
             * GLOBAL_STORE_B32 has already been tested on this target.
             */
            uint32_t self_code[14] = {
               0xbe8000ffu, (uint32_t)addr, /* s0 = BDA low */
               0xbe8100ffu, (uint32_t)(addr >> 32), /* s1 = BDA high */
               0x7e020284u, /* v1 = byte offset 4 */
               0xdc520000u, 0x00000001u, /* GLOBAL_LOAD_B32 v0, v1, s[0:1] */
               0xbf8c0000u, /* wait for vmcnt(0) before using v0 */
               0x7e020280u, /* v1 = byte offset 0 */
               0xdc6a0000u, 0x00000001u, /* GLOBAL_STORE_B32 v1, v0, s[0:1] */
               0xbf810000u, /* s_endpgm */
               0xbf9f0000u, 0xbf9f0000u, /* padding */
            };
            const char *cache = getenv("RADV_X940_DIAG_BDA_READ_CACHE");
            if (cache && !strcmp(cache, "glc"))
               self_code[5] |= 0x4000u; /* GLC: bypass L0 */
            else if (cache && !strcmp(cache, "dlc"))
               self_code[5] |= 0xa000u; /* DLC | SLC: change L2 policy */
            else if (cache && !strcmp(cache, "all"))
               self_code[5] |= 0xe000u; /* GLC | DLC | SLC */
            else if (cache) {
               fprintf(stderr, "x940-bda-cache: invalid policy; no shader replacement\n");
               cache = "invalid";
            }
            if (!cache || strcmp(cache, "invalid")) {
               if (getenv("RADV_X940_DIAG_FLAT_LOAD")) {
                  /* Change only the load's addressing path: v[0:1] holds
                   * the BDA VA, and v2 is poisoned before the FLAT load.
                   * The final GLOBAL store uses the same base and offset
                   * as the original self-load diagnostic.
                   */
                  uint32_t flat_code[14] = {
                     0xbe8000ffu, (uint32_t)addr, /* s0 = BDA low */
                     0xbe8100ffu, (uint32_t)(addr >> 32), /* s1 = BDA high */
                     0x7e000200u, /* v_mov_b32 v0, s0 */
                     0x7e020201u, /* v_mov_b32 v1, s1 */
                     0x7e040289u, /* v_mov_b32 v2, 9 */
                     0xdc500004u, 0x027c0000u, /* FLAT_LOAD_B32 v2, v[0:1] offset:4 */
                     0xbf8c0000u, /* s_waitcnt vmcnt(0) lgkmcnt(0) */
                     0x7e020280u, /* v_mov_b32 v1, 0 */
                     0xdc6a0000u, 0x00000201u, /* GLOBAL_STORE_B32 v1, v2, s[0:1] */
                     0xbf810000u, /* s_endpgm */
                  };
                  if (getenv("RADV_X940_DIAG_GFX11_WAIT")) {
                     flat_code[9] = 0xbf890000u; /* GFX11 S_WAITCNT vmcnt(0) lgkmcnt(0) expcnt(0) */
                     fprintf(stderr, "x940-gfx11-wait: FLAT wait word[9]=0x%08x\n", flat_code[9]);
                  }
                  memcpy(code, flat_code, sizeof(flat_code));
                  fprintf(stderr, "x940-bda-flat-load: v2=9; FLAT_LOAD_B32 BDA+4 -> v2, GLOBAL_STORE_B32 v2 -> BDA; VA=0x%016llx\n", addr);
               } else {
                  if (getenv("RADV_X940_DIAG_GFX11_WAIT")) {
                     self_code[7] = 0xbf890000u; /* GFX11 S_WAITCNT, before optional poison shift */
                     fprintf(stderr, "x940-gfx11-wait: GLOBAL wait word[7]=0x%08x (before poison)\n", self_code[7]);
                  }
                  if (getenv("RADV_X940_DIAG_POISON_LOAD")) {
                     /* Run after the cache policy has updated word[5].
                      * Move instructions one slot, replacing the final NOP.
                      */
                     memmove(&self_code[6], &self_code[5],
                             8 * sizeof(self_code[0]));
                     self_code[5] = 0x7e000289u; /* v_mov_b32 v0, 9 */
                     fprintf(stderr, "x940-bda-poison-load: v0=9 before GLOBAL_LOAD_B32; expected BDA[1]=0x1a2b3c4d\n");
                  }
                  memcpy(code, self_code, sizeof(self_code));
                  fprintf(stderr,
                          "x940-bda-cache: policy=%s GLOBAL_LOAD_B32 word[%u]=0x%08x\n",
                          cache ? cache : "default",
                          getenv("RADV_X940_DIAG_POISON_LOAD") ? 6u : 5u,
                          self_code[getenv("RADV_X940_DIAG_POISON_LOAD") ? 6 : 5]);
                  fprintf(stderr, "x940-bda-self-load: GPU reads BDA+4 and writes BDA; VA=0x%016llx\n", addr);
               }
            }
         } else if (getenv("RADV_X940_DIAG_GLOBAL_DESC_LOAD")) {
            uint32_t vmem_code[14] = {
               0xb0038000u, /* s_movk_i32 s3, -32768; s[2:3] = set VA */
               0x7e020280u, /* v_mov_b32 v1, 0: byte offset */
               0xdc520000u, 0x00020001u, /* GLOBAL_LOAD_B32 v0, v1, s[2:3] */
               0xbf8c0000u, /* s_waitcnt vmcnt(0), lgkmcnt(0), expcnt(0) */
               0xbe8000ffu, (uint32_t)addr, /* s_mov_b32 s0, BDA low */
               0xbe8100ffu, (uint32_t)(addr >> 32), /* s_mov_b32 s1, BDA high */
               0xdc6a0000u, 0x00000001u, /* GLOBAL_STORE_B32 v1, v0, s[0:1] */
               0xbf810000u, /* s_endpgm */
               0xbf9f0000u, 0xbf9f0000u, /* nops */
            };
            /* Previous tests used a descriptor BO in high VA 0xffff8000.
             * On this test only, place the pool outside VA_RANGE_32_BIT.
             * Its actual high word must be verified against x940-descset.
             */
            if (getenv("RADV_X940_DIAG_DESC_OUTSIDE_32BIT"))
               vmem_code[0] = 0xb0038001u; /* s_movk_i32 s3, -32767 => 0xffff8001 */
            memcpy(code, vmem_code, sizeof(vmem_code));
            fprintf(stderr, "x940-desc-read: GLOBAL_LOAD set_hi=0x%08x; output BDA=0x%016llx\n",
                    getenv("RADV_X940_DIAG_DESC_OUTSIDE_32BIT") ? 0xffff8001u : 0xffff8000u, addr);
         } else {
            memcpy(code, replacement, sizeof(replacement));
            fprintf(stderr, "x940-desc-read: patched; output BDA=0x%016llx\n", addr);
         }
      }
   }

   /* Diagnostic: fix the wait for the exact 56-byte descriptor probe.
    * ACO uses GFX10.3 here, where 0xbf8cc07f is S_WAITCNT lgkmcnt(0).
    * On GFX11 the same wait is 0xbf89fc07. Check the whole 3-word prefix
    * and the expected position to avoid rewriting arbitrary shader data.
    */
   if (pdev->info.is_xclipse940 && !is_internal && getenv("RADV_X940_DIAG_GFX11_WAIT") &&
       *cs_binary && (*cs_binary)->type == RADV_BINARY_TYPE_LEGACY) {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)*cs_binary;
      static const uint32_t signature[3] = {0xb0038000u, 0xf4080001u, 0xf8000000u};
      uint8_t *code = bin->data + bin->stats_size;
      if (bin->code_size == 56 && !memcmp(code, signature, sizeof(signature))) {
         bool replaced = false;
         for (unsigned i = 3; i <= 5; i += 2) {
            uint32_t instruction;
            memcpy(&instruction, code + i * 4u, sizeof(instruction));
            if (instruction == 0xbf8cc07fu) {
               const uint32_t gfx11_wait = 0xbf89fc07u;
               memcpy(code + i * 4u, &gfx11_wait, sizeof(gfx11_wait));
               fprintf(stderr, "x940-gfx11-wait: descriptor wait word[%u]=0x%08x (was 0x%08x)\n",
                       i, gfx11_wait, instruction);
               replaced = true;
               break;
            }
         }
         if (!replaced)
            fprintf(stderr, "x940-gfx11-wait: descriptor signature matched, wait not found; unchanged\n");
      }
   }

   cs_shader = radv_shader_create(device, cache, *cs_binary, skip_shaders_cache || dump_shader);

   /* Diagnostic only: inspect the final ACO binary even without LLVM's
    * disassembler. This does not change compilation or shader upload.
    */
   if (pdev->info.is_xclipse940 && !is_internal && getenv("RADV_X940_DUMP_CODE") &&
       cs_shader && *cs_binary && (*cs_binary)->type == RADV_BINARY_TYPE_LEGACY) {
      const struct radv_shader_binary_legacy *bin = (const struct radv_shader_binary_legacy *)*cs_binary;
      const uint8_t *code = bin->data + bin->stats_size;
      const unsigned words = bin->code_size / 4u < 48u ? bin->code_size / 4u : 48u;
      fprintf(stderr, "x940-isa: va=0x%016llx bytes=%u first_words=%u\n",
              (unsigned long long)cs_shader->va, bin->code_size, words);
      for (unsigned i = 0; i < words; ++i) {
         uint32_t word;
         memcpy(&word, code + i * 4u, sizeof(word));
         fprintf(stderr, "x940-isa: word[%u]=0x%08x\n", i, word);
      }
   }

   radv_shader_generate_debug_info(device, dump_shader, keep_executable_info, *cs_binary, cs_shader, &cs_stage->nir, 1,
                                   &cs_stage->info);

   if (dump_shader)
      simple_mtx_unlock(&instance->shader_dump_mtx);

   if (keep_executable_info && cs_stage->spirv.size) {
      cs_shader->spirv = malloc(cs_stage->spirv.size);
      memcpy(cs_shader->spirv, cs_stage->spirv.data, cs_stage->spirv.size);
      cs_shader->spirv_size = cs_stage->spirv.size;
   }

   return cs_shader;
}

void
radv_compute_pipeline_hash(const struct radv_device *device, const VkComputePipelineCreateInfo *pCreateInfo,
                           unsigned char *hash)
{
   VkPipelineCreateFlags2KHR create_flags = vk_compute_pipeline_create_flags(pCreateInfo);
   VK_FROM_HANDLE(radv_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   const VkPipelineShaderStageCreateInfo *sinfo = &pCreateInfo->stage;
   struct mesa_sha1 ctx;

   struct radv_shader_stage_key stage_key =
      radv_pipeline_get_shader_key(device, sinfo, create_flags, pCreateInfo->pNext);

   _mesa_sha1_init(&ctx);
   radv_pipeline_hash(device, pipeline_layout, &ctx);
   radv_pipeline_hash_shader_stage(create_flags, sinfo, &stage_key, &ctx);
   _mesa_sha1_final(&ctx, hash);
}

static VkResult
radv_compute_pipeline_compile(const VkComputePipelineCreateInfo *pCreateInfo, struct radv_compute_pipeline *pipeline,
                              struct radv_pipeline_layout *pipeline_layout, struct radv_device *device,
                              struct vk_pipeline_cache *cache, const VkPipelineShaderStageCreateInfo *pStage,
                              const VkPipelineCreationFeedbackCreateInfo *creation_feedback)
{
   struct radv_shader_binary *cs_binary = NULL;
   bool keep_executable_info = radv_pipeline_capture_shaders(device, pipeline->base.create_flags);
   bool keep_statistic_info = radv_pipeline_capture_shader_stats(device, pipeline->base.create_flags);
   struct radv_shader_stage cs_stage = {0};
   VkPipelineCreationFeedback pipeline_feedback = {
      .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
   };
   bool skip_shaders_cache = false;
   VkResult result = VK_SUCCESS;

   int64_t pipeline_start = os_time_get_nano();

   radv_compute_pipeline_hash(device, pCreateInfo, pipeline->base.sha1);

   pipeline->base.pipeline_hash = *(uint64_t *)pipeline->base.sha1;

   /* Skip the shaders cache when any of the below are true:
    * - shaders are captured because it's for debugging purposes
    * - binaries are captured for later uses
    */
   if (keep_executable_info || (pipeline->base.create_flags & VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR)) {
      skip_shaders_cache = true;
   }

   bool found_in_application_cache = true;
   if (!skip_shaders_cache &&
       radv_compute_pipeline_cache_search(device, cache, pipeline, &found_in_application_cache)) {
      if (found_in_application_cache)
         pipeline_feedback.flags |= VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      result = VK_SUCCESS;
      goto done;
   }

   if (pipeline->base.create_flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   int64_t stage_start = os_time_get_nano();

   const struct radv_shader_stage_key stage_key =
      radv_pipeline_get_shader_key(device, &pCreateInfo->stage, pipeline->base.create_flags, pCreateInfo->pNext);

   radv_pipeline_stage_init(pipeline->base.create_flags, pStage, pipeline_layout, &stage_key, &cs_stage);

   pipeline->base.shaders[MESA_SHADER_COMPUTE] =
      radv_compile_cs(device, cache, &cs_stage, keep_executable_info, keep_statistic_info, pipeline->base.is_internal,
                      skip_shaders_cache, &cs_binary);

   cs_stage.feedback.duration += os_time_get_nano() - stage_start;

   if (!skip_shaders_cache) {
      radv_pipeline_cache_insert(device, cache, &pipeline->base);
   }

   free(cs_binary);
   if (radv_can_dump_shader_stats(device, cs_stage.nir)) {
      radv_dump_shader_stats(device, &pipeline->base, pipeline->base.shaders[MESA_SHADER_COMPUTE], MESA_SHADER_COMPUTE,
                             stderr);
   }
   ralloc_free(cs_stage.nir);

done:
   pipeline_feedback.duration = os_time_get_nano() - pipeline_start;

   if (creation_feedback) {
      *creation_feedback->pPipelineCreationFeedback = pipeline_feedback;

      if (creation_feedback->pipelineStageCreationFeedbackCount) {
         assert(creation_feedback->pipelineStageCreationFeedbackCount == 1);
         creation_feedback->pPipelineStageCreationFeedbacks[0] = cs_stage.feedback;
      }
   }

   return result;
}

static VkResult
radv_compute_pipeline_import_binary(struct radv_device *device, struct radv_compute_pipeline *pipeline,
                                    const VkPipelineBinaryInfoKHR *binary_info)
{
   VK_FROM_HANDLE(radv_pipeline_binary, pipeline_binary, binary_info->pPipelineBinaries[0]);
   struct radv_shader *shader;
   struct blob_reader blob;

   assert(binary_info->binaryCount == 1);

   blob_reader_init(&blob, pipeline_binary->data, pipeline_binary->size);

   shader = radv_shader_deserialize(device, pipeline_binary->key, sizeof(pipeline_binary->key), &blob);
   if (!shader)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   pipeline->base.shaders[MESA_SHADER_COMPUTE] = shader;

   pipeline->base.pipeline_hash = *(uint64_t *)pipeline_binary->key;

   return VK_SUCCESS;
}

VkResult
radv_compute_pipeline_create(VkDevice _device, VkPipelineCache _cache, const VkComputePipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator, VkPipeline *pPipeline)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, _cache);
   VK_FROM_HANDLE(radv_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   struct radv_compute_pipeline *pipeline;
   VkResult result;

   pipeline = vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pipeline == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   radv_pipeline_init(device, &pipeline->base, RADV_PIPELINE_COMPUTE);
   pipeline->base.create_flags = vk_compute_pipeline_create_flags(pCreateInfo);
   pipeline->base.is_internal = _cache == device->meta_state.cache;

   const VkPipelineCreationFeedbackCreateInfo *creation_feedback =
      vk_find_struct_const(pCreateInfo->pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);

   const VkPipelineBinaryInfoKHR *binary_info = vk_find_struct_const(pCreateInfo->pNext, PIPELINE_BINARY_INFO_KHR);

   if (binary_info && binary_info->binaryCount > 0) {
      result = radv_compute_pipeline_import_binary(device, pipeline, binary_info);
   } else {
      result = radv_compute_pipeline_compile(pCreateInfo, pipeline, pipeline_layout, device, cache, &pCreateInfo->stage,
                                             creation_feedback);
   }

   if (result != VK_SUCCESS) {
      radv_pipeline_destroy(device, &pipeline->base, pAllocator);
      return result;
   }

   radv_compute_pipeline_init(pipeline, pipeline_layout, pipeline->base.shaders[MESA_SHADER_COMPUTE]);

   *pPipeline = radv_pipeline_to_handle(&pipeline->base);
   radv_rmv_log_compute_pipeline_create(device, &pipeline->base, pipeline->base.is_internal);
   return VK_SUCCESS;
}

static VkResult
radv_create_compute_pipelines(VkDevice _device, VkPipelineCache pipelineCache, uint32_t count,
                              const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines)
{
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < count; i++) {
      VkResult r;
      r = radv_compute_pipeline_create(_device, pipelineCache, &pCreateInfos[i], pAllocator, &pPipelines[i]);
      if (r != VK_SUCCESS) {
         result = r;
         pPipelines[i] = VK_NULL_HANDLE;

         VkPipelineCreateFlagBits2KHR create_flags = vk_compute_pipeline_create_flags(&pCreateInfos[i]);
         if (create_flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
            break;
      }
   }

   for (; i < count; ++i)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

void
radv_destroy_compute_pipeline(struct radv_device *device, struct radv_compute_pipeline *pipeline)
{
   if (pipeline->base.shaders[MESA_SHADER_COMPUTE])
      radv_shader_unref(device, pipeline->base.shaders[MESA_SHADER_COMPUTE]);
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CreateComputePipelines(VkDevice _device, VkPipelineCache pipelineCache, uint32_t count,
                            const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   return radv_create_compute_pipelines(_device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines);
}
