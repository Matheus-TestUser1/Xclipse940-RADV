/*
 * Xclipse 940 RADV - minimal SSBO compute write diagnostic
 *
 * Goal:
 *   Distinguish normal Vulkan SSBO memory access from RADV's internal
 *   meta-buffer path that uses raw 64-bit GPU virtual addresses.
 *
 * Shader:
 *   layout(local_size_x = 1) in;
 *   layout(set = 0, binding = 0) buffer Output {
 *       uint value;
 *   } outbuf;
 *   void main() {
 *       outbuf.value = 0x12345678u;
 *   }
 *
 * Test:
 *   - one 4-byte HOST_VISIBLE|HOST_COHERENT storage buffer
 *   - descriptor set with VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
 *   - one compute dispatch: vkCmdDispatch(1,1,1)
 *   - finite fence wait
 *   - CPU checks value == 0x12345678
 *
 * No images, no graphics, no large allocations.
 *
 * Safety:
 *   - one invocation
 *   - one 32-bit GPU store
 *   - 1 second fence timeout by default
 *   - child-process watchdog
 *   - persistent fsync() logs
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
 * Embedded SPIR-V 1.3 equivalent to:
 *
 * #version 450
 * layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
 *
 * layout(set = 0, binding = 0, std430) buffer Output {
 *     uint value;
 * } outbuf;
 *
 * void main()
 * {
 *     outbuf.value = 0x12345678u;
 * }
 *
 * IDs:
 *   %1  main
 *   %2  void
 *   %3  function type
 *   %4  uint
 *   %5  Output struct
 *   %6  ptr StorageBuffer Output
 *   %7  outbuf
 *   %8  ptr StorageBuffer uint
 *   %9  uint constant 0
 *   %10 uint constant 0x12345678
 *   %11 label
 *   %12 access-chain pointer
 */
static const uint32_t ssbo_write_spv[] = {
   /* SPIR-V header */
   0x07230203,
   0x00010300,
   0x00000000,
   0x0000000d,
   0x00000000,

   /* OpCapability Shader */
   0x00020011, 0x00000001,

   /* OpMemoryModel Logical GLSL450 */
   0x0003000e, 0x00000000, 0x00000001,

   /* OpEntryPoint GLCompute %1 "main" %7 */
   0x0006000f, 0x00000005, 0x00000001,
   0x6e69616d, 0x00000000, 0x00000007,

   /* OpExecutionMode %1 LocalSize 1 1 1 */
   0x00060010, 0x00000001, 0x00000011,
   0x00000001, 0x00000001, 0x00000001,

   /* OpDecorate %5 Block */
   0x00030047, 0x00000005, 0x00000002,

   /* OpMemberDecorate %5 0 Offset 0 */
   0x00050048, 0x00000005, 0x00000000,
   0x00000023, 0x00000000,

   /* OpDecorate %7 DescriptorSet 0 */
   0x00040047, 0x00000007, 0x00000022, 0x00000000,

   /* OpDecorate %7 Binding 0 */
   0x00040047, 0x00000007, 0x00000021, 0x00000000,

   /* %2 = OpTypeVoid */
   0x00020013, 0x00000002,

   /* %3 = OpTypeFunction %2 */
   0x00030021, 0x00000003, 0x00000002,

   /* %4 = OpTypeInt 32 0 */
   0x00040015, 0x00000004, 0x00000020, 0x00000000,

   /* %5 = OpTypeStruct %4 */
   0x0003001e, 0x00000005, 0x00000004,

   /* %6 = OpTypePointer StorageBuffer %5 */
   0x00040020, 0x00000006, 0x0000000c, 0x00000005,

   /* %7 = OpVariable %6 StorageBuffer */
   0x0004003b, 0x00000006, 0x00000007, 0x0000000c,

   /* %8 = OpTypePointer StorageBuffer %4 */
   0x00040020, 0x00000008, 0x0000000c, 0x00000004,

   /* %9 = OpConstant %4 0 */
   0x0004002b, 0x00000004, 0x00000009, 0x00000000,

   /* %10 = OpConstant %4 0x12345678 */
   0x0004002b, 0x00000004, 0x0000000a, 0x12345678,

   /* %1 = OpFunction %2 None %3 */
   0x00050036, 0x00000002, 0x00000001,
   0x00000000, 0x00000003,

   /* %11 = OpLabel */
   0x000200f8, 0x0000000b,

   /* %12 = OpAccessChain %8 %7 %9
    * Word count = 5: instruction + result type + result id + base + one index.
    */
   0x00050041, 0x00000008, 0x0000000c,
   0x00000007, 0x00000009,

   /* OpStore %12 %10 */
   0x0003003e, 0x0000000c, 0x0000000a,

   /* OpReturn */
   0x000100fd,

   /* OpFunctionEnd */
   0x00010038,
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

   probe_log =
      fopen("/data/local/tmp/xclipse940/ssbo_compute_probe_v3.log", "w");
   if (probe_log) {
      setvbuf(probe_log, NULL, _IONBF, 0);
      persist_mark("START ssbo_compute_probe");
   }

   printf("=== Xclipse 940 RADV minimal SSBO compute probe v3 (fixed SPIR-V + HOST barrier) ===\n");
   printf("driver: %s\n", driver_path);
   printf("shader: one SSBO uint store = 0x12345678\n");
   printf("dispatch: 1x1x1\n");
   printf("fence timeout: %.3f s\n\n",
          (double)fence_timeout_ns / 1000000000.0);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
      persist_mark("FAIL dlopen");
      return 10;
   }
   printf("[1/20] dlopen OK\n");

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
   printf("[2/20] HAL open OK\n");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-ssbo-compute-probe",
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
   printf("[3/20] vkCreateInstance = %d (%s)\n",
          vr, vk_result_name(vr));
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

   printf("[4/20] GPU: %s Vulkan=%u.%u.%u\n",
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
   printf("[5/20] vkCreateDevice = %d (%s)\n",
          vr, vk_result_name(vr));
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

   GET_DEV(vkCreateDescriptorSetLayout);
   GET_DEV(vkDestroyDescriptorSetLayout);
   GET_DEV(vkCreateDescriptorPool);
   GET_DEV(vkDestroyDescriptorPool);
   GET_DEV(vkAllocateDescriptorSets);
   GET_DEV(vkUpdateDescriptorSets);

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
   GET_DEV(vkCmdBindDescriptorSets);
   GET_DEV(vkCmdDispatch);
   GET_DEV(vkCmdPipelineBarrier);
   GET_DEV(vkEndCommandBuffer);

   GET_DEV(vkCreateFence);
   GET_DEV(vkDestroyFence);
   GET_DEV(vkQueueSubmit);
   GET_DEV(vkWaitForFences);
   GET_DEV(vkGetFenceStatus);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, selected_qf, 0, &queue);
   printf("[6/20] vkGetDeviceQueue = %p\n", (void *)queue);
   if (!queue)
      return 32;

   /*
    * Allocate 4096 bytes even though the shader only touches 4 bytes.
    * This keeps the BO comfortably page-sized and easy to inspect.
    */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4096,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };

   VkBuffer buffer = VK_NULL_HANDLE;
   vr = vkCreateBuffer(device, &bci, NULL, &buffer);
   printf("[7/20] vkCreateBuffer(STORAGE, 4096) = %d (%s)\n",
          vr, vk_result_name(vr));
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
   printf("[8/20] vkAllocateMemory = %d (%s), type=%u\n",
          vr, vk_result_name(vr), mt);
   if (vr != VK_SUCCESS)
      return 42;

   vr = vkBindBufferMemory(device, buffer, memory, 0);
   printf("[9/20] vkBindBufferMemory = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 43;

   void *map = NULL;
   vr = vkMapMemory(device, memory, 0, req.size, 0, &map);
   printf("[10/20] vkMapMemory = %d (%s), ptr=%p\n",
          vr, vk_result_name(vr), map);
   if (vr != VK_SUCCESS || !map)
      return 44;

   memset(map, 0, req.size);
   ((volatile uint32_t *)map)[0] = 0xdeadbeefu;

   printf("        initial value = 0x%08x\n",
          ((volatile uint32_t *)map)[0]);
   persist_mark("buffer ready initial=0x%08x",
                ((volatile uint32_t *)map)[0]);

   VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };

   VkDescriptorSetLayoutCreateInfo dsl_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };

   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   vr = vkCreateDescriptorSetLayout(device, &dsl_ci, NULL, &dsl);
   printf("[11/20] vkCreateDescriptorSetLayout = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 45;

   VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
   };

   VkDescriptorPoolCreateInfo dp_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };

   VkDescriptorPool desc_pool = VK_NULL_HANDLE;
   vr = vkCreateDescriptorPool(device, &dp_ci, NULL, &desc_pool);
   printf("[12/20] vkCreateDescriptorPool = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 46;

   VkDescriptorSetAllocateInfo ds_ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };

   VkDescriptorSet ds = VK_NULL_HANDLE;
   vr = vkAllocateDescriptorSets(device, &ds_ai, &ds);
   printf("[13/20] vkAllocateDescriptorSets = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 47;

   VkDescriptorBufferInfo dbi = {
      .buffer = buffer,
      .offset = 0,
      .range = sizeof(uint32_t),
   };

   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = ds,
      .dstBinding = 0,
      .dstArrayElement = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &dbi,
   };

   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
   printf("[14/20] vkUpdateDescriptorSets done\n");
   persist_mark("descriptor updated");

   VkShaderModuleCreateInfo sm_ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(ssbo_write_spv),
      .pCode = ssbo_write_spv,
   };

   VkShaderModule shader = VK_NULL_HANDLE;
   persist_mark("before vkCreateShaderModule");
   vr = vkCreateShaderModule(device, &sm_ci, NULL, &shader);
   printf("[15/20] vkCreateShaderModule = %d (%s), shader=%p\n",
          vr, vk_result_name(vr), (void *)shader);
   persist_mark("after vkCreateShaderModule result=%d", vr);
   if (vr != VK_SUCCESS)
      return 48;

   VkPipelineLayoutCreateInfo pl_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };

   VkPipelineLayout layout = VK_NULL_HANDLE;
   vr = vkCreatePipelineLayout(device, &pl_ci, NULL, &layout);
   printf("[16/20] vkCreatePipelineLayout = %d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 49;

   VkPipelineShaderStageCreateInfo stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader,
      .pName = "main",
   };

   VkComputePipelineCreateInfo cp_ci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage,
      .layout = layout,
   };

   VkPipeline pipeline = VK_NULL_HANDLE;
   persist_mark("before vkCreateComputePipelines");
   vr = vkCreateComputePipelines(device, VK_NULL_HANDLE,
                                 1, &cp_ci, NULL, &pipeline);
   printf("[17/20] vkCreateComputePipelines = %d (%s), pipeline=%p\n",
          vr, vk_result_name(vr), (void *)pipeline);
   persist_mark("after vkCreateComputePipelines result=%d", vr);
   if (vr != VK_SUCCESS)
      return 50;

   VkCommandPoolCreateInfo cmd_pool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
      .queueFamilyIndex = selected_qf,
   };

   VkCommandPool cmd_pool = VK_NULL_HANDLE;
   vr = vkCreateCommandPool(device, &cmd_pool_ci, NULL, &cmd_pool);
   if (vr != VK_SUCCESS)
      return 51;

   VkCommandBufferAllocateInfo cb_ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };

   VkCommandBuffer cb = VK_NULL_HANDLE;
   vr = vkAllocateCommandBuffers(device, &cb_ai, &cb);
   if (vr != VK_SUCCESS)
      return 52;

   VkCommandBufferBeginInfo cb_bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };

   vr = vkBeginCommandBuffer(cb, &cb_bi);
   if (vr != VK_SUCCESS)
      return 53;

   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   vkCmdBindDescriptorSets(cb,
                           VK_PIPELINE_BIND_POINT_COMPUTE,
                           layout,
                           0, 1, &ds,
                           0, NULL);

   persist_mark("before vkCmdDispatch");
   vkCmdDispatch(cb, 1, 1, 1);
   persist_mark("after vkCmdDispatch");

   /*
    * Make compute shader writes available/visible to subsequent HOST reads.
    * A fence is an execution-completion primitive; this barrier establishes
    * the required memory dependency for CPU readback.
    */
   VkMemoryBarrier host_read_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };

   persist_mark("before compute->host memory barrier");
   vkCmdPipelineBarrier(cb,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT,
                        0,
                        1, &host_read_barrier,
                        0, NULL,
                        0, NULL);
   persist_mark("after compute->host memory barrier");

   vr = vkEndCommandBuffer(cb);
   printf("[18/20] vkEndCommandBuffer = %d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkEndCommandBuffer result=%d", vr);
   if (vr != VK_SUCCESS)
      return 54;

   VkFenceCreateInfo fence_ci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };

   VkFence fence = VK_NULL_HANDLE;
   vr = vkCreateFence(device, &fence_ci, NULL, &fence);
   if (vr != VK_SUCCESS)
      return 55;

   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cb,
   };

   printf("[19/20] vkQueueSubmit(dispatch)...\n");
   persist_mark("before vkQueueSubmit");

   vr = vkQueueSubmit(queue, 1, &submit, fence);

   printf("        submit=%d (%s)\n",
          vr, vk_result_name(vr));
   persist_mark("after vkQueueSubmit result=%d", vr);
   if (vr != VK_SUCCESS)
      return 56;

   VkResult fs = vkGetFenceStatus(device, fence);
   printf("        immediate fence=%d (%s)\n",
          fs, vk_result_name(fs));

   persist_mark("before vkWaitForFences");
   vr = vkWaitForFences(device, 1, &fence,
                        VK_TRUE, fence_timeout_ns);
   persist_mark("after vkWaitForFences result=%d", vr);

   printf("[20/20] vkWaitForFences = %d (%s)\n",
          vr, vk_result_name(vr));

   if (vr == VK_TIMEOUT) {
      fprintf(stderr,
              "\nTIMEOUT: SSBO compute dispatch did not finish.\n");
      persist_mark("TIMEOUT");
      return 60;
   }

   if (vr != VK_SUCCESS)
      return 61;

   uint32_t got = ((volatile uint32_t *)map)[0];

   printf("\nCPU read after fence:\n");
   printf("    got      = 0x%08x\n", got);
   printf("    expected = 0x12345678\n");

   persist_mark("CPU result=0x%08x", got);

   printf("    first 16 bytes:");
   for (unsigned i = 0; i < 16; i++)
      printf(" %02x", ((volatile uint8_t *)map)[i]);
   printf("\n");

   if (got != 0x12345678u) {
      printf("\nFAIL: shader completed but SSBO value did not match.\n");
      persist_mark("verification FAIL");
      return 70;
   }

   printf("\nPASS: normal SSBO compute store worked.\n");
   printf("Shader wrote 0x12345678 through a Vulkan storage-buffer descriptor.\n");
   persist_mark("PASS complete");

   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, cmd_pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkDestroyDescriptorPool(device, desc_pool, NULL);
   vkDestroyDescriptorSetLayout(device, dsl, NULL);
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
            fopen("/data/local/tmp/xclipse940/ssbo_compute_parent_v3.log", "w");

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
            fopen("/data/local/tmp/xclipse940/ssbo_compute_parent_v3.log", "w");
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
