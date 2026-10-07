#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host fault-injection checks of RADV's actual zero-submit synchronization.

Compiles radv_amdgpu_cs_submit_zero and sync_accumulate from the source tree.
DRM calls and fence descriptors are mocked; no GPU access is performed.
"""

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

typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_DEVICE_LOST (-4)
enum amd_ip_type { AMD_IP_GFX };
struct radv_amdgpu_winsys {
   int fd;
   void *dev;
   struct { bool is_sgpu, is_xclipse940, has_timeline_syncobj; } info;
};
struct radv_amdgpu_ctx {
   struct radv_amdgpu_winsys *ws;
   bool queue_syncobj_wait[1][1];
};
struct radv_winsys_sem_info {
   struct {
      unsigned syncobj_count, timeline_syncobj_count;
      uint32_t *syncobj;
      uint64_t *points;
   } wait, signal;
};

/* Each bit represents an independent, initially pending GPU fence. */
static unsigned handles[64];
static struct { bool open; unsigned dependencies; } fds[64];
static unsigned next_fd, merges, fail_merge, imports, transfers, cpu_signals;
static unsigned queue_handle = 1;

static int new_fd(unsigned dependencies) {
   assert(next_fd < 64);
   fds[next_fd].open = true;
   fds[next_fd].dependencies = dependencies;
   return next_fd++;
}
static int close(int fd) {
   assert(fd >= 0 && (unsigned)fd < next_fd && fds[fd].open);
   fds[fd].open = false;
   return 0;
}
static int dup(int fd) {
   assert(fd >= 0 && (unsigned)fd < next_fd && fds[fd].open);
   return new_fd(fds[fd].dependencies);
}
static int sync_merge(const char *name, int fd1, int fd2) {
   (void)name;
   assert(fds[fd1].open && fds[fd2].open);
   if (++merges == fail_merge)
      return -1;
   return new_fd(fds[fd1].dependencies | fds[fd2].dependencies);
}
static unsigned radv_amdgpu_ctx_queue_syncobj(struct radv_amdgpu_ctx *ctx,
                                             unsigned ip, int queue) {
   (void)ctx; assert(!ip && !queue);
   return queue_handle;
}
static int amdgpu_cs_syncobj_export_sync_file(void *dev, uint32_t handle, int *fd) {
   (void)dev; assert(handle < 64);
   *fd = new_fd(handles[handle]);
   return 0;
}
static int amdgpu_cs_syncobj_export_sync_file2(void *dev, uint32_t handle,
                                              uint64_t point, unsigned flags, int *fd) {
   assert(point && !flags);
   return amdgpu_cs_syncobj_export_sync_file(dev, handle, fd);
}
static int amdgpu_cs_syncobj_import_sync_file(void *dev, uint32_t handle, int fd) {
   (void)dev; assert(handle < 64 && fds[fd].open);
   imports++;
   handles[handle] = fds[fd].dependencies;
   return 0;
}
static int amdgpu_cs_syncobj_transfer(void *dev, uint32_t dst, uint64_t dst_point,
                                    uint32_t src, uint64_t src_point, unsigned flags) {
   (void)dev; (void)dst_point;
   assert(dst < 64 && src < 64 && !src_point && !flags);
   transfers++;
   handles[dst] = handles[src];
   return 0;
}
static int amdgpu_cs_syncobj_query2(void *dev, uint32_t *handle, uint64_t *point,
                                  unsigned count, unsigned flags) {
   (void)dev; (void)handle; (void)point; (void)count; (void)flags;
   assert(!"Unexpected timeline export failure");
   return -1;
}
static int drmSyncobjSignal(int fd, uint32_t *handle, unsigned count) {
   (void)fd; (void)handle; (void)count;
   cpu_signals++;
   return 0;
}
static void reset(unsigned fail_at) {
   for (unsigned i = 0; i < 64; i++) {
      handles[i] = 0;
      fds[i].open = false;
      fds[i].dependencies = 0;
   }
   handles[1] = 1; handles[10] = 2; handles[11] = 4;
   next_fd = merges = imports = transfers = cpu_signals = 0;
   fail_merge = fail_at;
   queue_handle = 1;
}
static void no_open_fds(void) {
   for (unsigned i = 0; i < next_fd; i++)
      assert(!fds[i].open);
}
'''

MAIN = r'''
int main(void) {
   uint32_t wait_handles[] = {10, 11}, signal_handles[] = {30, 31};
   uint64_t wait_points[] = {3, 5}, signal_points[] = {7};
   struct radv_amdgpu_winsys ws = {
      .info = {.is_sgpu = true, .is_xclipse940 = true, .has_timeline_syncobj = true},
   };
   struct radv_amdgpu_ctx ctx = {.ws = &ws};
   struct radv_winsys_sem_info sem = {
      .wait = {.syncobj = wait_handles, .points = wait_points},
      .signal = {.syncobj_count = 1, .timeline_syncobj_count = 1,
                 .syncobj = signal_handles, .points = signal_points},
   };

   /* Binary only, timeline only, and a binary followed by a timeline wait.
    * Fail both the first merge and a later merge after a successful one.
    */
   for (unsigned kind = 0; kind < 3; kind++) {
      sem.wait.syncobj_count = kind == 0 ? 2 : kind == 2 ? 1 : 0;
      sem.wait.timeline_syncobj_count = 2 - sem.wait.syncobj_count;
      for (unsigned fail_at = 1; fail_at <= 2; fail_at++) {
         for (unsigned pending = 0; pending < 2; pending++) {
            reset(fail_at);
            ctx.queue_syncobj_wait[0][0] = pending;
            assert(radv_amdgpu_cs_submit_zero(&ctx, AMD_IP_GFX, 0, &sem) == VK_ERROR_DEVICE_LOST);
            assert(merges == fail_at && !imports && !transfers && !cpu_signals);
            assert(handles[1] == 1 && !handles[30] && !handles[31]);
            assert(ctx.queue_syncobj_wait[0][0] == (bool)pending);
            no_open_fds();
         }
      }
   }
   puts("Binary/timeline/mixed merge failures: no imports or signals; all FDs closed: OK");

   /* Successful merges must preserve all pending dependencies in both output
    * semaphores, using either syncobj transfer or binary sync_file import.
    */
   for (unsigned amd = 0; amd < 2; amd++) {
      ws.info.is_sgpu = !amd; ws.info.is_xclipse940 = !amd;
      for (unsigned transfer = 0; transfer < 2; transfer++) {
         ws.info.has_timeline_syncobj = transfer;
         for (unsigned kind = 0; kind < 3; kind++) {
            sem.wait.syncobj_count = kind == 0 ? 2 : kind == 2 ? 1 : 0;
            sem.wait.timeline_syncobj_count = 2 - sem.wait.syncobj_count;
            reset(0); ctx.queue_syncobj_wait[0][0] = false;
            assert(radv_amdgpu_cs_submit_zero(&ctx, AMD_IP_GFX, 0, &sem) == VK_SUCCESS);
            assert(handles[1] == 7 && handles[30] == 7 && handles[31] == 7);
            assert(merges == 2 && imports == (transfer ? 1u : 2u));
            assert(transfers == (transfer ? 2u : 1u) && !cpu_signals);
            assert(ctx.queue_syncobj_wait[0][0]);
            no_open_fds();
         }
      }
   }
   puts("AMD/X940 success paths retain queue and wait dependencies: OK");

   reset(0); ctx.queue_syncobj_wait[0][0] = false;
   sem.wait.syncobj_count = sem.wait.timeline_syncobj_count = 0;
   assert(radv_amdgpu_cs_submit_zero(&ctx, AMD_IP_GFX, 0, &sem) == VK_SUCCESS);
   assert(!merges && !imports && transfers == 2 && !cpu_signals);
   assert(handles[30] == 1 && handles[31] == 1 && !ctx.queue_syncobj_wait[0][0]);
   no_open_fds();
   queue_handle = 0;
   assert(radv_amdgpu_cs_submit_zero(&ctx, AMD_IP_GFX, 0, &sem) == VK_ERROR_OUT_OF_HOST_MEMORY);
   puts("No-wait and missing queue-syncobj paths: OK");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cs = (root / 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c').read_text()
    libsync = (root / 'src/util/libsync.h').read_text()
    start = libsync.index('static inline int sync_accumulate(')
    accumulate = libsync[start:libsync.index('\n}\n', start) + 2]
    code = MOCKS + '\n' + accumulate + '\n'
    code += function(cs, 'radv_amdgpu_cs_submit_zero') + '\n' + MAIN
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='x940-zero-submit-sync-') as folder:
        source = Path(folder) / 'check.c'
        executable = Path(folder) / 'check'
        source.write_text(code)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print('Host synchronization checks passed; Android build/device tests remain pending.')


if __name__ == '__main__':
    main()
