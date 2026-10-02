/* SPDX-License-Identifier: MIT
 * Physical MGFX2 addresses, checked against Samsung gc_10_4_0_offset_m2.h.
 * Use only at a named AMD register call site. Already-physical literals are
 * deliberately not translated: several addresses alias other AMD registers.
 */
#ifndef AC_X940_REG_V25_H
#define AC_X940_REG_V25_H
#include <stdbool.h>
static inline unsigned
ac_x940_reg_v25(bool is_xclipse940, unsigned amd_reg, unsigned mgfx2_reg)
{
   return is_xclipse940 ? mgfx2_reg : amd_reg;
}
#define AC_X940_CB_COLOR0_INFO 0x028d80u
#define AC_X940_CB_SHADER_MASK 0x028decu
#define AC_X940_DB_EQAA 0x02806cu
#define AC_X940_DB_RENDER_CONTROL 0x028064u
#define AC_X940_DB_RENDER_OVERRIDE2 0x02805cu
#define AC_X940_DB_RMI_L2_CACHE_CONTROL 0x028068u
#define AC_X940_DB_VRS_OVERRIDE_CNTL 0x028000u
#define AC_X940_GE_USER_VGPR_EN 0x030984u
#define AC_X940_PA_SC_CENTROID_PRIORITY_0 0x028bf0u
#define AC_X940_PA_SC_MODE_CNTL_0 0x028c40u
#define AC_X940_SPI_BARYC_CNTL 0x028658u
#define AC_X940_SPI_INTERP_CONTROL_0 0x028644u
#define AC_X940_SPI_SHADER_COL_FORMAT 0x028654u
#define AC_X940_SPI_SHADER_PGM_HI_LS 0x00b41cu
#define AC_X940_SPI_SHADER_PGM_RSRC3_HS 0x00b428u
#define AC_X940_SPI_SHADER_PGM_RSRC3_PS 0x00b000u
#define AC_X940_SPI_SHADER_PGM_RSRC4_HS 0x00b42cu
#define AC_X940_VGT_REUSE_OFF 0x028a9cu
#endif
