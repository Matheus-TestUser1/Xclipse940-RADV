#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate actual MGFX2 framebuffer emission against caller-supplied headers.

Compile the actual CB/DB blocks and fast-clear functions with a PM4 recorder.
Check eight MRTs, moved fields, address extensions, depth/stencil and AMD
fast-clear paths. No Android build or GPU execution is performed.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from x940_regmap_audit import kernel_map, numeric_defines
from x940_validate_emitters import generate_amd_header


def braced(text, start):
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[opening + 1:i], i + 1
    raise AssertionError('Unclosed C block')


def function(text, name):
    start = text.index('\n' + name + '(')
    _, end = braced(text, start)
    return 'static void' + text[start:end]


def new_branch(text, name):
    func = function(text, name)
    return braced(func, func.index('if (pdev->info.is_xclipse940)'))[0]


def decode(words):
    writes = []
    i = 0
    while i < len(words):
        h = words[i]
        assert h >> 30 == 3 and (h >> 8) & 255 == 0x69, hex(h)
        size = ((h >> 16) & 0x3fff) + 2
        assert i + size <= len(words)
        reg = 0x28000 + words[i + 1] * 4
        writes.extend((reg + n * 4, v) for n, v in enumerate(words[i + 2:i + size]))
        i += size
    return writes


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ac_mgfx2_regs.h"
#include "amdgfxregs.h"
#define SI_CONTEXT_REG_OFFSET 0x28000u
#define PKT3(o,c,p) (0xc0000000u | ((unsigned)(c)<<16) | ((unsigned)(o)<<8) | ((unsigned)(p)&1))
#define PKT3_LOAD_CONTEXT_REG_INDEX 0x9f
#define PKT3_COPY_DATA 0x40
#define PKT3_PFP_SYNC_ME 0x42
#define COPY_DATA_SRC_SEL(x) ((x)&0xf)
#define COPY_DATA_DST_SEL(x) (((unsigned)(x)&0xf)<<8)
#define COPY_DATA_SRC_MEM 1
#define COPY_DATA_REG 0
#define COPY_DATA_COUNT_SEL (1<<16)
#define ASSERTED
struct radeon_cmdbuf { uint32_t words[512]; unsigned cdw; };
static void radeon_emit(struct radeon_cmdbuf *cs, uint32_t v) { assert(cs->cdw<512); cs->words[cs->cdw++]=v; }
static void radeon_set_context_reg_seq(struct radeon_cmdbuf *cs, unsigned reg, unsigned n) {
  assert(reg>=0x28000 && reg<0x30000 && !(reg&3));
  radeon_emit(cs,PKT3(0x69,n,0)); radeon_emit(cs,(reg-0x28000)/4);
}
static void radeon_set_context_reg(struct radeon_cmdbuf *cs, unsigned r, uint32_t v) {
  radeon_set_context_reg_seq(cs,r,1); radeon_emit(cs,v);
}
static unsigned radeon_check_space(void *ws, struct radeon_cmdbuf *cs, unsigned n) { (void)ws; return cs->cdw+n; }
struct radv_image { bool support_comp_to_single, cmask, dcc, clear; };
struct radv_image_view { struct radv_image *image; struct { unsigned base_mip_level; } vk; };
struct radv_physical_device { struct { bool is_xclipse940, has_load_ctx_reg_pkt; } info; };
struct radv_device { struct radv_physical_device *pdev; void *ws; };
struct radv_cmd_buffer {
  struct radv_device *device; struct radeon_cmdbuf *cs;
  struct { bool context_roll_without_scissor_emitted, predicating;
    struct { int color_att_count; struct { struct radv_image_view *iview; } color_att[8]; } render;
  } state;
};
static struct radv_device *radv_cmd_buffer_device(struct radv_cmd_buffer *cmd) { return cmd->device; }
static struct radv_physical_device *radv_device_physical(struct radv_device *device) { return device->pdev; }
static bool radv_image_has_cmask(struct radv_image *image) { return image->cmask; }
static bool radv_dcc_enabled(struct radv_image *image, unsigned level) { (void)level; return image->dcc; }
static bool radv_image_has_clear_value(struct radv_image *image) { return image->clear; }
static uint64_t radv_image_get_fast_clear_va(struct radv_image *image, unsigned level) { (void)image; (void)level; return UINT64_C(0x1234567800); }
struct radv_color_buffer_info { struct {
  uint64_t cb_color_base, cb_color_cmask, cb_color_fmask, cb_dcc_base;
  uint32_t cb_color_view, cb_color_attrib, cb_dcc_control, cb_color_attrib2, cb_color_attrib3;
} ac; };
struct radv_ds_buffer_info { uint32_t db_render_override2; struct {
  uint64_t db_depth_base, db_stencil_base;
  uint32_t db_depth_view, db_depth_size, db_stencil_info;
} ac; };
static void emit_color(struct radv_cmd_buffer *cmd_buffer, struct radv_color_buffer_info *cb, unsigned index, uint32_t cb_color_info) {
@COLOR@
}
static void emit_ds(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds, bool is940) {
 const struct radv_physical_device *pdev = radv_device_physical(cmd_buffer->device);
 uint64_t db_htile_data_base=UINT64_C(0x5601234567);
 uint32_t db_htile_surface=0x80102, db_render_control=0x400, db_z_info=0x81230003;
 if (!is940) return;
@DS_COMMON@
@DS@
}
@CLEAR_FUNCTION@
@LOAD_FUNCTION@
static void dump(struct radeon_cmdbuf *cs) {
 for(unsigned i=0;i<cs->cdw;i++) printf("%08x%s",cs->words[i],i+1==cs->cdw?"\n":" ");
 if(!cs->cdw) puts("");
}
int main(int argc,char **argv) {
 assert(argc==5);
 unsigned mode=strtoul(argv[1],NULL,0), index=strtoul(argv[2],NULL,0), val=strtoul(argv[3],NULL,0);
 bool is940=strtoul(argv[4],NULL,0);
 assert(index<8);
 struct radeon_cmdbuf cs={0};
 struct radv_physical_device pdev={{is940,true}};
 struct radv_device device={&pdev,NULL};
 struct radv_cmd_buffer cmd={.device=&device,.cs=&cs};
 struct radv_image image={.cmask=true,.clear=true};
 struct radv_image_view view={.image=&image};
 cmd.state.render.color_att_count=8; cmd.state.render.color_att[index].iview=&view;
 if (mode==0) {
   struct radv_color_buffer_info cb={.ac={
    .cb_color_base=UINT64_C(0x1201234567)+index, .cb_color_cmask=UINT64_C(0x3402345678)+index,
    .cb_color_fmask=UINT64_C(0x5603456789)+index, .cb_dcc_base=UINT64_C(0x780456789a)+index,
    .cb_color_view=0x10000123+index, .cb_color_attrib=val, .cb_dcc_control=0x104218,
    .cb_color_attrib2=0x22220111+index, .cb_color_attrib3=0xddaa0123+index
   }};
   emit_color(&cmd,&cb,index,0x18060028+index);
 } else if(mode==1) {
   struct radv_ds_buffer_info ds={.db_render_override2=0x11220000,.ac={.db_depth_base=UINT64_C(0x120abcde10), .db_stencil_base=UINT64_C(0x340bcdef20),
     .db_depth_view=0x10203040,.db_depth_size=0x00190027,.db_stencil_info=0x28100001}};
   emit_ds(&cmd,&ds,is940);
 } else if(mode==2) {
   uint32_t vals[2]={0x11223344,0x55667788};
   if(val==1) cmd.state.render.color_att[index].iview=NULL;
   radv_update_bound_fast_clear_color(&cmd,&image,index,vals);
 } else if(mode==3) {
   pdev.info.has_load_ctx_reg_pkt=val!=1;
   if(val==2) image.clear=false;
   if(val==3) image.cmask=false;
   if(val==4) image.support_comp_to_single=true;
   radv_load_color_clear_metadata(&cmd,&view,index);
 } else if(mode==4) {
   printf("%08x\n",ac_mgfx2_cb_attrib3(val)); return 0;
 } else abort();
 dump(&cs); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--offset-header', type=Path, required=True)
    parser.add_argument('--ip-header', type=Path, required=True)
    parser.add_argument('--mask-header', type=Path, required=True)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    args = parser.parse_args()
    project = args.root.resolve()
    text = (project / 'src/amd/vulkan/radv_cmd_buffer.c').read_text()
    regs = kernel_map(args.offset_header, args.ip_header)
    masks = {}
    for name, value in numeric_defines(args.mask_header).items():
        if name.endswith('_MASK') and '__' in name:
            reg, field = name[:-5].split('__', 1)
            masks.setdefault(reg, {})[field] = value
    amd = json.loads((project / 'src/amd/registers/gfx103.json').read_text())
    refs = {'amd_fields': {r['name']: amd['register_types'][r['type_ref']]['fields']
                          for r in amd['register_mappings'] if 'type_ref' in r}}
    source = HARNESS.replace('@COLOR@', new_branch(text, 'radv_emit_fb_color_state'))
    source = source.replace('@DS@', new_branch(text, 'radv_emit_fb_ds_state'))
    dsfunc = function(text, 'radv_emit_fb_ds_state')
    dscommon, _ = braced(dsfunc, dsfunc.index('if (pdev->info.gfx_level < GFX12)'))
    override = re.search(r'^   radeon_set_context_reg\(cmd_buffer->cs, ac_mgfx2_reg\(pdev->info.is_xclipse940, R_028010_DB_RENDER_OVERRIDE2.*;', dsfunc, re.M)[0]
    # Include the common control/view/HTILE/override writes verbatim.
    source = source.replace('@DS_COMMON@', dscommon + '\n' + override)
    source = source.replace('@CLEAR_FUNCTION@', function(text, 'radv_update_bound_fast_clear_color'))
    source = source.replace('@LOAD_FUNCTION@', function(text, 'radv_load_color_clear_metadata'))
    # Field correspondence of retained values, against independent M2 masks.
    for reg in ('CB_COLOR0_VIEW', 'CB_COLOR0_ATTRIB2', 'DB_DEPTH_VIEW', 'DB_HTILE_SURFACE',
                'DB_DEPTH_SIZE_XY', 'DB_Z_INFO', 'DB_STENCIL_INFO'):
        for field in refs['amd_fields'][reg]:
            lo, hi = field['bits']
            assert masks[reg][field['name']] == ((1 << (hi-lo+1))-1) << lo, (reg,field)
    print('Retained VIEW/ATTRIB2/DB fields match MGFX2 masks: OK')
    with tempfile.TemporaryDirectory(prefix='x940-fb-test-') as tmp:
        tmp = Path(tmp)
        (tmp / 'amdgfxregs.h').write_text(generate_amd_header(project))
        (tmp / 'emission.c').write_text(source)
        exe = tmp / 'emission'
        subprocess.run(shlex.split(args.cc) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(project / 'src/amd/common'), '-I', str(tmp), str(tmp/'emission.c'), '-o', str(exe)], check=True)

        def run(mode, index=0, value=0, is940=True):
            result = subprocess.run([str(exe), str(mode), str(index), hex(value), str(int(is940))],
                                    capture_output=True, text=True, check=True)
            return [int(x,16) for x in result.stdout.split()]

        # Exercise all 256 combinations of the five moved fields at each MRT.
        field_masks = masks['CB_COLOR0_ATTRIB']
        fields = {f['name']: f['bits'] for f in refs['amd_fields']['CB_COLOR0_ATTRIB'] if f['name'] in field_masks}
        count = 0
        for index in range(8):
            expected_addr = [regs[f'CB_COLOR{index}_{name}'] for name in
                ('BASE','VIEW','ATTRIB','DCC_CONTROL','CMASK','FMASK','DCC_BASE','ATTRIB2','ATTRIB3',
                 'INFO','BASE_EXT','CMASK_BASE_EXT','FMASK_BASE_EXT','DCC_BASE_EXT')]
            for low in range(256):
                packed = 0
                for name, (lo, hi) in fields.items():
                    m = field_masks[name]
                    shift = (m & -m).bit_length()-1
                    packed |= ((low & m) >> shift) << lo
                # Legacy TILE/FMASK index bits must not leak into MGFX2 fields.
                words = run(0,index,packed | 0xfff)
                assert len(words)==26
                writes = decode(words)
                assert [r for r,v in writes] == expected_addr
                attrib3 = (0xddaa0123+index) & sum(masks['CB_COLOR0_ATTRIB3'].values())
                values = [(0x1201234567+index)&0xffffffff,0x10000123+index,low,0x104218,
                          (0x3402345678+index)&0xffffffff,(0x5603456789+index)&0xffffffff,
                          (0x780456789a+index)&0xffffffff,0x22220111+index,attrib3,
                          0x18060028+index,0x12,0x34,0x56,0x78]
                assert [v for r,v in writes] == values, (index,low,writes)
                assert len(set(r for r,v in writes))==14
                count += 1
        print(f'CB: {count} packets, eight MRTs, addresses/values/fields/EXT and size: OK')

        for bit in range(32):
            got = run(4,value=1<<bit)[0]
            assert got == (1<<bit) & sum(masks['CB_COLOR0_ATTRIB3'].values())
        print('ATTRIB3: every bit checked; AMD RESOURCE_LEVEL removed: OK')

        ds_words = run(1)
        ds_writes = decode(ds_words)
        expected_ds = {
          'DB_RENDER_CONTROL':0x400,'DB_DEPTH_VIEW':0x10203040,'DB_HTILE_SURFACE':0x80102,
          'DB_RENDER_OVERRIDE2':0x11220000,'DB_HTILE_DATA_BASE':0x01234567,'DB_DEPTH_SIZE_XY':0x00190027,
          'DB_Z_INFO':0x81230003,'DB_STENCIL_INFO':0x28100001,
          'DB_Z_READ_BASE':0x0abcde10,'DB_Z_WRITE_BASE':0x0abcde10,
          'DB_STENCIL_READ_BASE':0x0bcdef20,'DB_STENCIL_WRITE_BASE':0x0bcdef20,
          'DB_Z_READ_BASE_HI':0x12,'DB_Z_WRITE_BASE_HI':0x12,
          'DB_STENCIL_READ_BASE_HI':0x34,'DB_STENCIL_WRITE_BASE_HI':0x34,'DB_HTILE_DATA_BASE_HI':0x56,
        }
        assert len(ds_words)==41 and len(ds_writes)==17
        assert dict(ds_writes)=={regs[k]:v for k,v in expected_ds.items()}
        assert sum(r==regs['DB_DEPTH_SIZE_XY'] for r,v in ds_writes)==1
        print('DB: 17 values and separate high addresses; DEPTH_SIZE preserved; 41 dwords: OK')

        for is940 in (False,True):
            for index in range(8):
                reg=regs[f'CB_COLOR{index}_CLEAR_WORD0'] if is940 else 0x28c8c+index*0x3c
                clear=run(2,index,is940=is940)
                assert decode(clear)==[(reg,0x11223344),(reg+4,0x55667788)]
                assert run(2,index,1,is940)==[]
                load=run(3,index,0,is940)
                assert len(load)==5 and ((load[0]>>8)&255)==0x9f
                assert load[1:]==[0x34567800,0x12,(reg-0x28000)//4,2]
                copy=run(3,index,1,is940)
                assert len(copy)==8 and ((copy[0]>>8)&255)==0x40
                assert copy[2:6]==[0x34567800,0x12,reg//4,0]
                assert decode(run(3,index,2,is940))==[(reg,0),(reg+4,0)]
                assert run(3,index,3,is940)==[]
                assert run(3,index,4,is940)==[]
        print('Fast clear: write/LOAD/COPY, eight MRTs, guards and AMD paths preserved: OK')
    print('Framebuffer host validation complete. Android build and GPU execution remain required.')


if __name__=='__main__':
    main()
