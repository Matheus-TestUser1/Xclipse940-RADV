/*
 * Xclipse 940 RADV - minimal compute dispatch diagnostic
 *
 * Tests on graphics+compute queue (qf with GRAPHICS|COMPUTE):
 *   HAL -> Instance -> Device -> Queue
 *   -> create trivial compute shader module
 *   -> create empty pipeline layout
 *   -> create compute pipeline
 *   -> record vkCmdBindPipeline + vkCmdDispatch(1,1,1)
 *   -> submit + finite fence wait
 *
 * Shader does NOTHING:
 *   layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
 *   void main() {}
 *
 * No descriptors, buffers, images, push constants, global loads/stores.
 *
 * Safety:
 *   - exactly one workgroup / one invocation
 *   - no memory access from shader
 *   - 1 s fence timeout by default
 *   - userspace child watchdog
 *   - persistent fsync() diagnostic log
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
   case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
   case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
   default: return "VkResult(?)";
   }
}

/*
 * SPIR-V for:
 *
 * #version 450
 * layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
 * void main() {}
 *
 * Minimal SPIR-V 1.0 module.
 */
static const uint32_t trivial_compute_spv[] = {
   0x07230203, /* Magic */
   0x00010000, /* SPIR-V 1.0 */
   0x00000000, /* Generator */
   0x00000005, /* Bound */
   0x00000000, /* Schema */

   0x00020011, 0x00000001,                         /* OpCapability Shader */
   0x0003000e, 0x00000000, 0x00000001,             /* OpMemoryModel Logical GLSL450 */
   0x0005000f, 0x00000005, 0x00000001,
               0x6e69616d, 0x00000000,             /* OpEntryPoint GLCompute %1 "main" */
   0x00060010, 0x00000001, 0x00000011,
               0x00000001, 0x00000001, 0x00000001,/* OpExecutionMode %1 LocalSize 1 1 1 */
   0x00030003, 0x00000002, 0x000001c2,             /* OpSource GLSL 450 */
   0x00040005, 0x00000001, 0x6e69616d, 0x00000000,/* OpName %1 "main" */
   0x00020013, 0x00000002,                         /* %2 = OpTypeVoid */
   0x00030021, 0x00000003, 0x00000002,             /* %3 = OpTypeFunction %2 */
   0x00050036, 0x00000002, 0x00000001,
               0x00000000, 0x00000003,             /* %1 = OpFunction %2 None %3 */
   0x000200f8, 0x00000004,                         /* %4 = OpLabel */
   0x000100fd,                                     /* OpReturn */
   0x00010038,                                     /* OpFunctionEnd */
};

static int
run_child(const char *driver_path, uint64_t fence_timeout_ns)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   probe_log = fopen("/data/local/tmp/xclipse940/min_compute_probe.log", "w");
   if (probe_log) {
      setvbuf(probe_log, NULL, _IONBF, 0);
      persist_mark("START min_compute_probe");
   }

   printf("=== Xclipse 940 RADV minimal compute-dispatch probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("shader: empty main(), local_size=1x1x1\n");
   printf("dispatch: 1x1x1, NO MEMORY ACCESS\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      persist_mark("FAIL dlopen");
      return 10;
   }
   printf("[1/15] dlopen OK\n");

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL: HMI not found\n");
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
   printf("[2/15] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-min-compute-probe",
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
   printf("[3/15] vkCreateInstance = %d (%s)\n", vr, vk_result_name(vr));
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

   printf("[4/15] GPU: %s Vulkan=%u.%u.%u\n",
          props.deviceName,
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

      /* Mirror the meta-copy case: graphics queue that also supports compute. */
      if (selected_qf == UINT32_MAX &&
          qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
         selected_qf = i;
   }

   if (selected_qf == UINT32_MAX) {
      fprintf(stderr, "FAIL: no GRAPHICS|COMPUTE queue family\n");
      persist_mark("FAIL no graphics+compute qf");
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
   printf("[5/15] vkCreateDevice = %d (%s)\n", vr, vk_result_name(vr));
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
   GET_DEV(vkCreateShaderModule);
   GET_DEV(vkDestroyShaderModule);
   GET_DEV(vkCreatePipelineLayout);
   GET_DEV(vkDestroyPipelineLayout);
   GET_DEV(vkCreateComputePipelines);
   GET_DEV(vkDestroyPipeline);
   GET_DEV(vkCreateCommandPool);
   GET_DEV(vkDestroyCommandPool);
   GET_DEV(vkAllocateCommandBuffers);
   GET_DEV(vkBeginCommandBuffer);
   GET_DEV(vkCmdBindPipeline);
   GET_DEV(vkCmdDispatch);
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

   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(trivial_compute_spv),
      .pCode = trivial_compute_spv,
   };

   VkShaderModule shader = VK_NULL_HANDLE;
   persist_mark("before vkCreateShaderModule");
   vr = vkCreateShaderModule(device, &smci, NULL, &shader);
   printf("[7/15] vkCreateShaderModule = %d (%s), shader=%p\n",
          vr, vk_result_name(vr), (void *)shader);
   persist_mark("after vkCreateShaderModule result=%d", vr);
   if (vr != VK_SUCCESS)
      return 40;

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 0,
      .pushConstantRangeCount = 0,
   };

   VkPipelineLayout layout = VK_NULL_HANDLE;
   vr = vkCreatePipelineLayout(device, &plci, NULL, &layout);
   printf("[8/15] vkCreatePipelineLayout = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("vkCreatePipelineLayout result=%d", vr);
   if (vr != VK_SUCCESS)
      return 41;

   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader,
      .pName = "main",
   };

   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage,
      .layout = layout,
   };

   VkPipeline pipeline = VK_NULL_HANDLE;
   printf("[9/15] vkCreateComputePipelines ...\n");
   persist_mark("before vkCreateComputePipelines");
   vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);
   printf("       result = %d (%s), pipeline=%p\n",
          vr, vk_result_name(vr), (void *)pipeline);
   persist_mark("after vkCreateComputePipelines result=%d", vr);
   if (vr != VK_SUCCESS)
      return 42;

   VkCommandPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };

   VkCommandPool pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &pool_ci, NULL, &pool);
   printf("[10/15] vkCreateCommandPool = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };

   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cbai, &cb);
   if (vr != VK_SUCCESS)
      return 44;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };

   vr = vkBeginCommandBuffer(cb, &cbbi);
   if (vr != VK_SUCCESS)
      return 45;

   persist_mark("before vkCmdBindPipeline");
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   persist_mark("after vkCmdBindPipeline");

   printf("[11/15] recording vkCmdDispatch(1,1,1) ...\n");
   persist_mark("before vkCmdDispatch 1 1 1");
   vkCmdDispatch(cb, 1, 1, 1);
   persist_mark("after vkCmdDispatch 1 1 1");

   vr = vkEndCommandBuffer(cb);
   printf("       vkEndCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkEndCommandBuffer result=%d", vr);
   if (vr != VK_SUCCESS)
      return 46;

   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };

   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fci, NULL, &fence);
   if (vr != VK_SUCCESS)
      return 47;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   printf("[12/15] vkQueueSubmit(dispatch) ...\n");
   persist_mark("before vkQueueSubmit");
   vr = vkQueueSubmit(queue, 1, &si, fence);
   printf("       result = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkQueueSubmit result=%d", vr);
   if (vr != VK_SUCCESS)
      return 50;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("[13/15] immediate fence = %d (%s)\n",
          fs, vk_result_name(fs));
   persist_mark("immediate fence=%d", fs);

   printf("[14/15] vkWaitForFences(timeout=%.3f s) ...\n",
          (double)fence_timeout_ns / 1000000000.0);
   persist_mark("before vkWaitForFences");

   vr = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout_ns);
   printf("       result = %d (%s)\n", vr, vk_result_name(vr));
   persist_mark("after vkWaitForFences result=%d", vr);

   if (vr == VK_TIMEOUT) {
      fprintf(stderr,
              "\nTIMEOUT: minimal compute dispatch did not finish.\n"
              "No memory was accessed by the shader.\n");
      persist_mark("TIMEOUT");
      return 60;
   }

   if (vr != VK_SUCCESS) {
      persist_mark("FAIL wait result=%d", vr);
      return 61;
   }

   fs = vkGetFenceStatus(device, fence);
   printf("[15/15] final fence = %d (%s)\n",
          fs, vk_result_name(fs));

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, shader, NULL);

   printf("\nPASS: trivial compute shader dispatch completed.\n");
   printf("NO BUFFER/IMAGE/GLOBAL MEMORY ACCESS WAS USED.\n");
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
         FILE *f = fopen("/data/local/tmp/xclipse940/min_compute_parent.log", "w");

         if (WIFEXITED(status)) {
            int rc = WEXITSTATUS(status);
            printf("\nwatchdog: child exited normally, rc=%d\n", rc);
            if (f) {
               fprintf(f, "child exited rc=%d\n", rc);
               fflush(f); fsync(fileno(f)); fclose(f);
            }
            return rc;
         }

         if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("\nwatchdog: child died from signal %d\n", sig);
            if (f) {
               fprintf(f, "child died signal=%d\n", sig);
               fflush(f); fsync(fileno(f)); fclose(f);
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

         FILE *f = fopen("/data/local/tmp/xclipse940/min_compute_parent.log", "w");
         if (f) {
            fprintf(f, "watchdog timeout child=%d\n", (int)child);
            fflush(f); fsync(fileno(f)); fclose(f);
         }

         kill(child, SIGKILL);
         return 124;
      }

      usleep(100000);
   }
}
