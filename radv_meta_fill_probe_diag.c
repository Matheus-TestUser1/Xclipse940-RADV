/*
 * Xclipse 940 RADV - 4096-byte meta compute fill diagnostic
 *
 * Purpose:
 *   Test RADV's compute/meta buffer fill path at exactly 4096 bytes.
 *
 * Why:
 *   - < 4096 bytes: RADV normally uses CP-DMA for buffer ops.
 *   - >= 4096 bytes: RADV_BUFFER_OPS_CS_THRESHOLD selects compute meta shader.
 *
 * This probe:
 *   - creates one 4096-byte HOST_VISIBLE|HOST_COHERENT buffer
 *   - CPU clears it to zero
 *   - records vkCmdFillBuffer(buffer, 0, 4096, 0xA5A5A5A5)
 *   - submits on GRAPHICS|COMPUTE queue family
 *   - waits with a finite fence timeout
 *   - verifies all 4096 bytes from CPU
 *
 * No custom user shader is used. vkCmdFillBuffer exercises RADV's own
 * meta fill shader, which performs global stores.
 *
 * Safety:
 *   - 4 KiB only
 *   - finite 1 s fence wait by default
 *   - userspace child watchdog
 *   - persistent fsync() diagnostic logs
 *
 * A userspace watchdog cannot guarantee recovery from a kernel/GPU lockup.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
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
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   default: return "VkResult(?)";
   }
}

static uint32_t
find_host_coherent_type(uint32_t bits,
                        const VkPhysicalDeviceMemoryProperties *mp)
{
   const VkMemoryPropertyFlags need =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if (!(bits & (1u << i)))
         continue;

      VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
      if ((f & need) == need)
         return i;
   }

   return UINT32_MAX;
}

static int
run_child(const char *driver_path, uint64_t fence_timeout_ns)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   const VkDeviceSize SIZE = 4096;
   const uint32_t FILL = 0xA5A5A5A5u;

   probe_log = fopen("/data/local/tmp/xclipse940/meta_fill_probe.log", "w");
   if (probe_log) {
      setvbuf(probe_log, NULL, _IONBF, 0);
      persist_mark("START meta_fill_probe size=4096 value=0x%08x", FILL);
   }

   printf("=== Xclipse 940 RADV 4096-byte meta-fill probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("GPU work: vkCmdFillBuffer(4096, 0x%08x)\n", FILL);
   printf("expected path: RADV meta COMPUTE fill shader\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      persist_mark("FAIL dlopen");
      return 10;
   }
   printf("[1/16] dlopen OK\n");

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL: HMI missing\n");
      persist_mark("FAIL HMI");
      return 11;
   }

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr != 0 || !base_dev) {
      fprintf(stderr, "FAIL: HAL open=%d\n", hr);
      persist_mark("FAIL HAL open=%d", hr);
      return 12;
   }

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;
   printf("[2/16] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-meta-fill-probe",
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
   printf("[3/16] vkCreateInstance = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("vkCreateInstance result=%d", vr);
   if (vr != VK_SUCCESS)
      return 20;

#define GET_INST(name) \
   PFN_##name name = (PFN_##name)hal->GetInstanceProcAddr(instance, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetInstanceProcAddr(%s)=NULL\n", #name); \
      persist_mark("FAIL missing %s", #name); \
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
   if (vr != VK_SUCCESS || !phys_count)
      return 22;

   VkPhysicalDevice *phys = calloc(phys_count, sizeof(*phys));
   if (!phys)
      return 23;

   vr = vkEnumeratePhysicalDevices(instance, &phys_count, phys);
   if (vr != VK_SUCCESS || !phys_count)
      return 24;

   VkPhysicalDevice pdev = phys[0];

   VkPhysicalDeviceProperties props;
   memset(&props, 0, sizeof(props));
   vkGetPhysicalDeviceProperties(pdev, &props);

   printf("[4/16] GPU: %s Vulkan=%u.%u.%u\n",
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
      printf("       qf[%u]: queues=%u flags=0x%x\n",
             i, qf[i].queueCount, qf[i].queueFlags);

      if (selected_qf == UINT32_MAX &&
          qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
         selected_qf = i;
   }

   if (selected_qf == UINT32_MAX) {
      fprintf(stderr, "FAIL: no GRAPHICS|COMPUTE queue\n");
      return 27;
   }

   printf("       selected GRAPHICS|COMPUTE qf=%u\n", selected_qf);
   persist_mark("selected qf=%u", selected_qf);

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
   printf("[5/16] vkCreateDevice = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("vkCreateDevice result=%d", vr);
   if (vr != VK_SUCCESS)
      return 30;

#define GET_DEV(name) \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetDeviceProcAddr(%s)=NULL\n", #name); \
      persist_mark("FAIL missing %s", #name); \
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
   GET_DEV(vkCmdFillBuffer);
   GET_DEV(vkEndCommandBuffer);
   GET_DEV(vkCreateFence);
   GET_DEV(vkDestroyFence);
   GET_DEV(vkQueueSubmit);
   GET_DEV(vkWaitForFences);
   GET_DEV(vkGetFenceStatus);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   printf("[6/16] vkGetDeviceQueue = %p\n", (void *)queue);
   if (!queue)
      return 32;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = SIZE,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkBuffer buffer = VK_NULL_HANDLE;
   vr = vkCreateBuffer(device, &bci, NULL, &buffer);
   printf("[7/16] vkCreateBuffer = %d (%s), buffer=%p\n",
          vr, vk_result_name(vr), (void *)buffer);
   if (vr != VK_SUCCESS)
      return 40;

   VkMemoryRequirements req;
   memset(&req, 0, sizeof(req));
   vkGetBufferMemoryRequirements(device, buffer, &req);

   printf("       req.size=%llu align=%llu bits=0x%08x\n",
          (unsigned long long)req.size,
          (unsigned long long)req.alignment,
          req.memoryTypeBits);

   uint32_t mt = find_host_coherent_type(req.memoryTypeBits, &mp);
   if (mt == UINT32_MAX) {
      fprintf(stderr, "FAIL: no HOST_VISIBLE|HOST_COHERENT type\n");
      return 41;
   }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = mt,
   };

   VkDeviceMemory memory = VK_NULL_HANDLE;
   vr = vkAllocateMemory(device, &mai, NULL, &memory);
   printf("[8/16] vkAllocateMemory = %d (%s), memory=%p type=%u\n",
          vr, vk_result_name(vr), (void *)memory, mt);
   if (vr != VK_SUCCESS)
      return 42;

   vr = vkBindBufferMemory(device, buffer, memory, 0);
   printf("[9/16] vkBindBufferMemory = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   void *map = NULL;
   vr = vkMapMemory(device, memory, 0, req.size, 0, &map);
   printf("[10/16] vkMapMemory = %d (%s), ptr=%p\n",
          vr, vk_result_name(vr), map);
   if (vr != VK_SUCCESS || !map)
      return 44;

   memset(map, 0, SIZE);
   persist_mark("buffer allocated mapped and cleared");

   VkCommandPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };

   VkCommandPool pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &pool_ci, NULL, &pool);
   printf("[11/16] vkCreateCommandPool = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 45;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };

   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cbai, &cb);
   if (vr != VK_SUCCESS)
      return 46;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };

   vr = vkBeginCommandBuffer(cb, &cbbi);
   if (vr != VK_SUCCESS)
      return 47;

   printf("[12/16] recording vkCmdFillBuffer(4096, 0x%08x)...\n", FILL);
   persist_mark("before vkCmdFillBuffer");

   vkCmdFillBuffer(cb, buffer, 0, SIZE, FILL);

   persist_mark("after vkCmdFillBuffer");

   vr = vkEndCommandBuffer(cb);
   printf("        vkEndCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkEndCommandBuffer result=%d", vr);
   if (vr != VK_SUCCESS)
      return 48;

   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };

   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fci, NULL, &fence);
   if (vr != VK_SUCCESS)
      return 49;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   printf("[13/16] vkQueueSubmit(fill)...\n");
   persist_mark("before vkQueueSubmit");

   vr = vkQueueSubmit(queue, 1, &si, fence);

   printf("        submit=%d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkQueueSubmit result=%d", vr);
   if (vr != VK_SUCCESS)
      return 50;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("[14/16] immediate fence=%d (%s)\n",
          fs, vk_result_name(fs));
   persist_mark("immediate fence=%d", fs);

   printf("[15/16] vkWaitForFences(timeout=%.3f s)...\n",
          (double)fence_timeout_ns / 1000000000.0);
   persist_mark("before vkWaitForFences");

   vr = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout_ns);

   printf("        wait=%d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkWaitForFences result=%d", vr);

   if (vr == VK_TIMEOUT) {
      fprintf(stderr,
              "\nTIMEOUT: 4096-byte meta compute fill did not finish.\n");
      persist_mark("TIMEOUT");
      return 60;
   }

   if (vr != VK_SUCCESS)
      return 61;

   printf("[16/16] verifying 4096 bytes...\n");

   const uint8_t expected = 0xA5;
   uint32_t bad = 0;

   for (uint32_t i = 0; i < SIZE; i++) {
      uint8_t got = ((uint8_t *)map)[i];
      if (got != expected) {
         bad++;
         if (bad <= 8)
            printf("        mismatch[%u]: got=0x%02x expected=0x%02x\n",
                   i, got, expected);
      }
   }

   if (bad) {
      printf("\nFAIL: %u / 4096 bytes mismatched\n", bad);
      persist_mark("verification FAIL bad=%u", bad);
      return 70;
   }

   printf("        verification PASS: all 4096 bytes are 0xA5\n");

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);

   printf("\nPASS: RADV meta compute fill completed and data matched.\n");
   persist_mark("PASS complete");
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

   int watchdog_sec = 8;
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
         FILE *f =
            fopen("/data/local/tmp/xclipse940/meta_fill_parent.log", "w");

         if (WIFEXITED(status)) {
            int rc = WEXITSTATUS(status);
            printf("\nwatchdog: child exited normally, rc=%d\n", rc);
            if (f) {
               fprintf(f, "child exited rc=%d\n", rc);
               fflush(f);
               fsync(fileno(f));
               fclose(f);
            }
            return rc;
         }

         if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("\nwatchdog: child died from signal %d\n", sig);
            if (f) {
               fprintf(f, "child died signal=%d\n", sig);
               fflush(f);
               fsync(fileno(f));
               fclose(f);
            }
            return 128 + sig;
         }

         if (f)
            fclose(f);
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

         FILE *f =
            fopen("/data/local/tmp/xclipse940/meta_fill_parent.log", "w");
         if (f) {
            fprintf(f, "watchdog timeout child=%d\n", (int)child);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
         }

         kill(child, SIGKILL);
         return 124;
      }

      usleep(100000);
   }
}
