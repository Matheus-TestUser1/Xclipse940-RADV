/*
 * Xclipse 940 RADV - direct Buffer Device Address/global-store diagnostic.
 *
 * This intentionally avoids Vulkan descriptor sets and AMD raw buffer
 * descriptors. The compute shader receives the 64-bit VkDeviceAddress through
 * push constants and performs one 32-bit store through a
 * PhysicalStorageBuffer pointer.
 *
 * GPU work: exactly one compute invocation and one uint32_t store.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
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

/*
 * SPIR-V 1.3:
 *
 * layout(push_constant) uniform PC { uint64_t address; } pc;
 * void main() {
 *    *(PhysicalStorageBuffer uint *)pc.address = 0x12345678u;
 * }
 *
 * The store has an explicit 4-byte Aligned memory operand.
 */
static const uint32_t bda_store_spv[] = {
   0x07230203,
   0x00010300,
   0x00000000,
   0x00000011,
   0x00000000,
   0x00020011,
   0x00000001,
   0x00020011,
   0x0000000b,
   0x00020011,
   0x000014e3,
   0x0009000a,
   0x5f565053,
   0x5f52484b,
   0x73796870,
   0x6c616369,
   0x6f74735f,
   0x65676172,
   0x6675625f,
   0x00726566,
   0x0003000e,
   0x000014e4,
   0x00000001,
   0x0005000f,
   0x00000005,
   0x00000001,
   0x6e69616d,
   0x00000000,
   0x00060010,
   0x00000001,
   0x00000011,
   0x00000001,
   0x00000001,
   0x00000001,
   0x00030047,
   0x00000006,
   0x00000002,
   0x00050048,
   0x00000006,
   0x00000000,
   0x00000023,
   0x00000000,
   0x00020013,
   0x00000002,
   0x00030021,
   0x00000003,
   0x00000002,
   0x00040015,
   0x00000004,
   0x00000020,
   0x00000000,
   0x00040015,
   0x00000005,
   0x00000040,
   0x00000000,
   0x0003001e,
   0x00000006,
   0x00000005,
   0x00040020,
   0x00000007,
   0x00000009,
   0x00000006,
   0x0004003b,
   0x00000007,
   0x00000008,
   0x00000009,
   0x0004002b,
   0x00000004,
   0x00000009,
   0x00000000,
   0x00040020,
   0x0000000a,
   0x00000009,
   0x00000005,
   0x00040020,
   0x0000000b,
   0x000014e5,
   0x00000004,
   0x0004002b,
   0x00000004,
   0x0000000c,
   0x12345678,
   0x00050036,
   0x00000002,
   0x00000001,
   0x00000000,
   0x00000003,
   0x000200f8,
   0x0000000d,
   0x00050041,
   0x0000000a,
   0x0000000e,
   0x00000008,
   0x00000009,
   0x0004003d,
   0x00000005,
   0x0000000f,
   0x0000000e,
   0x0004007c,
   0x0000000b,
   0x00000010,
   0x0000000f,
   0x0005003e,
   0x00000010,
   0x0000000c,
   0x00000002,
   0x00000004,
   0x000100fd,
   0x00010038
};

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

   probe_log = fopen("/data/local/tmp/xclipse940/bda_cpdma_readback_probe.log", "w");
   if (probe_log) {
      setvbuf(probe_log, NULL, _IONBF, 0);
      persist_mark("START bda_global_store_probe");
   }

   printf("=== Xclipse 940 RADV BDA + CP-DMA readback probe ===\n");
   printf("driver: %s\n", driver_path);
   printf("work: one PhysicalStorageBuffer uint store = 0x12345678\n");
   printf("dispatch: 1x1x1\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      persist_mark("FAIL dlopen");
      return 10;
   }

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL: HMI missing\n");
      return 11;
   }

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr != 0 || !base_dev) {
      fprintf(stderr, "FAIL: HAL open=%d\n", hr);
      return 12;
   }

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-bda-global-store-probe",
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
   printf("[1/17] vkCreateInstance = %d (%s)\n", vr, vk_result_name(vr));
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
   GET_INST(vkGetPhysicalDeviceFeatures2);
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

   VkPhysicalDeviceProperties props = {0};
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[2/17] GPU: %s Vulkan=%u.%u.%u\n",
          props.deviceName,
          VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));

   VkPhysicalDeviceBufferDeviceAddressFeatures bda_support = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
   };
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &bda_support,
   };
   vkGetPhysicalDeviceFeatures2(pdev, &features2);

   printf("[3/17] shaderInt64=%u bufferDeviceAddress=%u\n",
          features2.features.shaderInt64,
          bda_support.bufferDeviceAddress);
   persist_mark("features shaderInt64=%u bda=%u",
                features2.features.shaderInt64,
                bda_support.bufferDeviceAddress);

   if (!features2.features.shaderInt64 || !bda_support.bufferDeviceAddress) {
      printf("SKIP: required shaderInt64/BDA feature is not exposed.\n");
      persist_mark("SKIP missing feature");
      return 90;
   }

   VkPhysicalDeviceMemoryProperties mp = {0};
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
      if (selected_qf == UINT32_MAX &&
          qf[i].queueCount > 0 &&
          (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
         selected_qf = i;
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

   VkPhysicalDeviceBufferDeviceAddressFeatures bda_enable = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
      .bufferDeviceAddress = VK_TRUE,
   };
   VkPhysicalDeviceFeatures enabled_features = {0};
   enabled_features.shaderInt64 = VK_TRUE;

   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &bda_enable,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .pEnabledFeatures = &enabled_features,
   };

   VkDevice device = VK_NULL_HANDLE;
   vr = vkCreateDevice(pdev, &dci, NULL, &device);
   printf("[4/17] vkCreateDevice = %d (%s)\n", vr, vk_result_name(vr));
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
   GET_DEV(vkGetBufferDeviceAddress);

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
   GET_DEV(vkCmdPushConstants);
   GET_DEV(vkCmdDispatch);
   GET_DEV(vkCmdPipelineBarrier);
   GET_DEV(vkCmdCopyBuffer);
   GET_DEV(vkEndCommandBuffer);

   GET_DEV(vkCreateFence);
   GET_DEV(vkDestroyFence);
   GET_DEV(vkQueueSubmit);
   GET_DEV(vkWaitForFences);
   GET_DEV(vkGetFenceStatus);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   if (!queue)
      return 32;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4096,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkBuffer buffer = VK_NULL_HANDLE;
   vr = vkCreateBuffer(device, &bci, NULL, &buffer);
   printf("[5/17] vkCreateBuffer(BDA,4096) = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 40;

   VkMemoryRequirements req = {0};
   vkGetBufferMemoryRequirements(device, buffer, &req);

   uint32_t mt = find_host_coherent_type(req.memoryTypeBits, &mp);
   if (mt == UINT32_MAX) {
      fprintf(stderr, "FAIL: no HOST_VISIBLE|HOST_COHERENT memory type\n");
      return 41;
   }

   VkMemoryAllocateFlagsInfo maf = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &maf,
      .allocationSize = req.size,
      .memoryTypeIndex = mt,
   };

   VkDeviceMemory memory = VK_NULL_HANDLE;
   vr = vkAllocateMemory(device, &mai, NULL, &memory);
   printf("[6/17] vkAllocateMemory(BDA flag) = %d (%s), type=%u\n",
          vr, vk_result_name(vr), mt);
   if (vr != VK_SUCCESS)
      return 42;

   vr = vkBindBufferMemory(device, buffer, memory, 0);
   printf("[7/17] vkBindBufferMemory = %d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   void *map = NULL;
   vr = vkMapMemory(device, memory, 0, req.size, 0, &map);
   printf("[8/17] vkMapMemory = %d (%s), ptr=%p\n",
          vr, vk_result_name(vr), map);
   if (vr != VK_SUCCESS || !map)
      return 44;

   memset(map, 0, req.size);
   ((volatile uint32_t *)map)[0] = 0xdeadbeefu;

   VkBufferDeviceAddressInfo addr_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = buffer,
   };
   VkDeviceAddress address = vkGetBufferDeviceAddress(device, &addr_info);
   printf("[9/17] vkGetBufferDeviceAddress = 0x%016llx\n",
          (unsigned long long)address);
   persist_mark("device_address=0x%016llx initial=0x%08x",
                (unsigned long long)address,
                ((volatile uint32_t *)map)[0]);
   if (!address) {
      fprintf(stderr, "FAIL: BDA returned zero\n");
      return 45;
   }

   /*
    * Readback buffer for a 4-byte vkCmdCopyBuffer after the shader store.
    * At 4 bytes RADV takes the CP-DMA path, avoiding the >=4096-byte
    * compute-meta copy path that is already known to be problematic here.
    */
   VkBufferCreateInfo rb_bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4096,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkBuffer readback = VK_NULL_HANDLE;
   vr = vkCreateBuffer(device, &rb_bci, NULL, &readback);
   printf("       vkCreateBuffer(readback,4096) = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 81;

   VkMemoryRequirements rb_req = {0};
   vkGetBufferMemoryRequirements(device, readback, &rb_req);

   uint32_t rb_mt = find_host_coherent_type(rb_req.memoryTypeBits, &mp);
   if (rb_mt == UINT32_MAX) {
      fprintf(stderr, "FAIL: no HOST_VISIBLE|HOST_COHERENT readback memory type\n");
      return 82;
   }

   VkMemoryAllocateInfo rb_mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = rb_req.size,
      .memoryTypeIndex = rb_mt,
   };

   VkDeviceMemory rb_memory = VK_NULL_HANDLE;
   vr = vkAllocateMemory(device, &rb_mai, NULL, &rb_memory);
   printf("       vkAllocateMemory(readback) = %d (%s), type=%u\n",
          vr, vk_result_name(vr), rb_mt);
   if (vr != VK_SUCCESS)
      return 83;

   vr = vkBindBufferMemory(device, readback, rb_memory, 0);
   if (vr != VK_SUCCESS)
      return 84;

   void *rb_map = NULL;
   vr = vkMapMemory(device, rb_memory, 0, rb_req.size, 0, &rb_map);
   printf("       vkMapMemory(readback) = %d (%s), ptr=%p\n",
          vr, vk_result_name(vr), rb_map);
   if (vr != VK_SUCCESS || !rb_map)
      return 85;

   memset(rb_map, 0, rb_req.size);
   ((volatile uint32_t *)rb_map)[0] = 0xcafebabeu;
   persist_mark("readback initial=0x%08x",
                ((volatile uint32_t *)rb_map)[0]);

   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(bda_store_spv),
      .pCode = bda_store_spv,
   };
   VkShaderModule shader = VK_NULL_HANDLE;
   persist_mark("before vkCreateShaderModule");
   vr = vkCreateShaderModule(device, &smci, NULL, &shader);
   printf("[10/17] vkCreateShaderModule = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkCreateShaderModule result=%d", vr);
   if (vr != VK_SUCCESS)
      return 46;

   VkPushConstantRange pcr = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0,
      .size = 8,
   };
   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   vr = vkCreatePipelineLayout(device, &plci, NULL, &layout);
   printf("[11/17] vkCreatePipelineLayout = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 47;

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
   persist_mark("before vkCreateComputePipelines");
   vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci,
                                 NULL, &pipeline);
   printf("[12/17] vkCreateComputePipelines = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkCreateComputePipelines result=%d", vr);
   if (vr != VK_SUCCESS)
      return 48;

   VkCommandPoolCreateInfo cpool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };
   VkCommandPool cmd_pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &cpool_ci, NULL, &cmd_pool);
   if (vr != VK_SUCCESS)
      return 49;

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cbai, &cb);
   if (vr != VK_SUCCESS)
      return 50;

   VkCommandBufferBeginInfo cbbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   vr = vkBeginCommandBuffer(cb, &cbbi);
   if (vr != VK_SUCCESS)
      return 51;

   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   /*
    * Diagnostic only: some SGPU/Xclipse shader-memory paths may consume
    * the 48-bit GPU VA without the CPU-side canonical sign extension.
    * Default remains the exact VkDeviceAddress. Set BDA_ADDR_MODE=mask48
    * to push only bits [47:0].
    */
   uint64_t pc_address = (uint64_t)address;
   const char *addr_mode = getenv("BDA_ADDR_MODE");
   if (addr_mode && !strcmp(addr_mode, "mask48"))
      pc_address &= 0x0000ffffffffffffull;

   printf("       pushed shader address = 0x%016llx (%s)\n",
          (unsigned long long)pc_address,
          (addr_mode && !strcmp(addr_mode, "mask48")) ? "mask48" : "canonical");
   persist_mark("pushed_shader_address=0x%016llx mode=%s",
                (unsigned long long)pc_address,
                (addr_mode && !strcmp(addr_mode, "mask48")) ? "mask48" : "canonical");

   vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                      0, sizeof(pc_address), &pc_address);

   persist_mark("before vkCmdDispatch");
   vkCmdDispatch(cb, 1, 1, 1);
   persist_mark("after vkCmdDispatch");

   VkMemoryBarrier shader_to_transfer = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   persist_mark("before compute->transfer barrier");
   vkCmdPipelineBarrier(cb,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0,
                        1, &shader_to_transfer,
                        0, NULL,
                        0, NULL);
   persist_mark("after compute->transfer barrier");

   VkBufferCopy copy_region = {
      .srcOffset = 0,
      .dstOffset = 0,
      .size = 4,
   };
   persist_mark("before vkCmdCopyBuffer 4B");
   vkCmdCopyBuffer(cb, buffer, readback, 1, &copy_region);
   persist_mark("after vkCmdCopyBuffer 4B");

   VkMemoryBarrier gpu_to_host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   persist_mark("before gpu->host barrier");
   vkCmdPipelineBarrier(cb,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT,
                        0,
                        1, &gpu_to_host,
                        0, NULL,
                        0, NULL);
   persist_mark("after gpu->host barrier");

   vr = vkEndCommandBuffer(cb);
   printf("[13/17] vkEndCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 52;

   VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fci, NULL, &fence);
   if (vr != VK_SUCCESS)
      return 53;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   persist_mark("before vkQueueSubmit");
   vr = vkQueueSubmit(queue, 1, &si, fence);
   printf("[14/17] vkQueueSubmit = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkQueueSubmit result=%d", vr);
   if (vr != VK_SUCCESS)
      return 54;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("[15/17] immediate fence = %d (%s)\n",
          fs, vk_result_name(fs));

   persist_mark("before vkWaitForFences");
   vr = vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout_ns);
   persist_mark("after vkWaitForFences result=%d", vr);
   printf("[16/17] vkWaitForFences = %d (%s)\n",
          vr, vk_result_name(vr));

   if (vr == VK_TIMEOUT) {
      printf("TIMEOUT: BDA/global-store dispatch did not finish.\n");
      persist_mark("TIMEOUT");
      return 60;
   }
   if (vr != VK_SUCCESS)
      return 61;

   uint32_t direct = ((volatile uint32_t *)map)[0];
   uint32_t copied = ((volatile uint32_t *)rb_map)[0];
   printf("[17/17] target CPU result   = 0x%08x\n", direct);
   printf("        CP-DMA readback result = 0x%08x\n", copied);
   persist_mark("target_cpu=0x%08x cpdma_readback=0x%08x",
                direct, copied);

   if (copied == 0x12345678u) {
      printf("PASS: CP-DMA observed the shader store (direct CPU=%08x).\n",
             direct);
      persist_mark("PASS cpdma observed shader store");
   } else if (direct == 0x12345678u) {
      printf("PARTIAL: direct CPU saw shader store, CP-DMA did not.\n");
      persist_mark("PARTIAL direct only");
      return 71;
   } else {
      printf("FAIL: neither CPU nor CP-DMA observed the shader store.\n");
      persist_mark("verification FAIL both paths");
      return 70;
   }

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, cmd_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkUnmapMemory(device, rb_memory);
   vkDestroyBuffer(device, readback, NULL);
   vkFreeMemory(device, rb_memory, NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);

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

   const char *driver_path = argc >= 2 ? argv[1] : "./libvulkan_radeon.so";

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
         FILE *f = fopen("/data/local/tmp/xclipse940/bda_cpdma_readback_parent.log", "w");

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

         FILE *f = fopen("/data/local/tmp/xclipse940/bda_cpdma_readback_parent.log", "w");
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
