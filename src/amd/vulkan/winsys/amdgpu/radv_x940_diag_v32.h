/* SPDX-License-Identifier: MIT
 * Opt-in Xclipse 940 telemetry. Queries do not submit GPU commands.
 * Query-reset-state (v1) is intentionally excluded: SGPU consumes its
 * per-context reset_counter_query. Query2 preserves this diagnostic state.
 */
#ifndef RADV_X940_DIAG_V32_H
#define RADV_X940_DIAG_V32_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

struct radv_x940_diag_v32_ring {
   struct amdgpu_cs_fence fence;
   uint64_t submit_id;
   uint64_t seen_seq;
   uint64_t seen_flags;
   uint64_t last_log_ns;
   int seen_fr;
   int seen_qr;
   uint32_t seen_expired;
};

struct radv_x940_diag_v32 {
   struct radv_amdgpu_ctx *ctx;
   pthread_mutex_t lock;
   pthread_t thread;
   bool stop;
   uint64_t next_submit_id;
   FILE *file;
   struct radv_x940_diag_v32_ring rings[MAX_RINGS_PER_TYPE];
#if defined(__ANDROID__)
   void *log_lib;
   int (*log_write)(int, const char *, const char *);
#endif
};

static long
radv_x940_diag_v32_tid(void)
{
#if defined(SYS_gettid)
   return syscall(SYS_gettid);
#else
   return (long)getpid();
#endif
}

static void
radv_x940_diag_v32_log(struct radv_x940_diag_v32 *d, bool durable, const char *fmt, ...)
{
   char body[1536], line[1792];
   va_list args;
   va_start(args, fmt);
   vsnprintf(body, sizeof(body), fmt, args);
   va_end(args);
   snprintf(line, sizeof(line),
            "x940-diag-v32: mono_ns=%llu pid=%ld tid=%ld ctx=%p %s",
            (unsigned long long)os_time_get_nano(), (long)getpid(),
            radv_x940_diag_v32_tid(), (void *)d->ctx->ctx, body);
   pthread_mutex_lock(&d->lock);
   fprintf(stderr, "%s\n", line);
   if (d->file) {
      fprintf(d->file, "%s\n", line);
      fflush(d->file);
      if (durable)
         fsync(fileno(d->file));
   }
#if defined(__ANDROID__)
   if (d->log_write)
      d->log_write(durable ? 6 : 4, "RADV-X940", line);
#endif
   pthread_mutex_unlock(&d->lock);
}

static int
radv_x940_diag_v32_reset(uint64_t flags)
{
#ifdef AMDGPU_CTX_QUERY2_FLAGS_RESET
   return !!(flags & AMDGPU_CTX_QUERY2_FLAGS_RESET);
#else
   (void)flags;
   return -1;
#endif
}

static int
radv_x940_diag_v32_guilty(uint64_t flags)
{
#ifdef AMDGPU_CTX_QUERY2_FLAGS_GUILTY
   return !!(flags & AMDGPU_CTX_QUERY2_FLAGS_GUILTY);
#else
   (void)flags;
   return -1;
#endif
}

static int
radv_x940_diag_v32_vramlost(uint64_t flags)
{
#ifdef AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST
   return !!(flags & AMDGPU_CTX_QUERY2_FLAGS_VRAMLOST);
#else
   (void)flags;
   return -1;
#endif
}

static void
radv_x940_diag_v32_sample(struct radv_x940_diag_v32 *d)
{
   uint64_t flags = 0;
   uint64_t begin = os_time_get_nano();
   int qr = amdgpu_cs_query_reset_state2(d->ctx->ctx, &flags);
   for (unsigned ring = 0; ring < MAX_RINGS_PER_TYPE; ++ring) {
      struct radv_x940_diag_v32_ring s;
      pthread_mutex_lock(&d->lock);
      s = d->rings[ring];
      pthread_mutex_unlock(&d->lock);
      if (!s.fence.fence)
         continue;
      uint32_t expired = 0;
      int fr = amdgpu_cs_query_fence_status(&s.fence, 0, 0, &expired);
      if (fr)
         expired = UINT32_MAX; /* An error does not supply a valid completion result. */
      uint64_t now = os_time_get_nano();
      bool changed = s.seen_seq != s.fence.fence || s.seen_flags != flags ||
                     s.seen_fr != fr || s.seen_qr != qr || s.seen_expired != expired;
      if (changed || now - s.last_log_ns >= 1000000000ull) {
         radv_x940_diag_v32_log(d, fr || qr || flags,
            "event=sample submit_id=%llu user_ctx_seq=%llu ip=%u instance=%u ring=%u "
            "fence_rc=%d expired=%d fence_state=%s q2_rc=%d q2_flags=0x%016llx "
            "kernel_reset=%d kernel_guilty=%d vram_lost=%d query_ns=%llu",
            (unsigned long long)s.submit_id, (unsigned long long)s.fence.fence,
            s.fence.ip_type, s.fence.ip_instance, s.fence.ring, fr, fr ? -1 : (int)expired,
            fr ? "QUERY_ERROR" : expired ? "COMPLETE" : "PENDING", qr,
            (unsigned long long)flags, qr ? -1 : radv_x940_diag_v32_reset(flags),
            qr ? -1 : radv_x940_diag_v32_guilty(flags),
            qr ? -1 : radv_x940_diag_v32_vramlost(flags), (unsigned long long)(now - begin));
         pthread_mutex_lock(&d->lock);
         if (d->rings[ring].fence.fence == s.fence.fence) {
            d->rings[ring].seen_seq = s.fence.fence;
            d->rings[ring].seen_flags = flags;
            d->rings[ring].seen_fr = fr;
            d->rings[ring].seen_qr = qr;
            d->rings[ring].seen_expired = expired;
            d->rings[ring].last_log_ns = now;
         }
         pthread_mutex_unlock(&d->lock);
      }
   }
}

static void *
radv_x940_diag_v32_worker(void *opaque)
{
   struct radv_x940_diag_v32 *d = opaque;
   for (;;) {
      pthread_mutex_lock(&d->lock);
      bool stop = d->stop;
      pthread_mutex_unlock(&d->lock);
      if (stop)
         break;
      radv_x940_diag_v32_sample(d);
      os_time_sleep(200000);
   }
   return NULL;
}

static void
radv_x940_diag_v32_ctx_init(struct radv_amdgpu_ctx *ctx)
{
   const char *flag = getenv("RADV_X940_DIAG_MONITOR_V32");
   if (!ctx->ws->info.is_xclipse940 || !flag || strcmp(flag, "1"))
      return;
   struct radv_x940_diag_v32 *d = calloc(1, sizeof(*d));
   if (!d) {
      fprintf(stderr, "x940-diag-v32: event=init_error reason=allocation\n");
      return;
   }
   d->ctx = ctx;
   if (pthread_mutex_init(&d->lock, NULL)) {
      fprintf(stderr, "x940-diag-v32: event=init_error reason=mutex\n");
      free(d);
      return;
   }
   char path[160];
   snprintf(path, sizeof(path), "/data/local/tmp/xclipse940/x940_diag_v32_%ld.log", (long)getpid());
   d->file = fopen(path, "a");
   if (!d->file)
      radv_x940_diag_v32_log(d, false, "event=init_error reason=file errno=%d", errno);
#if defined(__ANDROID__)
   d->log_lib = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
   if (d->log_lib)
      d->log_write = (int (*)(int, const char *, const char *))dlsym(d->log_lib, "__android_log_write");
#endif
   radv_x940_diag_v32_log(d, false,
      "event=ctx_create monitor_ms=200 heartbeat_ms=1000 scope=latest_GFX_fence_per_ring "
      "seq_domain=user_context_not_global_ring file=%s persistent=%d android_log=%d "
      "query1_polled=0 pm4_modified=0",
      path, d->file != NULL,
#if defined(__ANDROID__)
      d->log_write != NULL
#else
      0
#endif
   );
   int ret = pthread_create(&d->thread, NULL, radv_x940_diag_v32_worker, d);
   if (ret) {
      radv_x940_diag_v32_log(d, false, "event=init_error reason=thread error=%d", ret);
      if (d->file)
         fclose(d->file);
#if defined(__ANDROID__)
      if (d->log_lib)
         dlclose(d->log_lib);
#endif
      pthread_mutex_destroy(&d->lock);
      free(d);
      return;
   }
   ctx->x940_diag_v32 = d;
}

static void
radv_x940_diag_v32_ctx_destroy(struct radv_amdgpu_ctx *ctx)
{
   struct radv_x940_diag_v32 *d = ctx->x940_diag_v32;
   if (!d)
      return;
   pthread_mutex_lock(&d->lock);
   d->stop = true;
   pthread_mutex_unlock(&d->lock);
   pthread_join(d->thread, NULL);
   radv_x940_diag_v32_sample(d);
   radv_x940_diag_v32_log(d, true, "event=ctx_destroy monitor_joined=1");
   if (d->file)
      fclose(d->file);
#if defined(__ANDROID__)
   if (d->log_lib)
      dlclose(d->log_lib);
#endif
   pthread_mutex_destroy(&d->lock);
   ctx->x940_diag_v32 = NULL;
   free(d);
}

static uint64_t
radv_x940_diag_v32_submit_begin(struct radv_amdgpu_ctx *ctx,
                              struct radv_amdgpu_cs_request *request, int num_chunks,
                              const struct drm_amdgpu_cs_chunk *chunks)
{
   struct radv_x940_diag_v32 *d = ctx->x940_diag_v32;
   if (!d || request->ip_type != AMDGPU_HW_IP_GFX)
      return 0;
   pthread_mutex_lock(&d->lock);
   uint64_t id = ++d->next_submit_id;
   pthread_mutex_unlock(&d->lock);
   radv_x940_diag_v32_log(d, false,
      "event=submit_begin submit_id=%llu ip=%u instance=%u ring=%u ibs=%u bo_handles=%u chunks=%d",
      (unsigned long long)id, request->ip_type, request->ip_instance, request->ring,
      request->number_of_ibs, request->num_handles, num_chunks);
   for (int i = 0; i < num_chunks; ++i)
      radv_x940_diag_v32_log(d, false, "event=chunk submit_id=%llu index=%d id=0x%x dw=%u",
         (unsigned long long)id, i, chunks[i].chunk_id, chunks[i].length_dw);
   for (unsigned i = 0; i < request->number_of_ibs; ++i)
      radv_x940_diag_v32_log(d, false,
         "event=ib submit_id=%llu index=%u va=0x%016llx dw=%u ip=%u flags=0x%llx",
         (unsigned long long)id, i, (unsigned long long)request->ibs[i].ib_mc_address,
         request->ibs[i].size, request->ibs[i].ip_type, (unsigned long long)request->ibs[i].flags);
   for (unsigned i = 0; i < request->num_handles; ++i)
      radv_x940_diag_v32_log(d, false,
         "event=bo submit_id=%llu index=%u gem_handle=%u priority=%u",
         (unsigned long long)id, i, request->handles[i].bo_handle, request->handles[i].bo_priority);
   return id;
}

static void
radv_x940_diag_v32_submit_result(struct radv_amdgpu_ctx *ctx,
                               struct radv_amdgpu_cs_request *request, uint64_t id, int result)
{
   struct radv_x940_diag_v32 *d = ctx->x940_diag_v32;
   if (!d || !id)
      return;
   radv_x940_diag_v32_log(d, result != 0,
      "event=submit_result submit_id=%llu ioctl_rc=%d user_ctx_seq=%llu ip=%u instance=%u ring=%u",
      (unsigned long long)id, result, result ? 0ull : (unsigned long long)request->seq_no,
      request->ip_type, request->ip_instance, request->ring);
   if (!result && request->ring < MAX_RINGS_PER_TYPE) {
      pthread_mutex_lock(&d->lock);
      d->rings[request->ring].submit_id = id;
      d->rings[request->ring].fence = (struct amdgpu_cs_fence){
         .context = ctx->ctx, .ip_type = request->ip_type, .ip_instance = request->ip_instance,
         .ring = request->ring, .fence = request->seq_no,
      };
      pthread_mutex_unlock(&d->lock);
   }
}

#endif
