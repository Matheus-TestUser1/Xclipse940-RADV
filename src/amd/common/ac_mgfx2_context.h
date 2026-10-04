/* SPDX-License-Identifier: MIT
 * Diagnostic MGFX2 context image: V38 stock control, Samsung HAL SHA256
 * e3eacfdf52904bbb81d1dbbbd343bc352ee6d1c62d8eae71618615908d25cd3a.
 * Ranges came from LOAD_CONTEXT_REG; values from the verified pre-submit
 * shadow BO after stock initialization, before the application draw.
 * This is a captured default profile, not an architectural reset guarantee.
 * No GPU virtual addresses, UCONFIG globals or shader registers are copied.
 */
#ifndef AC_MGFX2_CONTEXT_H
#define AC_MGFX2_CONTEXT_H

#include "ac_mgfx2_regs.h"
#include "sid.h"
#include "util/macros.h"

#define AC_MGFX2_CONTEXT_IMAGE_DWORDS 1024
#define AC_MGFX2_CONTEXT_IMAGE_BYTES (AC_MGFX2_CONTEXT_IMAGE_DWORDS * 4)
#define AC_MGFX2_CONTEXT_REGISTER_COUNT 614

struct ac_mgfx2_context_range {
   uint32_t offset_dw; /* Relative to SI_CONTEXT_REG_OFFSET, not a packed table index. */
   uint32_t count;
};

static const struct ac_mgfx2_context_range ac_mgfx2_context_ranges[] = {
   {0x000, 36}, /* 0x028000..0x02808c */
   {0x07a, 94}, /* 0x0281e8..0x02835c */
   {0x0dd, 7}, /* 0x028374..0x02838c */
   {0x0fa, 2}, /* 0x0283e8..0x0283ec */
   {0x103, 194}, /* 0x02840c..0x028710 */
   {0x1d4, 20}, /* 0x028750..0x02879c */
   {0x1f5, 4}, /* 0x0287d4..0x0287e0 */
   {0x1ff, 20}, /* 0x0287fc..0x028848 */
   {0x280, 4}, /* 0x028a00..0x028a0c */
   {0x286, 2}, /* 0x028a18..0x028a1c */
   {0x293, 2}, /* 0x028a4c..0x028a50 */
   {0x2a3, 1}, /* 0x028a8c..0x028a8c */
   {0x2a5, 63}, /* 0x028a94..0x028b8c */
   {0x2f7, 133}, /* 0x028bdc..0x028dec */
   {0x390, 32}, /* 0x028e40..0x028ebc */
};

#define AC_MGFX2_CONTEXT_LOAD_DWORDS (12 + 2 * ARRAY_SIZE(ac_mgfx2_context_ranges))

/* Zeroes are intentional, but only the ranges above are loaded. Aperture
 * holes outside these ranges are never written by LOAD_CONTEXT_REG.
 */
static const uint32_t ac_mgfx2_context_image[AC_MGFX2_CONTEXT_IMAGE_DWORDS] = {
   [(AC_MGFX2_PA_SC_SCREEN_SCISSOR_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_WINDOW_SCISSOR_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_WINDOW_SCISSOR_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_CLIPRECT_RULE - SI_CONTEXT_REG_OFFSET) / 4] = 0x0000ffffu,
   [(AC_MGFX2_PA_SC_CLIPRECT_0_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_CLIPRECT_1_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_CLIPRECT_2_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_CLIPRECT_3_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_EDGERULE - SI_CONTEXT_REG_OFFSET) / 4] = 0xaa99aaaau,
   [(AC_MGFX2_PA_SC_GENERIC_SCISSOR_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_GENERIC_SCISSOR_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_0_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_0_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_1_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_1_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_2_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_2_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_3_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_3_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_4_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_4_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_5_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_5_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_6_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_6_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_7_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_7_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_8_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_8_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_9_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_9_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_10_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_10_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_11_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_11_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_12_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_12_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_13_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_13_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_14_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_14_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_15_TL - SI_CONTEXT_REG_OFFSET) / 4] = 0x80000000u,
   [(AC_MGFX2_PA_SC_VPORT_SCISSOR_15_BR - SI_CONTEXT_REG_OFFSET) / 4] = 0x40004000u,
   [(AC_MGFX2_CVC_CNTL - SI_CONTEXT_REG_OFFSET) / 4] = 0x83000000u,
   [(AC_MGFX2_CB_RMI_GL2_CACHE_CONTROL - SI_CONTEXT_REG_OFFSET) / 4] = 0x00550055u,
   [(AC_MGFX2_VGT_TESS_DISTRIBUTION - SI_CONTEXT_REG_OFFSET) / 4] = 0xd8181e0cu,
   [(AC_MGFX2_PA_SC_LINE_CNTL - SI_CONTEXT_REG_OFFSET) / 4] = 0x00001000u,
   [(AC_MGFX2_PA_SU_VTX_CNTL - SI_CONTEXT_REG_OFFSET) / 4] = 0x00000005u,
   [(AC_MGFX2_PA_SC_AA_MASK_X0Y0_X1Y0 - SI_CONTEXT_REG_OFFSET) / 4] = 0xffffffffu,
   [(AC_MGFX2_PA_SC_AA_MASK_X0Y1_X1Y1 - SI_CONTEXT_REG_OFFSET) / 4] = 0xffffffffu,
   [(AC_MGFX2_PA_SC_BINNER_CNTL_0 - SI_CONTEXT_REG_OFFSET) / 4] = 0x19fc0c92u,
   [(AC_MGFX2_PA_SC_BINNER_CNTL_2 - SI_CONTEXT_REG_OFFSET) / 4] = 0x03e00800u,
   [(AC_MGFX2_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL - SI_CONTEXT_REG_OFFSET) / 4] = 0x00100000u,
};

static inline void
ac_mgfx2_emit_context_image(uint64_t va, void (*emit)(void *, uint32_t), void *cs)
{
   assert(!(va & 4095));
   /* A continue preamble can follow unfinished graphics work. Complete that
    * work before replacing the per-context image. No compute SH state changes.
    */
   emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
   emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
   emit(cs, EVENT_TYPE(V_028A90_PS_PARTIAL_FLUSH) | EVENT_INDEX(4));
   emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
   emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));

   /* Enable only per-context LOAD. Shadow writes and all other LOAD groups
    * remain disabled. This is initialization, not context save/restore.
    */
   emit(cs, PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
   emit(cs, CC0_UPDATE_LOAD_ENABLES(1) | CC0_LOAD_PER_CONTEXT_STATE(1));
   emit(cs, CC1_UPDATE_SHADOW_ENABLES(1));
   emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG, 1 + 2 * ARRAY_SIZE(ac_mgfx2_context_ranges), 0));
   emit(cs, va);
   emit(cs, va >> 32);
   for (unsigned i = 0; i < ARRAY_SIZE(ac_mgfx2_context_ranges); i++) {
      emit(cs, ac_mgfx2_context_ranges[i].offset_dw);
      emit(cs, ac_mgfx2_context_ranges[i].count);
   }
}

#endif /* AC_MGFX2_CONTEXT_H */
