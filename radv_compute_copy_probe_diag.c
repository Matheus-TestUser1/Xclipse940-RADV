/*
 * Xclipse 940 RADV - 4096-byte compute-path diagnostic probe
 *
 * Tests:
 *   HAL -> Instance -> PhysicalDevice -> Device -> Graphics Queue
 *   -> 2 x 4 KiB host-visible/coherent buffers
 *   -> CPU fills SRC with a pattern, clears DST
 *   -> vkCmdCopyBuffer(SRC -> DST)
 *   -> vkQueueSubmit + VkFence with finite timeout
 *   -> CPU verifies DST byte-for-byte
 *
 * Deliberately NOT done:
 *   - no shaders
 *   - no draw / dispatch
 *   - no images
 *   - no render pass / dynamic rendering
 *   - no vkQueueWaitIdle / vkDeviceWaitIdle
 *
 * Safety:
 *   - tiny 4 KiB copy
 *   - finite fence wait (default 1 s)
 *   - whole risky section in child process
 *   - parent watchdog defaults to 6 s
 *
 * A userspace watchdog cannot guarantee recovery from a kernel/GPU lockup.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vulkan.h>

#include <stdarg.h>

static FILE *probe_log;

static void
persist_mark(const char *fmt, ...)
{
   if (!probe_log)
      return;

   va_list ap;
   va_start(ap, fmt);
   vfprintf(probe_log, fmt, ap);
   va_end(ap);

   fputc('\n', probe_log);
   fflush(probe_log);
   fsync(fileno(probe_log));
}

static const char *
vk_result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_INCOMPLETE: return "VK_INCOMPLETE";
   case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   default: return "VkResult(?)";
   }
}

static uint32_t
find_host_coherent_type(uint32_t bits,
                        const VkPhysicalDeviceMemoryProperties *mp)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if (!(bits & (1u << i)))
         continue;

      const VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
      const VkMemoryPropertyFlags need =
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      if ((f & need) == need)
         return i;
   }

   return UINT32_MAX;
}

struct buffer_alloc {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize alloc_size;
   uint32_t memory_type;
};

static int
create_buffer_alloc(VkDevice device,
                    PFN_vkCreateBuffer vkCreateBuffer,
                    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements,
                    PFN_vkAllocateMemory vkAllocateMemory,
                    PFN_vkBindBufferMemory vkBindBufferMemory,
                    PFN_vkMapMemory vkMapMemory,
                    const VkPhysicalDeviceMemoryProperties *mp,
                    VkDeviceSize size,
                    struct buffer_alloc *out)
{
   memset(out, 0, sizeof(*out));

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkResult vr = vkCreateBuffer(device, &bci, NULL, &out->buffer);
   if (vr != VK_SUCCESS) {
      fprintf(stderr, "vkCreateBuffer failed: %d (%s)\n",
              vr, vk_result_name(vr));
      return 0;
   }

   VkMemoryRequirements req;
   memset(&req, 0, sizeof(req));
   vkGetBufferMemoryRequirements(device, out->buffer, &req);

   uint32_t mt = find_host_coherent_type(req.memoryTypeBits, mp);
   if (mt == UINT32_MAX) {
      fprintf(stderr,
              "No HOST_VISIBLE|HOST_COHERENT memory type for bits=0x%08x\n",
              req.memoryTypeBits);
      return 0;
   }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = mt,
   };

   vr = vkAllocateMemory(device, &mai, NULL, &out->memory);
   if (vr != VK_SUCCESS) {
      fprintf(stderr, "vkAllocateMemory failed: %d (%s)\n",
              vr, vk_result_name(vr));
      return 0;
   }

   vr = vkBindBufferMemory(device, out->buffer, out->memory, 0);
   if (vr != VK_SUCCESS) {
      fprintf(stderr, "vkBindBufferMemory failed: %d (%s)\n",
              vr, vk_result_name(vr));
      return 0;
   }

   vr = vkMapMemory(device, out->memory, 0, req.size, 0, &out->map);
   if (vr != VK_SUCCESS || !out->map) {
      fprintf(stderr, "vkMapMemory failed: %d (%s)\n",
              vr, vk_result_name(vr));
      return 0;
   }

   out->alloc_size = req.size;
   out->memory_type = mt;

   printf("       buffer=%p memory=%p map=%p req=%llu align=%llu type=%u\n",
          (void *)out->buffer,
          (void *)out->memory,
          out->map,
          (unsigned long long)req.size,
          (unsigned long long)req.alignment,
          mt);

   return 1;
}

static int
run_child(const char *driver_path, uint64_t fence_timeout_ns)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   const VkDeviceSize COPY_SIZE = 4096;

   probe_log = fopen("/data/local/tmp/xclipse940/compute_copy_probe.log", "w");
   if (probe_log) {
      setvbuf(probe_log, NULL, _IONBF, 0);
      persist_mark("START compute_copy_probe size=4096");
   }

   printf("=== Xclipse 940 RADV GPU buffer-copy probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("GPU work: one 4096-byte vkCmdCopyBuffer (RADV compute/meta threshold)\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      return 10;
   }
   printf("[1/15] dlopen OK\n");

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module)
      return 11;

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr != 0 || !base_dev)
      return 12;

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;
   printf("[2/15] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-copy-probe",
      .applicationVersion = 1,
      .pEngineName = "none",
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_3,
   };

   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkInstance instance = VK_NULL_HANDLE;
   VkResult vr = hal->CreateInstance(&ici, NULL, &instance);
   printf("[3/15] vkCreateInstance = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("vkCreateInstance result=%d", vr);
   if (vr != VK_SUCCESS)
      return 20;

#define GET_INST(name) \
   PFN_##name name = (PFN_##name)hal->GetInstanceProcAddr(instance, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetInstanceProcAddr(%s)=NULL\n", #name); \
      return 21; \
   }

   GET_INST(vkEnumeratePhysicalDevices);
   GET_INST(vkGetPhysicalDeviceProperties);
   GET_INST(vkGetPhysicalDeviceMemoryProperties);
   GET_INST(vkGetPhysicalDeviceQueueFamilyProperties);
   GET_INST(vkCreateDevice);
   GET_INST(vkGetDeviceProcAddr);

   uint32_t phys_count = 0;
   vr = vkEnumeratePhysicalDevices(instance, &phys_count, NULL);
   if (vr != VK_SUCCESS || phys_count == 0)
      return 22;

   VkPhysicalDevice *phys = calloc(phys_count, sizeof(*phys));
   if (!phys)
      return 23;

   vr = vkEnumeratePhysicalDevices(instance, &phys_count, phys);
   if (vr != VK_SUCCESS || phys_count == 0)
      return 24;

   VkPhysicalDevice pdev = phys[0];

   VkPhysicalDeviceProperties props;
   memset(&props, 0, sizeof(props));
   vkGetPhysicalDeviceProperties(pdev, &props);

   printf("[4/15] GPU: %s Vulkan=%u.%u.%u\n",
          props.deviceName,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

   VkPhysicalDeviceMemoryProperties mp;
   memset(&mp, 0, sizeof(mp));
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   if (!qf_count)
      return 25;

   VkQueueFamilyProperties *qf = calloc(qf_count, sizeof(*qf));
   if (!qf)
      return 26;

   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qf);

   uint32_t selected_qf = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
         selected_qf = i;
         break;
      }
   }

   if (selected_qf == UINT32_MAX)
      return 27;

   printf("       graphics queue family=%u\n", selected_qf);

   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = selected_qf,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };

   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 0,
      .pEnabledFeatures = NULL,
   };

   VkDevice device = VK_NULL_HANDLE;
   vr = vkCreateDevice(pdev, &dci, NULL, &device);
   printf("[5/15] vkCreateDevice = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("vkCreateDevice result=%d", vr);
   if (vr != VK_SUCCESS)
      return 30;

#define GET_DEV(name) \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetDeviceProcAddr(%s)=NULL\n", #name); \
      return 31; \
   }

   GET_DEV(vkGetDeviceQueue);
   GET_DEV(vkCreateBuffer);
   GET_DEV(vkDestroyBuffer);
   GET_DEV(vkGetBufferMemoryRequirements);
   GET_DEV(vkAllocateMemory);
   GET_DEV(vkFreeMemory);
   GET_DEV(vkBindBufferMemory);
   GET_DEV(vkMapMemory);
   GET_DEV(vkUnmapMemory);
   GET_DEV(vkCreateCommandPool);
   GET_DEV(vkDestroyCommandPool);
   GET_DEV(vkAllocateCommandBuffers);
   GET_DEV(vkBeginCommandBuffer);
   GET_DEV(vkCmdCopyBuffer);
   GET_DEV(vkEndCommandBuffer);
   GET_DEV(vkCreateFence);
   GET_DEV(vkDestroyFence);
   GET_DEV(vkQueueSubmit);
   GET_DEV(vkWaitForFences);
   GET_DEV(vkGetFenceStatus);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   printf("[6/15] vkGetDeviceQueue = %p\n", (void *)queue);
   if (!queue)
      return 32;

   struct buffer_alloc src, dst;

   printf("[7/15] Creating SRC buffer...\n");
   persist_mark("before create SRC");
   if (!create_buffer_alloc(device,
                            vkCreateBuffer,
                            vkGetBufferMemoryRequirements,
                            vkAllocateMemory,
                            vkBindBufferMemory,
                            vkMapMemory,
                            &mp,
                            COPY_SIZE,
                            &src))
      return 40;

   persist_mark("SRC ready");
   printf("[8/15] Creating DST buffer...\n");
   persist_mark("before create DST");
   if (!create_buffer_alloc(device,
                            vkCreateBuffer,
                            vkGetBufferMemoryRequirements,
                            vkAllocateMemory,
                            vkBindBufferMemory,
                            vkMapMemory,
                            &mp,
                            COPY_SIZE,
                            &dst))
      return 41;

   for (uint32_t i = 0; i < COPY_SIZE; i++)
      ((uint8_t *)src.map)[i] = (uint8_t)(((i * 37u) + 0x5au) & 0xffu);

   memset(dst.map, 0, COPY_SIZE);

   printf("[9/15] CPU initialized SRC pattern and cleared DST\n");
   persist_mark("buffers ready and CPU initialized");

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };

   VkCommandPool pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &cpci, NULL, &pool);
   printf("[10/15] vkCreateCommandPool = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 42;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };

   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cbai, &cb);
   if (vr != VK_SUCCESS)
      return 43;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };

   persist_mark("before vkBeginCommandBuffer");
   vr = vkBeginCommandBuffer(cb, &cbbi);
   if (vr != VK_SUCCESS)
      return 44;

   VkBufferCopy region = {
      .srcOffset = 0,
      .dstOffset = 0,
      .size = COPY_SIZE,
   };

   persist_mark("before vkCmdCopyBuffer 4096");
   vkCmdCopyBuffer(cb, src.buffer, dst.buffer, 1, &region);
   persist_mark("after vkCmdCopyBuffer 4096");

   persist_mark("before vkEndCommandBuffer");
   vr = vkEndCommandBuffer(cb);
   printf("[11/15] Recorded one vkCmdCopyBuffer(4096 bytes), end=%d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkEndCommandBuffer result=%d", vr);
   if (vr != VK_SUCCESS)
      return 45;

   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };

   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fci, NULL, &fence);
   if (vr != VK_SUCCESS)
      return 46;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   printf("[12/15] vkQueueSubmit(copy) ...\n");
   persist_mark("before vkQueueSubmit");
   vr = vkQueueSubmit(queue, 1, &si, fence);
   printf("        submit = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkQueueSubmit result=%d", vr);
   if (vr != VK_SUCCESS)
      return 50;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("[13/15] immediate fence = %d (%s)\n",
          fs, vk_result_name(fs));

   printf("[14/15] vkWaitForFences(timeout=%.3f s) ...\n",
          (double)fence_timeout_ns / 1000000000.0);
   persist_mark("before vkWaitForFences");

   vr = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout_ns);
   printf("        wait = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkWaitForFences result=%d", vr);

   if (vr == VK_TIMEOUT) {
      fprintf(stderr,
              "\nTIMEOUT: copy submit did not signal the fence.\n"
              "Exiting immediately without idle waits.\n");
      return 60;
   }

   if (vr != VK_SUCCESS)
      return 61;

   printf("[15/15] Verifying copied bytes on CPU...\n");

   uint32_t bad = 0;
   for (uint32_t i = 0; i < COPY_SIZE; i++) {
      const uint8_t expected =
         (uint8_t)(((i * 37u) + 0x5au) & 0xffu);
      const uint8_t got = ((uint8_t *)dst.map)[i];

      if (got != expected) {
         bad++;
         if (bad <= 8) {
            printf("        mismatch[%u]: got=0x%02x expected=0x%02x\n",
                   i, got, expected);
         }
      }
   }

   if (bad) {
      printf("\nFAIL: %u / %llu bytes mismatched.\n",
             bad, (unsigned long long)COPY_SIZE);
      return 70;
   }

   printf("        verification PASS: all %llu bytes match\n",
          (unsigned long long)COPY_SIZE);
   persist_mark("verification PASS bytes=%llu", (unsigned long long)COPY_SIZE);

   /*
    * Fence signaled, so submitted work completed. Safe to tear down resources
    * without queue/device idle calls.
    */
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);

   vkUnmapMemory(device, src.memory);
   vkUnmapMemory(device, dst.memory);

   vkDestroyBuffer(device, src.buffer, NULL);
   vkDestroyBuffer(device, dst.buffer, NULL);

   vkFreeMemory(device, src.memory, NULL);
   vkFreeMemory(device, dst.memory, NULL);

   printf("\nPASS: GPU copied 4 KiB SRC -> DST and CPU verification matched.\n");
   persist_mark("PASS complete");
   printf("NO SHADER/DRAW/DISPATCH WAS USED.\n");

   return 0;
}

static double
monotonic_seconds(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   const char *driver_path =
      argc >= 2 ? argv[1] : "./libvulkan_radeon.so";

   int watchdog_sec = 6;
   const char *wd = getenv("PROBE_TIMEOUT_SEC");
   if (wd && *wd) {
      int v = atoi(wd);
      if (v >= 2 && v <= 30)
         watchdog_sec = v;
   }

   uint64_t fence_timeout_ns = 1000000000ull;
   const char *ft = getenv("FENCE_TIMEOUT_MS");
   if (ft && *ft) {
      long ms = strtol(ft, NULL, 10);
      if (ms >= 10 && ms <= 5000)
         fence_timeout_ns = (uint64_t)ms * 1000000ull;
   }

   printf("Watchdog timeout: %d seconds\n", watchdog_sec);

   pid_t child = fork();
   if (child < 0) {
      fprintf(stderr, "fork failed: %s\n", strerror(errno));
      return 100;
   }

   if (child == 0) {
      int rc = run_child(driver_path, fence_timeout_ns);
      _exit(rc);
   }

   const double deadline = monotonic_seconds() + watchdog_sec;

   for (;;) {
      int status = 0;
      pid_t r = waitpid(child, &status, WNOHANG);

      if (r == child) {
         if (WIFEXITED(status)) {
            int rc = WEXITSTATUS(status);
            printf("\nwatchdog: child exited normally, rc=%d\n", rc);
            FILE *f = fopen("/data/local/tmp/xclipse940/compute_copy_parent.log", "w");
            if (f) { fprintf(f, "child exited rc=%d\n", rc); fflush(f); fsync(fileno(f)); fclose(f); }
            return rc;
         }

         if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("\nwatchdog: child died from signal %d\n", sig);
            FILE *f = fopen("/data/local/tmp/xclipse940/compute_copy_parent.log", "w");
            if (f) { fprintf(f, "child died signal=%d\n", sig); fflush(f); fsync(fileno(f)); fclose(f); }
            return 128 + sig;
         }

         return 101;
      }

      if (r < 0) {
         fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
         return 102;
      }

      if (monotonic_seconds() >= deadline) {
         fprintf(stderr,
                 "\nWATCHDOG TIMEOUT: probe exceeded %d seconds.\n"
                 "Killing userspace child %d and returning.\n",
                 watchdog_sec, (int)child);
         FILE *f = fopen("/data/local/tmp/xclipse940/compute_copy_parent.log", "w");
         if (f) { fprintf(f, "watchdog timeout child=%d\n", (int)child); fflush(f); fsync(fileno(f)); fclose(f); }
         kill(child, SIGKILL);
         return 124;
      }

      usleep(100000);
   }
}
