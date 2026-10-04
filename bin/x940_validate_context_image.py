#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host checks of the V39 context image, real uploader and preamble BO retention.

Compiles the actual C functions with allocation/map/CS dependency mocks. No GPU
access. The independent fixture is the V38 initialized stock context snapshot.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from x940_validate_emitters import function, generate_amd_header


MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ac_mgfx2_context.h"
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_FEATURE_NOT_PRESENT (-8)
#define VK_ERROR_MEMORY_MAP_FAILED (-5)
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define RADEON_DOMAIN_GTT 4
#define RADEON_FLAG_CPU_ACCESS 1
#define RADEON_FLAG_NO_INTERPROCESS_SHARING 2
#define RADEON_FLAG_READ_ONLY 4
#define RADEON_FLAG_GTT_WC 8
#define RADV_BO_PRIORITY_CS 3
struct radeon_winsys_bo { uint64_t va; };
struct radeon_cmdbuf {unsigned added, count; uint32_t words[64];};
struct radeon_winsys {
   void (*buffer_unmap)(struct radeon_winsys *, struct radeon_winsys_bo *, bool);
   void (*cs_execute_ib)(struct radeon_cmdbuf *, struct radeon_winsys_bo *, unsigned, unsigned, bool);
};
struct radv_physical_device {struct {bool is_xclipse940;} info;};
struct radv_device {
   struct radv_physical_device *pdev;
   struct radeon_winsys *ws;
   bool uses_shadow_regs;
   struct radeon_winsys_bo *x940_context_image, *gfx_init;
   unsigned gfx_init_size_dw;
};
static struct radeon_winsys_bo image_bo = {.va = 0x123456789000ull}, cached_bo;
static uint32_t mapped[AC_MGFX2_CONTEXT_IMAGE_DWORDS];
static bool allocation_failure, map_failure;
static unsigned creates, destroys, unmaps, cached_calls, inline_calls;
static struct radv_physical_device *radv_device_physical(struct radv_device *d) {return d->pdev;}
static bool debug_get_bool_option(const char *name, bool default_value) {
   const char *s = getenv(name); return s ? !strcmp(s, "1") : default_value;
}
static uint64_t radv_buffer_get_va(struct radeon_winsys_bo *b) {return b->va;}
static VkResult radv_bo_create(struct radv_device *d, void *obj, unsigned size, unsigned align,
   unsigned domain, unsigned flags, unsigned priority, unsigned replay, bool internal,
   struct radeon_winsys_bo **bo) {
   (void)d; assert(!obj && size == 4096 && align == 4096 && domain == RADEON_DOMAIN_GTT);
   assert(flags == 15 && priority == RADV_BO_PRIORITY_CS && !replay && internal);
   creates++;
   if (allocation_failure) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   *bo = &image_bo; return VK_SUCCESS;
}
static void radv_bo_destroy(struct radv_device *d, void *obj, struct radeon_winsys_bo *b) {
   (void)d; assert(!obj && b == &image_bo); destroys++;
}
static void *radv_buffer_map(struct radeon_winsys *w, struct radeon_winsys_bo *b) {
   (void)w; assert(b == &image_bo); return map_failure ? NULL : mapped;
}
static void unmap(struct radeon_winsys *w, struct radeon_winsys_bo *b, bool persist) {
   (void)w; assert(b == &image_bo && !persist); unmaps++;
}
static void radv_cs_add_buffer(struct radeon_winsys *w, struct radeon_cmdbuf *c, struct radeon_winsys_bo *b) {
   (void)w; assert(b == &image_bo || b == &cached_bo);
   c->added |= b == &image_bo ? 1 : 2;
}
static void execute(struct radeon_cmdbuf *c, struct radeon_winsys_bo *b, unsigned offset, unsigned n, bool chain) {
   (void)c; assert(b == &cached_bo && !offset && n == 128 && !chain); cached_calls++;
}
static VkResult radv_emit_graphics(struct radv_device *d, struct radeon_cmdbuf *c) {
   (void)d; (void)c; inline_calls++; return -99;
}
static void radeon_emit(struct radeon_cmdbuf *cs, uint32_t word) {assert(cs->count < 64); cs->words[cs->count++] = word;}
static void dump_word(void *unused, uint32_t word) {(void)unused; printf("%08x\n", word);}
'''

MAIN = r'''
int main(void) {
   unsetenv("RADV_X940_DIAG_DUMP_GFX_PREAMBLE");
   unsetenv("RADV_X940_DIAG_CONTEXT_IMAGE_V39");
   struct radv_physical_device pdev = {.info.is_xclipse940 = true};
   struct radeon_winsys ws = {.buffer_unmap = unmap, .cs_execute_ib = execute};
   struct radv_device d = {.pdev = &pdev, .ws = &ws};
   assert(radv_device_init_mgfx2_context_image(&d) == VK_SUCCESS && !creates);
   setenv("RADV_X940_DIAG_CONTEXT_IMAGE_V39", "0", 1);
   assert(radv_device_init_mgfx2_context_image(&d) == VK_SUCCESS && !creates);
   setenv("RADV_X940_DIAG_CONTEXT_IMAGE_V39", "1", 1);
   pdev.info.is_xclipse940 = false;
   assert(radv_device_init_mgfx2_context_image(&d) == VK_SUCCESS && !creates);
   pdev.info.is_xclipse940 = true; d.uses_shadow_regs = true;
   assert(radv_device_init_mgfx2_context_image(&d) == VK_ERROR_FEATURE_NOT_PRESENT && !creates);
   d.uses_shadow_regs = false; allocation_failure = true;
   assert(radv_device_init_mgfx2_context_image(&d) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(!d.x940_context_image && !destroys && !unmaps);
   allocation_failure = false; map_failure = true;
   assert(radv_device_init_mgfx2_context_image(&d) == VK_ERROR_MEMORY_MAP_FAILED);
   assert(!d.x940_context_image && destroys == 1 && !unmaps);
   map_failure = false; memset(mapped, 0xff, sizeof(mapped));
   assert(radv_device_init_mgfx2_context_image(&d) == VK_SUCCESS);
   assert(d.x940_context_image == &image_bo && unmaps == 1);
   assert(!memcmp(mapped, ac_mgfx2_context_image, sizeof(mapped)));
   for (unsigned i = 0; i < 3; i++) {
      struct radeon_cmdbuf cs = {0};
      if (i < 2) radv_emit_mgfx2_context_image(&d, &cs);
      assert(cs.added == (i < 2 ? 1u : 0u) && cs.count == (i < 2 ? 42u : 0u));
      d.gfx_init = &cached_bo; d.gfx_init_size_dw = 128;
      assert(radv_init_graphics_state(&cs, &d) == VK_SUCCESS);
      assert(cs.added == (i < 2 ? 3u : 2u));
   }
   struct radeon_cmdbuf cs = {0}; d.gfx_init = NULL;
   radv_emit_mgfx2_context_image(&d, &cs);
   assert(radv_init_graphics_state(&cs, &d) == -99 && cs.added == 1);
   assert(cached_calls == 3 && inline_calls == 1);
   puts("IMAGE");
   for (unsigned i = 0; i < ARRAY_SIZE(mapped); i++) printf("%08x\n", mapped[i]);
   puts("PACKETS"); ac_mgfx2_emit_context_image(image_bo.va, dump_word, NULL);
   radv_device_finish_mgfx2_context_image(&d);
   assert(!d.x940_context_image && destroys == 2);
   radv_device_finish_mgfx2_context_image(&d); assert(destroys == 2);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--stock-folder', type=Path, help='Optional extracted V38 round_1 folder')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    oracle = json.loads((root / 'bin/x940_context_v39_reference.json').read_text())
    device = (root / 'src/amd/vulkan/radv_device.c').read_text()
    queue = (root / 'src/amd/vulkan/radv_queue.c').read_text()
    test = MOCKS + '\n' + function(device, 'radv_device_finish_mgfx2_context_image')
    test += '\n' + function(device, 'radv_device_init_mgfx2_context_image')
    test += '\n' + function(queue, 'radv_emit_mgfx2_context_image')
    test += '\n' + function(queue, 'radv_init_graphics_state') + '\n' + MAIN
    assert 'if (i < 2)\n            radv_emit_mgfx2_context_image(device, cs);' in function(queue, 'radv_update_preamble_cs')
    assert 'radv_emit_mgfx2_context_image' not in function(queue, 'radv_emit_graphics')
    # Both device failure cleanup and normal destruction must release the BO.
    assert device.count('radv_device_finish_mgfx2_context_image(device);') == 3
    assert device.index('radv_device_init_mgfx2_context_image(device);') < device.index('/* Create one context per queue priority. */')
    assert device.index('radv_device_finish_mgfx2_context_image(device);', device.index('fail_queue:')) > device.index('fail_queue:')
    with tempfile.TemporaryDirectory(prefix='x940-context-host-') as folder:
        tmp = Path(folder)
        (tmp / 'amdgfxregs.h').write_text(generate_amd_header(root))
        (tmp / 'test.c').write_text(test)
        subprocess.run([args.cc, '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(tmp), '-I'+str(root/'src'), '-I'+str(root/'src/amd/common'),
                        str(tmp/'test.c'), '-o', str(tmp/'test')], check=True)
        result = subprocess.run([str(tmp/'test')], check=True, capture_output=True, text=True,
                                env=dict(os.environ, LC_ALL='C'))
    image_text, packet_text = result.stdout.split('PACKETS\n')
    image = [int(x, 16) for x in image_text.split('IMAGE\n')[1].splitlines()]
    expected = [0] * 1024
    for addr, entry in oracle['nonzero_defaults'].items():
        expected[(int(addr, 16) - 0x28000) // 4] = int(entry['value'], 16)
    assert image == expected and len([x for x in image if x]) == 53
    ranges = oracle['ranges_dw']
    covered = set()
    for first, count in ranges:
        assert first >= 0 and count > 0 and first + count <= len(image)
        span = set(range(first, first + count))
        assert not covered & span
        covered |= span
    assert len(covered) == 614
    assert all(not x or i in covered for i, x in enumerate(image))
    packets = [int(x, 16) for x in packet_text.splitlines()]
    # Partial flush VS/PS + VGT flush, per-context LOAD-only CC, then exact ranges.
    assert packets[:12] == [0xc0004600, 0x40f, 0xc0004600, 0x410,
                           0xc0004600, 0x24, 0xc0012800, 0x80000002, 0x80000000,
                           0xc01f6100, 0x56789000, 0x1234]
    assert packets[12:] == [x for pair in ranges for x in pair]
    if args.stock_folder:
        snapshot = (args.stock_folder / 'tables/context_000002.bin').read_bytes()
        assert len(snapshot) == 32768
        assert snapshot[:4096] == struct.pack('<1024I', *image) and not any(snapshot[4096:])
        capture = json.loads((args.stock_folder / 'context_capture.json').read_text())
        loads = [l for p in capture['pal'] for l in p['loads'] if l['opcode'] == '0x61']
        assert len(loads) == 1 and loads[0]['body'][2:] == packets[12:]
    print('V38 context image: 15 ranges, 614 positions, 53 nonzero defaults, holes excluded: OK')
    print('PM4: LOAD-only context control, exact range/count/VA, graphics waits: OK')
    print('Real uploader: opt-in/AMD/shadow guards, allocation/map errors, upload/unmap/cleanup: OK')
    print('Queue initials retain image BO; continue has no LOAD; cached/inline graphics init preserved: OK')
    print('Host validation only. Android build and GPU execution remain pending.')


if __name__ == '__main__':
    main()
