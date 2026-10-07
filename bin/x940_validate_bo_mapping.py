#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise actual winsys map/unmap functions with real host memory mappings."""

import argparse
from pathlib import Path
import resource
import re
import subprocess
import tempfile

from x940_regmap_audit import strip_comments_and_strings


def function(source, name):
    match = re.search(r'(?m)^static void\s*\*?\s+' + re.escape(name) + r'\(', source)
    if not match:
        raise ValueError(f'Function not found: {name}')
    start = source.index('{', match.end())
    clean = strip_comments_and_strings(source)
    depth, end = 1, start + 1
    while depth:
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    return source[match.start():end]


MOCKS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "drm-uapi/amdgpu_drm.h"
struct radeon_winsys {int unused;};
struct radeon_winsys_bo {uint64_t size;};
struct radv_amdgpu_winsys {struct radeon_winsys base; int fd;};
struct radv_amdgpu_winsys_bo {
   struct radeon_winsys_bo base;
   void *cpu_map;
   uint32_t bo_handle;
   int bo;
};
static int backing_fd, ioctl_error;
static bool map_error;
static unsigned ioctl_calls, map_calls, unmap_calls, libdrm_calls;
static uint64_t returned_offset;
static size_t page_size;
static __attribute__((unused)) struct radv_amdgpu_winsys *radv_amdgpu_winsys(struct radeon_winsys *ws) {return (void *)ws;}
static struct radv_amdgpu_winsys_bo *radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo) {return (void *)bo;}
static int __attribute__((unused)) drmCommandWriteRead(int fd, unsigned long command, void *data, unsigned long size) {
   union drm_amdgpu_gem_mmap *args=data;
   assert(fd==backing_fd && command==DRM_AMDGPU_GEM_MMAP && size==sizeof(*args));
   assert(args->in.handle==42 && args->in._pad==0);
   ioctl_calls++;
   if(ioctl_error) return ioctl_error;
   args->out.addr_ptr=returned_offset;
   return 0;
}
static void *audit_mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset) {
   map_calls++;
   if((flags & MAP_FIXED) && fd==-1)
      assert(prot==PROT_NONE && (flags & (MAP_PRIVATE|MAP_ANONYMOUS))==(MAP_PRIVATE|MAP_ANONYMOUS));
   if(map_error) {errno=ENOMEM; return MAP_FAILED;}
   return mmap(addr,size,prot,flags,fd,offset);
}
static int audit_munmap(void *addr, size_t size) {unmap_calls++; return munmap(addr,size);}
/* Compatibility stub allows this test to compile against the old implementation. */
static int __attribute__((unused)) amdgpu_bo_cpu_map(int bo, void **data) {
   (void)bo; libdrm_calls++;
   *data=mmap(NULL,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,backing_fd,returned_offset);
   return *data==MAP_FAILED ? -ENOMEM : 0;
}
#define mmap audit_mmap
#define munmap audit_munmap
'''

MAIN = r'''
static void reset_counts(void) {
   ioctl_calls=map_calls=unmap_calls=libdrm_calls=0;
   ioctl_error=0; map_error=false;
}
int main(void) {
   page_size=sysconf(_SC_PAGESIZE); assert(page_size>0);
   backing_fd=memfd_create("x940-map-test",0); assert(backing_fd>=0);
   assert(!ftruncate(backing_fd,2*page_size));
   returned_offset=page_size; /* Check that the ioctl's offset reaches mmap. */
   struct radv_amdgpu_winsys ws={.fd=backing_fd};
   struct radv_amdgpu_winsys_bo bo={.base={.size=page_size},.bo_handle=42};

   void *reserved=mmap(NULL,page_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
   assert(reserved!=MAP_FAILED);
   assert(radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,true,reserved)==reserved);
   radv_amdgpu_winsys_bo_unmap(&ws.base,&bo.base,false);

   reset_counts(); ioctl_error=-EINVAL;
   assert(!radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,false,NULL));
   assert(!bo.cpu_map && ioctl_calls==1 && !map_calls && !libdrm_calls);
   reset_counts(); map_error=true;
   assert(!radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,false,NULL));
   assert(!bo.cpu_map && ioctl_calls==1 && map_calls==1 && !libdrm_calls);
   puts("GEM_MMAP/mmap failures leave the BO unmapped: OK");

   reset_counts();
   for(unsigned i=0;i<64;i++) {
      uint32_t *data=radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,false,NULL);
      assert(data && data==bo.cpu_map && ioctl_calls==i+1 && map_calls==i+1);
      assert(data[0]==i);
      data[0]=i+1;
      assert(radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,false,NULL)==data);
      assert(ioctl_calls==i+1 && map_calls==i+1);
      radv_amdgpu_winsys_bo_unmap(&ws.base,&bo.base,false);
      assert(!bo.cpu_map && unmap_calls==i+1);
      radv_amdgpu_winsys_bo_unmap(&ws.base,&bo.base,false);
      assert(unmap_calls==i+1);
   }
   assert(!libdrm_calls);
   uint32_t value;
   assert(pread(backing_fd,&value,sizeof(value),returned_offset)==sizeof(value) && value==64);
   assert(pread(backing_fd,&value,sizeof(value),0)==sizeof(value) && value==0);
   puts("Repeated map/unmap, cached map, mapped writes, and ioctl offset: OK");

   reset_counts();
   reserved=mmap(NULL,page_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
   assert(reserved!=MAP_FAILED);
   map_calls=0;
   for(unsigned i=0;i<16;i++) {
      uint32_t *data=radv_amdgpu_winsys_bo_map(&ws.base,&bo.base,true,reserved);
      assert(data==reserved && data==bo.cpu_map && ioctl_calls==i+1);
      assert(data[0]==64+i); data[0]++;
      radv_amdgpu_winsys_bo_unmap(&ws.base,&bo.base,true);
      assert(!bo.cpu_map && !unmap_calls && map_calls==2*(i+1));
      /* A PROT_NONE reservation remains at the requested address. */
      unsigned char residency;
      assert(!mincore(reserved,page_size,&residency));
   }
   assert(!libdrm_calls && !munmap(reserved,page_size));
   assert(!close(backing_fd));
   puts("Placed mappings return the requested address; reserved unmaps can be remapped: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c').read_text()
    names = ['radv_amdgpu_winsys_bo_map', 'radv_amdgpu_winsys_bo_unmap']
    code = MOCKS + '\n' + '\n'.join(function(source, name) for name in names) + '\n' + MAIN
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='x940-bo-mapping-') as folder:
        cfile, executable = Path(folder) / 'check.c', Path(folder) / 'check'
        cfile.write_text(code)
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-I', str(root / 'include'), *flags,
                        str(cfile), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print('Host mapping checks passed; SGPU/device validation remains pending.')


if __name__ == '__main__':
    main()
