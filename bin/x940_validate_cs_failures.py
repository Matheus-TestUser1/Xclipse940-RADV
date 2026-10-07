#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual CS functions with allocation/map/submit fault injection."""

import argparse
from pathlib import Path
import resource
import subprocess
import tempfile

from x940_validate_emitters import function


MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_ERROR_DEVICE_LOST (-4)
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define align(a,b) (((a) + (b) - 1) & ~((uint64_t)(b) - 1))
#define C_3F2_IB_SIZE 0xfff00000u
#define G_3F2_IB_SIZE(v) ((v) & 0xfffffu)
#define S_3F2_CHAIN(v) ((v) << 20)
#define S_3F2_VALID(v) ((v) << 23)
#define PKT3_INDIRECT_BUFFER 0x3fu
#define PKT3_WRITE_DATA 0x37u
#define PKT3_ACQUIRE_MEM 0x58u
#define PKT3_SHADER_TYPE_S(v) ((v)<<1)
#define S_370_DST_SEL(v) ((v)<<8)
#define V_370_MEM 5u
#define V_370_ME 0u
#define S_370_WR_CONFIRM(v) ((v)<<20)
#define S_370_ENGINE_SEL(v) ((v)<<30)
#define S_0301F0_TC_WB_ACTION_ENA(v) ((v)<<18)
#define S_0301F0_TC_NC_ACTION_ENA(v) ((v)<<19)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define GFX8 8
#define GFX9 9
#define PKT3(op,n,p) (0xc0000000u | ((n) << 16) | ((op) << 8) | (p))
#define AMDGPU_HW_IP_GFX 0
#define AMD_IP_GFX AMDGPU_HW_IP_GFX
#define AMDGPU_IB_FLAG_PREEMPT 1
#define AMD_NUM_IP_TYPES 2
#define RADV_MAX_IBS_PER_SUBMIT 192
struct radeon_cmdbuf {uint64_t cdw, max_dw, reserved_dw; uint32_t *buf;};
struct radeon_winsys_bo {uint64_t va;};
struct radv_amdgpu_ib {struct radeon_winsys_bo *bo; uint64_t va; unsigned cdw;};
struct radv_amdgpu_cs_ib_info {int64_t flags; uint64_t ib_mc_address; uint32_t size; unsigned ip_type;};
struct radv_amdgpu_winsys {
   struct {
      VkResult (*cs_finalize)(struct radeon_cmdbuf *);
      void (*buffer_destroy)(void *, struct radeon_winsys_bo *);
      void (*cs_add_buffer)(struct radeon_cmdbuf *, struct radeon_winsys_bo *);
      void (*cs_execute_ib)(struct radeon_cmdbuf *, struct radeon_winsys_bo *, uint64_t, uint32_t, bool);
   } base;
   struct {struct {unsigned ib_alignment;} ip[2]; uint32_t max_submitted_ibs[2]; unsigned gfx_level;} info;
   struct {int lock;} global_bo_list;
};
struct radv_amdgpu_cs {
   struct radeon_cmdbuf base;
   struct radv_amdgpu_winsys *ws;
   struct {uint32_t size; uint64_t ib_mc_address;} ib;
   struct radeon_winsys_bo *ib_buffer;
   uint8_t *ib_mapped;
   struct radv_amdgpu_ib *ib_buffers;
   unsigned num_ib_buffers, max_num_ib_buffers;
   uint32_t *ib_size_ptr;
   VkResult status;
   struct radv_amdgpu_cs *chained_to;
   bool use_ib;
   bool is_secondary;
   unsigned hw_ip;
   unsigned num_buffers, num_virtual_buffers;
   struct drm_amdgpu_bo_list_entry *handles;
   struct radeon_winsys_bo **virtual_buffers;
};
struct drm_amdgpu_bo_list_entry {uint32_t bo_handle, bo_priority;};
struct radv_amdgpu_cs_request {
   unsigned ip_type, ip_instance, ring;
   struct drm_amdgpu_bo_list_entry *handles;
   uint32_t num_handles, number_of_ibs;
   struct radv_amdgpu_cs_ib_info *ibs;
};
struct radv_amdgpu_ctx {int unused;};
struct radv_winsys_sem_info {int unused;};
static struct radeon_winsys_bo old_bo = {.va=0x10000}, new_bo = {.va=0x20000};
static uint32_t old_words[128], new_words[128], child_words[128];
static unsigned creates, maps, destroys, additions, submissions, assignments, handle_frees, stack_frees;
static VkResult create_failure, submit_failure;
static bool map_failure, append_failure, stack_failure, list_failure, buffer_failure;
static void *handle_allocation, *stack_allocation;
static struct radv_amdgpu_cs *radv_amdgpu_cs(struct radeon_cmdbuf *cs) {return (void *)cs;}
static struct {struct radeon_winsys_bo base;} *radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo) {return (void *)bo;}
static uint32_t get_nop_packet(struct radv_amdgpu_cs *cs) {(void)cs; return 0xffff1000;}
static void radv_amdgpu_winsys_cs_pad(struct radeon_cmdbuf *cs, unsigned leave) {(void)cs; (void)leave;}
static void radeon_emit_unchecked(struct radeon_cmdbuf *cs, uint32_t word) {cs->buf[cs->cdw++] = word;}
static void radeon_emit(struct radeon_cmdbuf *cs, uint32_t word) {
   cs->reserved_dw=MAX2(cs->reserved_dw,cs->cdw+1);
   radeon_emit_unchecked(cs,word);
}
static void radeon_emit_array(struct radeon_cmdbuf *cs, const uint32_t *words, size_t count) {
   for(size_t i=0;i<count;i++) radeon_emit(cs,words[i]);
}
static unsigned radv_amdgpu_cs_get_initial_size(struct radv_amdgpu_winsys *ws, unsigned ip) {
   (void)ws; (void)ip; return 256;
}
static VkResult radv_amdgpu_cs_bo_create(struct radv_amdgpu_cs *cs, uint32_t bytes) {
   creates++; assert(bytes && bytes % 16 == 0); cs->ib_buffer = NULL;
   if (create_failure) return create_failure;
   cs->ib_buffer = &new_bo; return VK_SUCCESS;
}
static void *radv_buffer_map(void *ws, struct radeon_winsys_bo *bo) {
   (void)ws; maps++;
   if(bo->va==0x30000) return child_words;
   assert(bo == &new_bo || bo == &old_bo);
   return map_failure ? NULL : bo == &new_bo ? new_words : old_words + 8;
}
static void destroy(void *ws, struct radeon_winsys_bo *bo) {(void)ws; assert(bo == &new_bo); destroys++;}
static void add(struct radeon_cmdbuf *cs, struct radeon_winsys_bo *bo) {(void)cs; assert(bo == &new_bo); additions++;}
static void radv_amdgpu_cs_add_buffer_internal(struct radv_amdgpu_cs *cs, uint32_t handle, unsigned priority) {
   (void)handle; (void)priority;
   if(buffer_failure) cs->status=VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void radv_amdgpu_cs_add_buffer(struct radeon_cmdbuf *cs, struct radeon_winsys_bo *bo) {
   (void)bo; radv_amdgpu_cs_add_buffer_internal(radv_amdgpu_cs(cs),0,0);
}
static void *audit_realloc(void *ptr, size_t size) {return append_failure ? NULL : realloc(ptr,size);}
static void *audit_malloc(size_t size) {
   assert(!stack_allocation);
   return stack_allocation = stack_failure ? NULL : malloc(size);
}
static void audit_free(void *ptr) {
   if (ptr && ptr == handle_allocation) {handle_frees++; handle_allocation = NULL;}
   if (ptr && ptr == stack_allocation) {stack_frees++; stack_allocation = NULL;}
   free(ptr);
}
static void u_rwlock_rdlock(int *lock) {assert(!*lock); *lock = 1;}
static void u_rwlock_rdunlock(int *lock) {assert(*lock == 1); *lock = 0;}
static unsigned radv_amdgpu_count_ibs(struct radeon_cmdbuf **array, unsigned count,
   unsigned initial, unsigned cont, unsigned post) {
   assert(count == 1 && !initial && !cont && !post);
   return radv_amdgpu_cs(array[0])->num_ib_buffers;
}
static VkResult radv_amdgpu_get_bo_list(struct radv_amdgpu_winsys *ws,
   struct radeon_cmdbuf **array, unsigned count, struct radeon_cmdbuf **initial, unsigned ni,
   struct radeon_cmdbuf **cont, unsigned nc, struct radeon_cmdbuf **post, unsigned np,
   unsigned *nhandles, struct drm_amdgpu_bo_list_entry **handles) {
   (void)ws; (void)array; (void)initial; (void)cont; (void)post;
   assert(count == 1 && !ni && !nc && !np);
   if (list_failure) return VK_ERROR_OUT_OF_HOST_MEMORY;
   *handles = malloc(sizeof(**handles)); assert(*handles);
   handle_allocation = *handles; *nhandles = 1; return VK_SUCCESS;
}
static unsigned radv_amdgpu_get_num_ibs_per_cs(const struct radv_amdgpu_cs *cs) {return cs->use_ib ? 1 : cs->num_ib_buffers;}
static struct radv_amdgpu_cs_ib_info radv_amdgpu_cs_ib_to_info(struct radv_amdgpu_cs *cs, struct radv_amdgpu_ib ib) {
   return (struct radv_amdgpu_cs_ib_info){.ib_mc_address=ib.va,.size=ib.cdw,.ip_type=cs->hw_ip};
}
static VkResult radv_amdgpu_cs_submit(struct radv_amdgpu_ctx *ctx,
   struct radv_amdgpu_cs_request *request, struct radv_winsys_sem_info *sem) {
   (void)ctx; (void)sem; assert(request->number_of_ibs && request->handles);
   submissions++; return submit_failure;
}
static void radv_assign_last_submit(struct radv_amdgpu_ctx *ctx, struct radv_amdgpu_cs_request *request) {
   (void)ctx; (void)request; assignments++;
}
#define realloc audit_realloc
#define malloc audit_malloc
#define free audit_free
#define STACK_ARRAY_SIZE 8
#define STACK_ARRAY(type,name,size) type _stack_##name[STACK_ARRAY_SIZE]; \
   type *const name = (size) <= STACK_ARRAY_SIZE ? _stack_##name : malloc((size)*sizeof(type))
#define STACK_ARRAY_FINISH(name) if (name != _stack_##name) free(name)
'''

MAIN = r'''
static void init(struct radv_amdgpu_cs *cs, struct radv_amdgpu_winsys *ws, bool use_ib) {
   memset(cs,0,sizeof(*cs));
   for (unsigned i=0;i<128;i++) old_words[i] = new_words[i] = 0xfeedface;
   cs->base = (struct radeon_cmdbuf){.cdw=4,.max_dw=64,.reserved_dw=4,.buf=old_words+8};
   cs->ws=ws; cs->ib_buffer=&old_bo; cs->ib_mapped=(void *)(old_words+8);
   cs->ib_size_ptr=&cs->ib.size; cs->use_ib=use_ib;
   creates=maps=destroys=additions=submissions=assignments=handle_frees=stack_frees=0;
   create_failure=submit_failure=VK_SUCCESS;
   map_failure=append_failure=stack_failure=list_failure=buffer_failure=false;
   assert(!handle_allocation && !stack_allocation);
}
static void guards(void) {for(unsigned i=0;i<8;i++) assert(old_words[i] == 0xfeedface);}
int main(void) {
   struct radv_amdgpu_winsys ws = {
      .base={.cs_finalize=radv_amdgpu_cs_finalize,.buffer_destroy=destroy,.cs_add_buffer=add},
      .info={.ip={{.ib_alignment=16},{.ib_alignment=16}},.max_submitted_ibs={192,192},.gfx_level=GFX9},
   };
   struct radv_amdgpu_cs cs;
   for(unsigned use_ib=0;use_ib<2;use_ib++) {
      for(unsigned failure=0;failure<3;failure++) {
         init(&cs,&ws,use_ib);
         if(failure<2) create_failure=failure ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY;
         else map_failure=true;
         radv_amdgpu_cs_grow(&cs.base,64);
         VkResult expected=failure<2 ? create_failure : VK_ERROR_OUT_OF_DEVICE_MEMORY;
         assert(cs.status==expected && !cs.base.cdw && cs.ib_buffer==&old_bo);
         assert(cs.base.buf==old_words+8 && cs.ib_mapped==(void *)(old_words+8));
         assert(cs.base.max_dw==64 && !cs.num_ib_buffers && !additions);
         assert(creates==1 && maps==(failure==2 ? 1u : 0u) && destroys==(failure==2 ? 1u : 0u));
         uint32_t snapshot[128]; memcpy(snapshot,old_words,sizeof(snapshot));
         assert(radv_amdgpu_cs_finalize(&cs.base)==expected);
         assert(!memcmp(snapshot,old_words,sizeof(snapshot))); guards();
         free(cs.ib_buffers);
      }
      init(&cs,&ws,use_ib); append_failure=true;
      radv_amdgpu_cs_grow(&cs.base,64);
      assert(cs.status==VK_ERROR_OUT_OF_HOST_MEMORY && cs.ib_buffer==&old_bo);
      assert(!cs.num_ib_buffers && !creates && !maps && !additions);
      free(cs.ib_buffers);
      init(&cs,&ws,use_ib);
      radv_amdgpu_cs_grow(&cs.base,64);
      assert(cs.status==VK_SUCCESS && cs.ib_buffer==&new_bo && cs.num_ib_buffers==1);
      assert(cs.ib_buffers[0].bo==&old_bo && cs.base.buf==new_words && cs.ib_mapped==(void *)new_words);
      assert(creates==1 && maps==1 && additions==1 && !destroys);
      if(use_ib) {
         assert(old_words[12]==PKT3(PKT3_INDIRECT_BUFFER,2,0));
         assert(old_words[13]==new_bo.va && old_words[14]==0);
         assert(old_words[15]==(S_3F2_CHAIN(1)|S_3F2_VALID(1)));
      }
      guards(); free(cs.ib_buffers);
   }
   puts("IB create/map/append failures preserve ownership and stop writes; success chains intact: OK");
   struct radeon_winsys_bo child_bo={.va=0x30000};
   struct radv_amdgpu_ib child_ib={.bo=&child_bo,.va=child_bo.va};
   struct drm_amdgpu_bo_list_entry child_handle={.bo_handle=42};
   for(unsigned ib2=0;ib2<2;ib2++) {
      for(unsigned failure=0;failure<5;failure++) {
         init(&cs,&ws,ib2);
         cs.base.cdw=cs.base.reserved_dw=64;
         child_ib.cdw=ib2 ? 4 : 80;
         for(unsigned i=0;i<128;i++) child_words[i]=i;
         struct radv_amdgpu_cs child={.use_ib=ib2,.num_ib_buffers=1,.ib_buffers=&child_ib,
            .num_buffers=1,.handles=&child_handle,.ib={.size=4,.ib_mc_address=child_bo.va}};
         if(failure==1) create_failure=VK_ERROR_OUT_OF_DEVICE_MEMORY;
         if(failure==2) map_failure=true;
         if(failure==3) append_failure=true;
         if(failure==4) buffer_failure=true;
         radv_amdgpu_cs_execute_secondary(&cs.base,&child.base,ib2);
         if(failure) {
            assert(cs.status!=VK_SUCCESS && !additions);
            assert(cs.ib_buffer==&old_bo && cs.base.buf==old_words+8);
            /* No child mapping, copy, or IB2 emission after a failed growth. */
            assert(maps==(failure==2 ? 1u : 0u));
            assert(old_words[8]==0xfeedface);
         } else {
            assert(cs.status==VK_SUCCESS && cs.ib_buffer==&new_bo && additions==1);
            if(ib2) assert(cs.base.cdw==4 && new_words[0]==PKT3(PKT3_INDIRECT_BUFFER,2,0));
            else assert(cs.base.cdw==80 && !memcmp(new_words,child_words,80*sizeof(uint32_t)));
         }
         guards(); free(cs.ib_buffers);
      }
   }
   puts("Secondary copy and IB2 paths stop after growth/resource failure; success preserved: OK");
   for(unsigned failure=0;failure<4;failure++) {
      init(&cs,&ws,true); cs.hw_ip=1;
      if(failure==1) create_failure=VK_ERROR_OUT_OF_HOST_MEMORY;
      if(failure==2) map_failure=true;
      if(failure==3) append_failure=true;
      radv_amdgpu_cs_chain_dgc_ib(&cs.base,0x40000,32,0x50000,false);
      if(failure) {
         assert(cs.status!=VK_SUCCESS && cs.ib_buffer==&old_bo && !cs.num_ib_buffers);
         assert(cs.base.buf==old_words+8 && cs.ib_mapped==(void *)(old_words+8) && !additions);
         assert(creates==(failure==3 ? 0u : 1u));
         assert(maps==(failure==2 ? 1u : 0u) && destroys==(failure==2 ? 1u : 0u));
      } else {
         assert(cs.status==VK_SUCCESS && cs.ib_buffer==&new_bo && cs.num_ib_buffers==1);
         assert(cs.base.buf==new_words && cs.ib_mapped==(void *)new_words && additions==1);
         /* The WRITE_DATA patch address starts at a DWORD, not necessarily uint64_t alignment. */
         assert(old_words[17]==new_bo.va && old_words[18]==0);
         assert(cs.ib_size_ptr==old_words+19);
      }
      guards(); free(cs.ib_buffers);
   }
   puts("DGC create/map/finalize failures preserve ownership; unaligned trailer patch is safe: OK");
   struct radv_amdgpu_ctx ctx={0}; struct radv_winsys_sem_info sem={0};
   for(unsigned failure=0;failure<6;failure++) {
      init(&cs,&ws,false);
      cs.num_ib_buffers=failure==2 || failure>=4 ? 9 : 1;
      cs.ib_buffers=calloc(cs.num_ib_buffers,sizeof(*cs.ib_buffers)); assert(cs.ib_buffers);
      for(unsigned i=0;i<cs.num_ib_buffers;i++) cs.ib_buffers[i]=(struct radv_amdgpu_ib){.bo=&old_bo,.va=old_bo.va,.cdw=4};
      submit_failure=failure==1 || failure==5 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
      stack_failure=failure==2; list_failure=failure==3;
      struct radeon_cmdbuf *array[]={&cs.base};
      VkResult result=radv_amdgpu_winsys_cs_submit_internal(&ctx,0,&sem,array,1,NULL,0,NULL,0,NULL,0,false);
      VkResult expected=failure==0 || failure==4 ? VK_SUCCESS : submit_failure ? submit_failure : VK_ERROR_OUT_OF_HOST_MEMORY;
      assert(result==expected && !handle_allocation && !stack_allocation && !ws.global_bo_list.lock);
      assert(submissions==(failure<2 || failure>=4 ? 1u : 0u) && assignments==(expected==VK_SUCCESS ? 1u : 0u));
      assert(handle_frees==(failure<2 || failure>=4 ? 1u : 0u));
      assert(stack_frees==(failure>=4 ? 1u : 0u));
      free(cs.ib_buffers);
   }
   puts("Submit/IB-array/BO-list failures release allocations and locks; success tracking preserved: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c').read_text()
    names = ['radv_amdgpu_cs_add_ib_buffer', 'radv_amdgpu_restore_last_ib',
             'radv_amdgpu_cs_finalize', 'radv_amdgpu_cs_grow',
             'radv_amdgpu_cs_execute_secondary',
             'radv_amdgpu_cs_chain_dgc_ib',
             'radv_amdgpu_winsys_cs_submit_internal']
    code = MOCKS + '\n' + '\n'.join(function(source, name) for name in names) + '\n' + MAIN
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='x940-cs-failures-') as folder:
        cfile, executable = Path(folder) / 'check.c', Path(folder) / 'check'
        cfile.write_text(code)
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                        str(cfile), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print('Host CS fault-injection checks passed; Android/device validation remains pending.')


if __name__ == '__main__':
    main()
