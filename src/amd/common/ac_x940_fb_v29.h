/* SPDX-License-Identifier: MIT
 * Compatibility names for the V29 framebuffer bring-up package.
 * New code selects the named MGFX2 layout from ac_mgfx2_regs.h.
 */
#ifndef AC_X940_FB_V29_H
#define AC_X940_FB_V29_H

#include "ac_mgfx2_regs.h"

#define AC_X940_FB_CB_COLOR0_BASE AC_MGFX2_CB_COLOR0_BASE
#define AC_X940_FB_CB_COLOR0_CLEAR_WORD0 AC_MGFX2_CB_COLOR0_CLEAR_WORD0
#define AC_X940_FB_DB_HTILE_DATA_BASE AC_MGFX2_DB_HTILE_DATA_BASE
#define AC_X940_FB_DB_DEPTH_SIZE_XY AC_MGFX2_DB_DEPTH_SIZE_XY
#define AC_X940_FB_DB_DEPTH_VIEW AC_MGFX2_DB_DEPTH_VIEW
#define AC_X940_FB_DB_HTILE_SURFACE AC_MGFX2_DB_HTILE_SURFACE
#define AC_X940_FB_DB_STENCIL_WRITE_BASE_HI AC_MGFX2_DB_STENCIL_WRITE_BASE_HI
#define AC_X940_FB_DB_HTILE_DATA_BASE_HI AC_MGFX2_DB_HTILE_DATA_BASE_HI
#define AC_X940_FB_DB_Z_READ_BASE_HI AC_MGFX2_DB_Z_READ_BASE_HI
#define AC_X940_FB_DB_Z_WRITE_BASE_HI AC_MGFX2_DB_Z_WRITE_BASE_HI
#define AC_X940_FB_DB_STENCIL_READ_BASE_HI AC_MGFX2_DB_STENCIL_READ_BASE_HI

static inline unsigned
ac_x940_cb_clear_word0_v29(unsigned index)
{
   return ac_mgfx2_cb_clear_word0(index);
}

static inline uint32_t
ac_x940_cb_attrib_v29(uint32_t value)
{
   return ac_mgfx2_cb_attrib(value);
}

static inline uint32_t
ac_x940_cb_attrib3_v29(uint32_t value)
{
   return ac_mgfx2_cb_attrib3(value);
}

#endif
