#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fault-inject actual native-submit and sparse-bind functions; no GPU work."""

import argparse
from pathlib import Path
import re
import resource
import subprocess
import tempfile

from x940_regmap_audit import strip_comments_and_strings


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drm-uapi/amdgpu_drm.h"
typedef int VkResult;
enum amd_ip_type {AMD_IP_GFX, AMD_IP_COMPUTE};
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_UNKNOWN (-13)
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
static unsigned allocation_calls, allocation_failure, live_allocations;
static void *tracked[32];
static void *audit_malloc(size_t size) {
   if (++allocation_calls == allocation_failure) return NULL;
   void *ptr = malloc(size); assert(ptr);
   for (unsigned i=0;i<32;i++) if (!tracked[i]) {
      tracked[i]=ptr; live_allocations++; return ptr;
   }
   abort();
}
static void audit_free(void *ptr) {
   if (!ptr) return;
   for (unsigned i=0;i<32;i++) if (tracked[i]==ptr) {
      tracked[i]=NULL; live_allocations--; free(ptr); return;
   }
   abort();
}
'''

SUBMIT_MOCKS = r'''
#define MAX_RINGS_PER_TYPE 1
struct radeon_winsys_bo {int unused;};
struct radv_amdgpu_winsys_bo {struct radeon_winsys_bo base; int bo;};
struct radv_amdgpu_winsys {
   int dev, fd;
   struct {
      bool is_sgpu, is_xclipse940, has_timeline_syncobj;
      struct {unsigned ib_alignment;} ip[AMDGPU_HW_IP_NUM];
   } info;
};
struct radv_amdgpu_ctx {
   struct radv_amdgpu_winsys *ws;
   int ctx;
   struct radeon_winsys_bo *fence_bo;
   bool queue_syncobj_wait[AMDGPU_HW_IP_NUM][MAX_RINGS_PER_TYPE];
};
struct amdgpu_cs_fence_info {int handle; uint64_t offset;};
struct amdgpu_cs_fence {int context; unsigned ip_type, ring; uint64_t fence;};
static unsigned raw_calls, sleeps, rejection_count, expected_waits;
static int raw_failure;
static uint64_t clock_ns;
static struct radv_amdgpu_ctx *active_ctx;
static struct radv_winsys_sem_info *active_sem;
static struct radv_amdgpu_winsys_bo *radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo) {return (void *)bo;}
static uint32_t radv_amdgpu_ctx_queue_syncobj(struct radv_amdgpu_ctx *ctx, unsigned ip, unsigned ring) {
   (void)ctx; (void)ip; (void)ring; return 41;
}
static void amdgpu_cs_chunk_fence_info_to_data(struct amdgpu_cs_fence_info *info,
   struct drm_amdgpu_cs_chunk_data *data) {
   data->fence_data.handle=info->handle; data->fence_data.offset=info->offset;
}
static uint64_t os_time_get_absolute_timeout(uint64_t timeout) {return clock_ns+timeout;}
static uint64_t os_time_get_nano(void) {clock_ns+=100000000; return clock_ns;}
static void os_time_sleep(unsigned delay) {assert(delay==1000); sleeps++;}
static uint64_t radv_x940_diag_v32_submit_begin(struct radv_amdgpu_ctx *ctx,
   struct radv_amdgpu_cs_request *request, int n, struct drm_amdgpu_cs_chunk *chunks) {
   (void)ctx; (void)request; (void)n; (void)chunks; return 1;
}
static void radv_x940_diag_v32_submit_result(struct radv_amdgpu_ctx *ctx,
   struct radv_amdgpu_cs_request *request, uint64_t id, int r) {
   (void)ctx; (void)request; (void)id; (void)r;
}
static int amdgpu_cs_submit_raw2(int dev, int ctx, int flags, int n,
   struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq) {
   (void)dev; (void)ctx; assert(!flags); raw_calls++;
   unsigned waits=0, signals=0, bos=0, ibs=0;
   for (int i=0;i<n;i++) {
      switch(chunks[i].chunk_id) {
      case AMDGPU_CHUNK_ID_IB: ibs++; break;
      case AMDGPU_CHUNK_ID_BO_HANDLES: bos++; break;
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN: {
         struct drm_amdgpu_cs_chunk_sem *s=(void *)(uintptr_t)chunks[i].chunk_data;
         assert(chunks[i].length_dw==2 && s[0].handle==70 && s[1].handle==41);
         waits++; break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
         struct drm_amdgpu_cs_chunk_syncobj *s=(void *)(uintptr_t)chunks[i].chunk_data;
         assert(chunks[i].length_dw==3*sizeof(*s)/4);
         assert(s[0].handle==70 && s[0].point==0 && s[0].flags==0);
         assert(s[1].handle==71 && s[1].point==5 && s[1].flags==DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT);
         assert(s[2].handle==41 && s[2].point==0 && s[2].flags==0);
         waits++; break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT:
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: signals++; break;
      }
   }
   assert(waits==expected_waits && signals==1 && bos==1 && ibs==1);
   if (expected_waits) {
      assert(active_sem->cs_emit_wait);
      assert(active_ctx->queue_syncobj_wait[AMDGPU_HW_IP_GFX][0]);
   }
   if (raw_calls<=rejection_count) return raw_failure;
   *seq=123; return 0;
}
static int amdgpu_cs_query_fence_status(struct amdgpu_cs_fence *f, uint64_t t, unsigned flags, uint32_t *expired) {
   (void)f; (void)t; (void)flags; *expired=1; return 0;
}
static int amdgpu_cs_query_reset_state2(int ctx, uint64_t *flags) {(void)ctx; *flags=0; return 0;}
static int amdgpu_cs_query_reset_state(int ctx, uint32_t *state, uint32_t *hangs) {
   (void)ctx; *state=*hangs=0; return 0;
}
static int drmSyncobjSignal(int fd, uint32_t *handles, unsigned n) {(void)fd; (void)handles; (void)n; abort();}
#define malloc audit_malloc
#define free audit_free
'''

SUBMIT_MAIN = r'''
int main(void) {
   const char *flags[]={"RADV_X940_DIAG_NO_USER_FENCE_V30", "RADV_X940_DIAG_SUBMIT_RAW",
      "RADV_X940_DIAG_RAW_FENCE", "RADV_X940_LEGACY_FENCE_TEST"};
   for (unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);i++) unsetenv(flags[i]);
   struct radv_amdgpu_winsys ws={0}; ws.info.ip[AMDGPU_HW_IP_GFX].ib_alignment=16;
   struct radv_amdgpu_winsys_bo fence={.bo=9};
   struct radv_amdgpu_ctx ctx={.ws=&ws,.fence_bo=&fence.base};
   struct radv_amdgpu_cs_ib_info ib={.ib_mc_address=0x10000,.size=16,.ip_type=AMD_IP_GFX};
   struct radv_amdgpu_cs_request req={.ip_type=AMDGPU_HW_IP_GFX,.number_of_ibs=1,.ibs=&ib};
   uint32_t wait_handles[]={70,71}, signal_handle=80; uint64_t point=5;
   struct radv_winsys_sem_info sem;
   active_ctx=&ctx; active_sem=&sem;
   for(unsigned gpu=0;gpu<2;gpu++) for(unsigned timeline=0;timeline<2;timeline++) {
      ws.info.is_sgpu=ws.info.is_xclipse940=gpu;
      ws.info.has_timeline_syncobj=timeline;
      for(unsigned scenario=0;scenario<9;scenario++) {
         assert(!live_allocations); allocation_calls=raw_calls=sleeps=0;
         allocation_failure=scenario<4 ? scenario+1 : 0;
         raw_failure=scenario==4 ? -ENOMEM : scenario==5 ? -ECANCELED : scenario==6 ? -EINVAL : -ENOMEM;
         rejection_count=scenario>=4 && scenario<=6 ? 100 : scenario==7 ? 1 : 0;
         clock_ns=0; expected_waits=1; req.seq_no=0;
         ctx.queue_syncobj_wait[AMDGPU_HW_IP_GFX][0]=true;
         sem=(struct radv_winsys_sem_info){.cs_emit_wait=true,.cs_emit_signal=true,
            .wait={.syncobj_count=1,.timeline_syncobj_count=timeline,.syncobj=wait_handles,.points=&point},
            .signal={.syncobj_count=1,.syncobj=&signal_handle}};
         VkResult result=radv_amdgpu_cs_submit(&ctx,&req,&sem);
         assert(!live_allocations);
         if(scenario<=6) {
            assert(result==(scenario<=4 ? VK_ERROR_OUT_OF_HOST_MEMORY : scenario==5 ? VK_ERROR_DEVICE_LOST : VK_ERROR_UNKNOWN));
            assert(sem.cs_emit_wait && ctx.queue_syncobj_wait[AMDGPU_HW_IP_GFX][0]);
            assert(!req.seq_no && (scenario>=4 || !raw_calls));
            allocation_failure=rejection_count=raw_calls=0;
            assert(radv_amdgpu_cs_submit(&ctx,&req,&sem)==VK_SUCCESS);
            assert(!live_allocations && req.seq_no==123);
            assert(!sem.cs_emit_wait && !ctx.queue_syncobj_wait[AMDGPU_HW_IP_GFX][0]);
         } else {
            assert(result==VK_SUCCESS && !sem.cs_emit_wait && !ctx.queue_syncobj_wait[AMDGPU_HW_IP_GFX][0]);
            assert(req.seq_no==123 && raw_calls==(scenario==7 ? 2u : 1u));
            expected_waits=0; rejection_count=0;
            assert(radv_amdgpu_cs_submit(&ctx,&req,&sem)==VK_SUCCESS);
            assert(!live_allocations);
         }
      }
   }
   puts("Native AMD/X940 binary and timeline waits survive allocation/rejection; accepted waits consumed once: OK");
}
'''

SPARSE_MOCKS = r'''
struct radeon_winsys {int unused;};
struct radeon_winsys_bo {uint64_t va; bool resident;};
struct radv_amdgpu_winsys {struct radeon_winsys base;};
struct radv_amdgpu_bo;
struct radv_amdgpu_winsys_bo {
   struct radeon_winsys_bo base;
   void *bo;
   int lock;
   bool is_virtual;
   struct radv_amdgpu_map_range *ranges;
   uint32_t range_count, range_capacity, bo_count, bo_capacity;
   struct radv_amdgpu_winsys_bo **bos;
};
struct radv_amdgpu_map_range {
   uint64_t offset, size;
   struct radv_amdgpu_winsys_bo *bo;
   uint64_t bo_offset;
};
static unsigned realloc_calls, realloc_failure, va_calls;
static bool va_failure;
static int gpu_bo[16]; static uint64_t gpu_offset[16];
static struct radv_amdgpu_winsys *radv_amdgpu_winsys(struct radeon_winsys *ws) {return (void *)ws;}
static bool radv_buffer_is_resident(struct radeon_winsys_bo *bo) {return bo->resident;}
static void u_rwlock_wrlock(int *lock) {assert(!*lock); *lock=1;}
static void u_rwlock_wrunlock(int *lock) {assert(*lock==1); *lock=0;}
static void *audit_realloc(void *ptr,size_t size) {
   if(++realloc_calls==realloc_failure) return NULL;
   void *result=realloc(ptr,size); assert(result); return result;
}
static int radv_amdgpu_bo_va_op(struct radv_amdgpu_winsys *ws,void *bo,uint64_t off,
   uint64_t size,uint64_t addr,uint32_t flags,uint64_t internal,unsigned op) {
   (void)ws; assert(op==AMDGPU_VA_OP_REPLACE && !flags);
   assert(!internal || internal==AMDGPU_VM_PAGE_PRT);
   assert(addr>=0x10000 && (addr-0x10000+size)<=16*4096 && off%4096==0 && size%4096==0);
   va_calls++; if(va_failure) return -ENOMEM;
   unsigned first=(addr-0x10000)/4096;
   for(unsigned i=0;i<size/4096;i++) {gpu_bo[first+i]=(uintptr_t)bo; gpu_offset[first+i]=bo ? off+i*4096 : 0;}
   return 0;
}
#define realloc audit_realloc
'''

SPARSE_MAIN = r'''
static void validate(struct radv_amdgpu_winsys_bo *p) {
   assert(!p->lock); unsigned page=0, unique=0;
   for(unsigned i=0;i<p->range_count;i++) {
      struct radv_amdgpu_map_range *r=&p->ranges[i]; assert(r->offset==page*4096 && r->size);
      for(unsigned j=0;j<r->size/4096;j++,page++) {
         assert(gpu_bo[page]==(r->bo ? (int)(uintptr_t)r->bo->bo : 0));
         if(r->bo) assert(gpu_offset[page]==r->bo_offset+j*4096);
      }
      if(r->bo) {
         bool found=false; for(unsigned j=0;j<p->bo_count;j++) found|=p->bos[j]==r->bo;
         assert(found);
         bool first=true; for(unsigned j=0;j<i;j++) first &= p->ranges[j].bo!=r->bo;
         unique+=first;
      }
   }
   assert(page==16 && p->bo_count==unique);
   for(unsigned i=0;i<p->bo_count;i++) for(unsigned j=i+1;j<p->bo_count;j++) assert(p->bos[i]!=p->bos[j]);
}
static struct radv_amdgpu_winsys_bo parent(void) {
   struct radv_amdgpu_winsys_bo p={.base={.va=0x10000},.is_virtual=true,.range_count=1,.range_capacity=1};
   p.ranges=calloc(1,sizeof(*p.ranges)); assert(p.ranges); p.ranges[0].size=16*4096;
   memset(gpu_bo,0,sizeof(gpu_bo)); memset(gpu_offset,0,sizeof(gpu_offset));
   realloc_calls=va_calls=0; va_failure=false; return p;
}
int main(void) {
   struct radv_amdgpu_winsys ws={0}; struct radv_amdgpu_winsys_bo b[2]={{.bo=(void *)1},{.bo=(void *)2}};
   for(unsigned fail=1;fail<=2;fail++) {
      struct radv_amdgpu_winsys_bo p=parent(); realloc_failure=fail;
      assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,4096,2*4096,&b[0].base,0)==VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(!va_calls && p.range_count==1 && !p.bo_count); validate(&p);
      realloc_failure=0;
      assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,4096,2*4096,&b[0].base,0)==VK_SUCCESS);
      validate(&p);
      realloc_calls=va_calls=0; realloc_failure=fail;
      assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,5*4096,4096,&b[1].base,0)==VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(!va_calls && p.range_count==3 && p.bo_count==1); validate(&p);
      realloc_failure=0;
      assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,5*4096,4096,&b[1].base,0)==VK_SUCCESS);
      validate(&p); free(p.ranges); free(p.bos);
   }
   struct radv_amdgpu_winsys_bo p=parent(); realloc_failure=0; va_failure=true;
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,4096,2*4096,&b[0].base,0)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(va_calls==1); validate(&p); va_failure=false;
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,4096,5*4096,&b[0].base,0)==VK_SUCCESS); validate(&p);
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,3*4096,2*4096,&b[1].base,4096)==VK_SUCCESS); validate(&p);
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,7*4096,4096,&b[0].base,0)==VK_SUCCESS); validate(&p);
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,3*4096,2*4096,NULL,0)==VK_SUCCESS); validate(&p);
   assert(radv_amdgpu_winsys_bo_virtual_bind(&ws.base,&p.base,0,16*4096,NULL,0)==VK_SUCCESS); validate(&p);
   assert(p.range_count==1 && !p.bo_count); free(p.ranges); free(p.bos);
   puts("Sparse allocation failures leave GPU mappings intact; retries, split/overlap/unbind and residency lists agree: OK");
}
'''


def struct(source, name):
    return re.search(r'^struct ' + name + r' \{.*?^\};', source, re.M | re.S)[0]


def function(source, name):
    match = re.search(r'^static\s+[\w\s*]+?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise ValueError(f'Function not found: {name}')
    start = match.end() - 1
    clean = strip_comments_and_strings(source)
    depth, end = 1, start + 1
    while depth:
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--check', choices=['all', 'submit', 'sparse'], default='all')
    args = parser.parse_args()
    root = args.root.resolve()
    cs = (root / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c').read_text()
    bo = (root / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c').read_text()
    declarations = '\n'.join(struct(cs, name) for name in ['radv_amdgpu_cs_ib_info',
        'radv_winsys_sem_counts', 'radv_winsys_sem_info', 'radv_amdgpu_cs_request'])
    submit = COMMON + declarations + SUBMIT_MOCKS + '\n'.join(function(cs, name) for name in [
        'radv_amdgpu_cs_alloc_syncobj_chunk', 'radv_amdgpu_cs_alloc_timeline_syncobj_chunk',
        'radv_amdgpu_cs_has_user_fence', 'radv_amdgpu_cs_submit']) + SUBMIT_MAIN
    sparse = COMMON + SPARSE_MOCKS + '\n'.join(function(bo, name) for name in [
        'bo_comparator', 'radv_amdgpu_winsys_rebuild_bo_list',
        'radv_amdgpu_winsys_bo_virtual_bind']) + SPARSE_MAIN
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='x940-resource-errors-') as folder:
        for name, code in [('submit', submit), ('sparse', sparse)]:
            if args.check not in ('all', name):
                continue
            cfile, executable = Path(folder) / f'{name}.c', Path(folder) / name
            cfile.write_text(code)
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
            subprocess.run([args.cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                '-Werror', '-Wno-sign-compare', '-Wno-unused-function', '-Wno-unused-variable', *flags,
                '-I', str(root / 'include'), str(cfile), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
    print('Host resource-error checks passed; Android build and GPU stability remain untested.')


if __name__ == '__main__':
    main()
