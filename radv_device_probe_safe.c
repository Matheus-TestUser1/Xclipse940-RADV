/*
 * Xclipse 940 RADV - conservative logical-device probe
 *
 * Purpose:
 *   - Load the Mesa Android Vulkan HAL directly
 *   - Create VkInstance
 *   - Enumerate one VkPhysicalDevice
 *   - Print queue families
 *   - Create ONE VkDevice with ONE queue
 *   - Fetch that VkQueue
 *
 * Deliberately NOT done:
 *   - no vkQueueSubmit
 *   - no command buffers
 *   - no vkDeviceWaitIdle
 *   - no images / buffers / memory allocations requested by this probe
 *
 * The risky section runs in a child process. The parent watchdog terminates
 * the child after PROBE_TIMEOUT_SEC (default 10 seconds).
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

static void
print_queue_flags(VkQueueFlags f)
{
   int first = 1;
#define QFLAG(bit, name) do { \
   if (f & (bit)) { \
      printf("%s%s", first ? "" : "|", (name)); \
      first = 0; \
   } \
} while (0)

   QFLAG(VK_QUEUE_GRAPHICS_BIT, "GRAPHICS");
   QFLAG(VK_QUEUE_COMPUTE_BIT, "COMPUTE");
   QFLAG(VK_QUEUE_TRANSFER_BIT, "TRANSFER");
   QFLAG(VK_QUEUE_SPARSE_BINDING_BIT, "SPARSE");
#ifdef VK_QUEUE_PROTECTED_BIT
   QFLAG(VK_QUEUE_PROTECTED_BIT, "PROTECTED");
#endif
#ifdef VK_QUEUE_VIDEO_DECODE_BIT_KHR
   QFLAG(VK_QUEUE_VIDEO_DECODE_BIT_KHR, "VIDEO_DECODE");
#endif
#ifdef VK_QUEUE_VIDEO_ENCODE_BIT_KHR
   QFLAG(VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "VIDEO_ENCODE");
#endif
   if (first)
      printf("0");
#undef QFLAG
}

static int
run_child(const char *driver_path)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   printf("=== Xclipse 940 RADV safe device probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("policy: NO SUBMIT, NO WAIT-IDLE\n\n");

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      return 10;
   }
   printf("[1/7] dlopen OK\n");

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL: dlsym(%s): %s\n",
              HAL_MODULE_INFO_SYM_AS_STR, dlerror());
      return 11;
   }
   printf("[2/7] HMI OK: %s\n",
          module->common.name ? module->common.name : "(null)");

   if (!module->common.methods || !module->common.methods->open) {
      fprintf(stderr, "FAIL: HAL open callback missing\n");
      return 12;
   }

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr != 0 || !base_dev) {
      fprintf(stderr, "FAIL: HAL open = %d, dev=%p\n", hr, (void *)base_dev);
      return 13;
   }

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;
   if (!hal->CreateInstance || !hal->GetInstanceProcAddr) {
      fprintf(stderr, "FAIL: Vulkan HAL entrypoints missing\n");
      return 14;
   }
   printf("[3/7] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-safe-device-probe",
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
   printf("[4/7] vkCreateInstance = %d (%s), instance=%p\n",
          vr, vk_result_name(vr), (void *)instance);
   if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE)
      return 20;

#define GET_INST(name) \
   PFN_##name name = (PFN_##name)hal->GetInstanceProcAddr(instance, #name); \
   if (!(name)) { \
      fprintf(stderr, "FAIL: GetInstanceProcAddr(%s) = NULL\n", #name); \
      return 21; \
   }

   GET_INST(vkEnumeratePhysicalDevices);
   GET_INST(vkGetPhysicalDeviceProperties);
   GET_INST(vkGetPhysicalDeviceQueueFamilyProperties);
   GET_INST(vkCreateDevice);
   GET_INST(vkGetDeviceProcAddr);

   uint32_t phys_count = 0;
   vr = vkEnumeratePhysicalDevices(instance, &phys_count, NULL);
   printf("[5/7] vkEnumeratePhysicalDevices = %d (%s), count=%u\n",
          vr, vk_result_name(vr), phys_count);
   if (vr != VK_SUCCESS || phys_count == 0)
      return 22;

   VkPhysicalDevice *phys =
      calloc(phys_count, sizeof(*phys));
   if (!phys)
      return 23;

   vr = vkEnumeratePhysicalDevices(instance, &phys_count, phys);
   if (vr != VK_SUCCESS || phys_count == 0) {
      fprintf(stderr, "FAIL: second physical-device enumeration: %d (%s)\n",
              vr, vk_result_name(vr));
      return 24;
   }

   VkPhysicalDevice pdev = phys[0];
   VkPhysicalDeviceProperties props;
   memset(&props, 0, sizeof(props));
   vkGetPhysicalDeviceProperties(pdev, &props);

   printf("      GPU: %s\n", props.deviceName);
   printf("      vendor=0x%04x device=0x%08x Vulkan=%u.%u.%u\n",
          props.vendorID, props.deviceID,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   printf("      queueFamilyCount=%u\n", qf_count);
   if (qf_count == 0)
      return 25;

   VkQueueFamilyProperties *qf =
      calloc(qf_count, sizeof(*qf));
   if (!qf)
      return 26;

   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qf);

   uint32_t selected = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      printf("      qf[%u]: queues=%u flags=", i, qf[i].queueCount);
      print_queue_flags(qf[i].queueFlags);
      printf("\n");

      if (selected == UINT32_MAX &&
          qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         selected = i;
   }

   if (selected == UINT32_MAX) {
      for (uint32_t i = 0; i < qf_count; i++) {
         if (qf[i].queueCount > 0 &&
             (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            selected = i;
            break;
         }
      }
   }

   if (selected == UINT32_MAX) {
      fprintf(stderr, "FAIL: no usable GRAPHICS/COMPUTE queue family\n");
      return 27;
   }

   printf("      selected queue family = %u\n", selected);
   printf("\n[6/7] entering vkCreateDevice (watchdog active)...\n");

   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = selected,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };

   /*
    * Conservative first test:
    * - zero device extensions
    * - zero optional features
    * - exactly one queue
    */
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledLayerCount = 0,
      .enabledExtensionCount = 0,
      .pEnabledFeatures = NULL,
   };

   VkDevice device = VK_NULL_HANDLE;
   vr = vkCreateDevice(pdev, &dci, NULL, &device);

   printf("      vkCreateDevice = %d (%s), device=%p\n",
          vr, vk_result_name(vr), (void *)device);

   if (vr != VK_SUCCESS || device == VK_NULL_HANDLE)
      return 30;

   PFN_vkGetDeviceQueue vkGetDeviceQueue =
      (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
   if (!vkGetDeviceQueue) {
      fprintf(stderr, "FAIL: vkGetDeviceProcAddr(vkGetDeviceQueue) = NULL\n");
      return 31;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected, 0, &queue);

   printf("[7/7] vkGetDeviceQueue: queue=%p\n", (void *)queue);
   if (queue == VK_NULL_HANDLE) {
      fprintf(stderr, "FAIL: queue is VK_NULL_HANDLE\n");
      return 32;
   }

   /*
    * Deliberately don't submit anything and don't call vkDeviceWaitIdle.
    * For this one-shot probe, process exit lets Android/Linux close the
    * descriptors and reclaim the test resources.
    */
   printf("\nPASS: VkDevice + VkQueue created.\n");
   printf("NO GPU COMMANDS WERE SUBMITTED.\n");
   printf("Exiting child without vkQueueSubmit/vkDeviceWaitIdle.\n");

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

   int timeout_sec = 10;
   const char *env_timeout = getenv("PROBE_TIMEOUT_SEC");
   if (env_timeout && *env_timeout) {
      int t = atoi(env_timeout);
      if (t >= 1 && t <= 30)
         timeout_sec = t;
   }

   printf("Watchdog timeout: %d seconds\n", timeout_sec);

   pid_t child = fork();
   if (child < 0) {
      fprintf(stderr, "fork failed: %s\n", strerror(errno));
      return 100;
   }

   if (child == 0) {
      int rc = run_child(driver_path);
      _exit(rc);
   }

   const double deadline = monotonic_seconds() + timeout_sec;

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
                 "Killing child process %d and returning to shell.\n",
                 timeout_sec, (int)child);
         kill(child, SIGKILL);

         /*
          * Don't block waiting for a task that could theoretically be stuck
          * in an uninterruptible kernel sleep. The parent exits immediately.
          */
         return 124;
      }

      usleep(100000);
   }
}
