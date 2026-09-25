/*
 * Xclipse 940 RADV - dedicated compute-queue empty-submit probe
 *
 * Tests:
 *   HAL -> Instance -> PhysicalDevice -> Device -> Queue
 *   -> CommandPool -> empty primary CommandBuffer
 *   -> vkQueueSubmit(empty CB, fence)
 *   -> vkWaitForFences with finite timeout
 *
 * Deliberately NOT done:
 *   - no shaders
 *   - no draw / dispatch
 *   - no images
 *   - no buffer access by GPU
 *   - no vkQueueWaitIdle
 *   - no vkDeviceWaitIdle
 *
 * Safety:
 *   - fence wait is finite (default 1 second)
 *   - whole risky section runs in a child process
 *   - parent watchdog defaults to 6 seconds
 *
 * NOTE: A userspace watchdog cannot guarantee recovery from a kernel/GPU
 * lockup. This probe minimizes the submitted work but still exercises the
 * real kernel command-submission path.
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

static const char *
vk_result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_EVENT_SET: return "VK_EVENT_SET";
   case VK_EVENT_RESET: return "VK_EVENT_RESET";
   case VK_INCOMPLETE: return "VK_INCOMPLETE";
   case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
   case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
   case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
   default: return "VkResult(?)";
   }
}

static int
run_child(const char *driver_path, uint64_t fence_timeout_ns)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   printf("=== Xclipse 940 RADV dedicated compute-queue empty-submit probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("GPU work: EMPTY COMMAND BUFFER ONLY on dedicated COMPUTE queue\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      return 10;
   }
   printf("[1/12] dlopen OK\n");

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL: dlsym(%s): %s\n",
              HAL_MODULE_INFO_SYM_AS_STR, dlerror());
      return 11;
   }

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr != 0 || !base_dev) {
      fprintf(stderr, "FAIL: HAL open = %d\n", hr);
      return 12;
   }

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;
   printf("[2/12] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-empty-submit-probe",
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
   printf("[3/12] vkCreateInstance = %d (%s)\n",
          vr, vk_result_name(vr));
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
   GET_INST(vkGetPhysicalDeviceQueueFamilyProperties);
   GET_INST(vkCreateDevice);
   GET_INST(vkGetDeviceProcAddr);

   uint32_t phys_count = 0;
   vr = vkEnumeratePhysicalDevices(instance, &phys_count, NULL);
   if (vr != VK_SUCCESS || phys_count == 0) {
      fprintf(stderr, "FAIL: physical-device count=%u result=%d\n",
              phys_count, vr);
      return 22;
   }

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

   printf("[4/12] GPU: %s, vendor=0x%04x device=0x%08x Vulkan=%u.%u.%u\n",
          props.deviceName, props.vendorID, props.deviceID,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

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

      /* Prefer a dedicated compute family: COMPUTE set, GRAPHICS clear. */
      if (selected_qf == UINT32_MAX &&
          qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
          !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         selected_qf = i;
   }

   if (selected_qf == UINT32_MAX) {
      fprintf(stderr, "FAIL: no dedicated compute queue family\n");
      return 27;
   }

   printf("       selected dedicated compute queue family = %u\n", selected_qf);

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
   printf("[5/12] vkCreateDevice = %d (%s), device=%p\n",
          vr, vk_result_name(vr), (void *)device);
   if (vr != VK_SUCCESS)
      return 30;

#define GET_DEV(name) \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetDeviceProcAddr(%s)=NULL\n", #name); \
      return 31; \
   }

   GET_DEV(vkGetDeviceQueue);
   GET_DEV(vkCreateCommandPool);
   GET_DEV(vkDestroyCommandPool);
   GET_DEV(vkAllocateCommandBuffers);
   GET_DEV(vkBeginCommandBuffer);
   GET_DEV(vkEndCommandBuffer);
   GET_DEV(vkCreateFence);
   GET_DEV(vkDestroyFence);
   GET_DEV(vkQueueSubmit);
   GET_DEV(vkWaitForFences);
   GET_DEV(vkGetFenceStatus);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   printf("[6/12] vkGetDeviceQueue = %p\n", (void *)queue);
   if (!queue)
      return 32;

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };

   VkCommandPool pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &cpci, NULL, &pool);
   printf("[7/12] vkCreateCommandPool = %d (%s), pool=%p\n",
          vr, vk_result_name(vr), (void *)pool);
   if (vr != VK_SUCCESS)
      return 40;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };

   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cbai, &cb);
   printf("[8/12] vkAllocateCommandBuffers = %d (%s), cb=%p\n",
          vr, vk_result_name(vr), (void *)cb);
   if (vr != VK_SUCCESS)
      return 41;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };

   vr = vkBeginCommandBuffer(cb, &cbbi);
   printf("[9/12] vkBeginCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 42;

   /* Intentionally record zero commands. */

   vr = vkEndCommandBuffer(cb);
   printf("[10/12] vkEndCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = 0,
   };

   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fci, NULL, &fence);
   if (vr != VK_SUCCESS) {
      fprintf(stderr, "FAIL: vkCreateFence = %d (%s)\n",
              vr, vk_result_name(vr));
      return 44;
   }

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   printf("[11/12] vkQueueSubmit(empty CB) ...\n");
   vr = vkQueueSubmit(queue, 1, &si, fence);
   printf("        vkQueueSubmit = %d (%s)\n",
          vr, vk_result_name(vr));

   if (vr != VK_SUCCESS)
      return 50;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("        immediate vkGetFenceStatus = %d (%s)\n",
          fs, vk_result_name(fs));

   printf("[12/12] vkWaitForFences(timeout=%.3f s) ...\n",
          (double)fence_timeout_ns / 1000000000.0);

   vr = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout_ns);
   printf("        vkWaitForFences = %d (%s)\n",
          vr, vk_result_name(vr));

   if (vr == VK_TIMEOUT) {
      fprintf(stderr,
              "\nTIMEOUT: submit did not signal the fence in time.\n"
              "Exiting immediately; no idle wait and no teardown calls.\n");
      return 60;
   }

   if (vr != VK_SUCCESS) {
      fprintf(stderr,
              "\nFAIL: fence wait returned %d (%s).\n",
              vr, vk_result_name(vr));
      return 61;
   }

   fs = vkGetFenceStatus(device, fence);
   printf("        final vkGetFenceStatus = %d (%s)\n",
          fs, vk_result_name(fs));

   /*
    * Submission completed, so it is safe to destroy the fence/pool without
    * queue/device idle calls. Destroying the pool releases the command buffer.
    */
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);

   printf("\nPASS: empty command buffer on dedicated compute queue submitted and fence signaled.\n");
   printf("NO SHADER/DRAW/DISPATCH/MEMORY ACCESS WAS SUBMITTED.\n");

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

   uint64_t fence_timeout_ns = 1000000000ull; /* 1 second */
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
            return rc;
         }

         if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("\nwatchdog: child died from signal %d\n", sig);
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
         kill(child, SIGKILL);
         return 124;
      }

      usleep(100000);
   }
}
