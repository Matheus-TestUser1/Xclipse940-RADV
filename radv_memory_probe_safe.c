/*
 * Xclipse 940 RADV - safe buffer/memory probe
 *
 * Tests:
 *   HAL -> VkInstance -> VkPhysicalDevice -> VkDevice -> VkQueue
 *   -> vkCreateBuffer
 *   -> vkGetBufferMemoryRequirements
 *   -> vkAllocateMemory
 *   -> vkBindBufferMemory
 *   -> vkMapMemory (only if HOST_VISIBLE)
 *   -> CPU write/read verification
 *
 * Deliberately NOT done:
 *   - no command buffers
 *   - no vkQueueSubmit
 *   - no shaders
 *   - no vkDeviceWaitIdle / vkQueueWaitIdle
 *
 * The test runs in a child process. Parent watchdog kills the child after
 * PROBE_TIMEOUT_SEC (default 10 s) if userspace gets stuck.
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

static void
print_mem_flags(VkMemoryPropertyFlags f)
{
   int first = 1;
#define MFLAG(bit, name) do { \
   if (f & (bit)) { \
      printf("%s%s", first ? "" : "|", (name)); \
      first = 0; \
   } \
} while (0)

   MFLAG(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL");
   MFLAG(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE");
   MFLAG(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT");
   MFLAG(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED");
   MFLAG(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZY");
#ifdef VK_MEMORY_PROPERTY_PROTECTED_BIT
   MFLAG(VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED");
#endif
   if (first)
      printf("0");
#undef MFLAG
}

static int
pick_memory_type(uint32_t bits,
                 const VkPhysicalDeviceMemoryProperties *mp,
                 uint32_t *out_index,
                 int *out_host_visible)
{
   /* First choice: memory we can safely CPU-map and verify. */
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if (!(bits & (1u << i)))
         continue;

      VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
      if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
         *out_index = i;
         *out_host_visible = 1;
         return 1;
      }
   }

   /* Second choice: any HOST_VISIBLE type. */
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if (!(bits & (1u << i)))
         continue;

      VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
      if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         *out_index = i;
         *out_host_visible = 1;
         return 1;
      }
   }

   /* Last choice: any compatible type. We only allocate+bind; no map. */
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
      if (bits & (1u << i)) {
         *out_index = i;
         *out_host_visible =
            !!(mp->memoryTypes[i].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
         return 1;
      }
   }

   return 0;
}

static int
run_child(const char *driver_path)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   printf("=== Xclipse 940 RADV safe memory probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("policy: NO COMMAND BUFFER, NO SUBMIT, NO WAIT-IDLE\n\n");

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      return 10;
   }
   printf("[1/10] dlopen OK\n");

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
   printf("[2/10] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-safe-memory-probe",
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
   printf("[3/10] vkCreateInstance = %d (%s)\n",
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
   GET_INST(vkGetPhysicalDeviceMemoryProperties);
   GET_INST(vkGetPhysicalDeviceQueueFamilyProperties);
   GET_INST(vkCreateDevice);
   GET_INST(vkGetDeviceProcAddr);

   uint32_t phys_count = 0;
   vr = vkEnumeratePhysicalDevices(instance, &phys_count, NULL);
   if (vr != VK_SUCCESS || phys_count == 0) {
      fprintf(stderr, "FAIL: physical-device count = %u, result=%d\n",
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

   printf("[4/10] GPU: %s, vendor=0x%04x device=0x%08x Vulkan=%u.%u.%u\n",
          props.deviceName, props.vendorID, props.deviceID,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

   VkPhysicalDeviceMemoryProperties mp;
   memset(&mp, 0, sizeof(mp));
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);

   printf("       heaps=%u memoryTypes=%u\n",
          mp.memoryHeapCount, mp.memoryTypeCount);

   for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
      printf("       heap[%u]: %llu MiB flags=0x%x\n",
             i,
             (unsigned long long)(mp.memoryHeaps[i].size / (1024ull * 1024ull)),
             mp.memoryHeaps[i].flags);
   }

   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      printf("       type[%u]: heap=%u flags=",
             i, mp.memoryTypes[i].heapIndex);
      print_mem_flags(mp.memoryTypes[i].propertyFlags);
      printf("\n");
   }

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   if (qf_count == 0)
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
   printf("[5/10] vkCreateDevice = %d (%s), device=%p\n",
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
   GET_DEV(vkCreateBuffer);
   GET_DEV(vkDestroyBuffer);
   GET_DEV(vkGetBufferMemoryRequirements);
   GET_DEV(vkAllocateMemory);
   GET_DEV(vkFreeMemory);
   GET_DEV(vkBindBufferMemory);
   GET_DEV(vkMapMemory);
   GET_DEV(vkUnmapMemory);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   printf("[6/10] vkGetDeviceQueue = %p\n", (void *)queue);
   if (queue == VK_NULL_HANDLE)
      return 32;

   /*
    * 64 KiB is intentionally small. This tests the SGPU BO/GTT allocation
    * path without putting meaningful pressure on memory.
    */
   const VkDeviceSize requested_size = 64 * 1024;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = requested_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkBuffer buffer = VK_NULL_HANDLE;
   vr = vkCreateBuffer(device, &bci, NULL, &buffer);
   printf("[7/10] vkCreateBuffer(64 KiB) = %d (%s), buffer=%p\n",
          vr, vk_result_name(vr), (void *)buffer);
   if (vr != VK_SUCCESS)
      return 40;

   VkMemoryRequirements req;
   memset(&req, 0, sizeof(req));
   vkGetBufferMemoryRequirements(device, buffer, &req);

   printf("       requirements: size=%llu alignment=%llu memoryTypeBits=0x%08x\n",
          (unsigned long long)req.size,
          (unsigned long long)req.alignment,
          req.memoryTypeBits);

   uint32_t memory_type = UINT32_MAX;
   int host_visible = 0;
   if (!pick_memory_type(req.memoryTypeBits, &mp,
                         &memory_type, &host_visible)) {
      fprintf(stderr, "FAIL: no compatible memory type\n");
      return 41;
   }

   printf("       selected type[%u]: heap=%u flags=",
          memory_type, mp.memoryTypes[memory_type].heapIndex);
   print_mem_flags(mp.memoryTypes[memory_type].propertyFlags);
   printf("\n");

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type,
   };

   VkDeviceMemory memory = VK_NULL_HANDLE;
   vr = vkAllocateMemory(device, &mai, NULL, &memory);
   printf("[8/10] vkAllocateMemory(%llu bytes) = %d (%s), memory=%p\n",
          (unsigned long long)req.size,
          vr, vk_result_name(vr), (void *)memory);
   if (vr != VK_SUCCESS)
      return 42;

   vr = vkBindBufferMemory(device, buffer, memory, 0);
   printf("[9/10] vkBindBufferMemory = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   if (host_visible) {
      void *map = NULL;
      vr = vkMapMemory(device, memory, 0, req.size, 0, &map);
      printf("[10/10] vkMapMemory = %d (%s), ptr=%p\n",
             vr, vk_result_name(vr), map);
      if (vr != VK_SUCCESS || !map)
         return 44;

      /*
       * CPU-only roundtrip. No GPU commands are involved.
       * Touch only 256 bytes.
       */
      uint8_t *p = (uint8_t *)map;
      for (unsigned i = 0; i < 256; i++)
         p[i] = (uint8_t)(i ^ 0x5a);

      unsigned bad = 0;
      for (unsigned i = 0; i < 256; i++) {
         if (p[i] != (uint8_t)(i ^ 0x5a)) {
            bad++;
            if (bad <= 4)
               printf("       mismatch at %u: got=0x%02x expected=0x%02x\n",
                      i, p[i], (uint8_t)(i ^ 0x5a));
         }
      }

      printf("       CPU write/read verification: %s\n",
             bad ? "FAIL" : "PASS");

      vkUnmapMemory(device, memory);

      if (bad)
         return 45;
   } else {
      printf("[10/10] selected memory is not HOST_VISIBLE; map test skipped.\n");
   }

   /*
    * No submissions exist, so these resource destroys do not require an idle
    * wait. The watchdog is still active if a driver bug causes a hang.
    */
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);

   printf("\nPASS: buffer allocation + bind");
   if (host_visible)
      printf(" + map/write/read");
   printf(" succeeded.\n");
   printf("NO GPU COMMANDS WERE SUBMITTED.\n");

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
         return 124;
      }

      usleep(100000);
   }
}
