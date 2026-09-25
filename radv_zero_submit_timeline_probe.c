/*
 * Xclipse 940 RADV - zero-submit timeline ordering probe
 *
 * Tests a Vulkan submit with:
 *   - one timeline wait semaphore at value 1
 *   - ZERO command buffers
 *   - one timeline signal semaphore at value 1
 *
 * Correct behavior:
 *   1. vkQueueSubmit returns successfully while the wait semaphore is unsignaled.
 *   2. The signal semaphore must NOT reach value 1 yet.
 *   3. After host-signaling the wait semaphore to value 1, the signal semaphore
 *      must reach value 1.
 *
 * This directly exercises radv_amdgpu_cs_submit_zero().
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vulkan.h>

static const char *vk_result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
   default: return "VkResult(?)";
   }
}

int main(int argc, char **argv)
{
   const char *driver_path =
      argc > 1 ? argv[1] : "./libvulkan_radeon.so";

   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   printf("START zero_submit_timeline_probe\n");
   printf("driver=%s\n", driver_path);

   void *lib = dlopen(driver_path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "FAIL dlopen: %s\n", dlerror());
      return 10;
   }

   hwvulkan_module_t *module =
      (hwvulkan_module_t *)dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
   if (!module) {
      fprintf(stderr, "FAIL dlsym HAL module: %s\n", dlerror());
      return 11;
   }

   hw_device_t *base_dev = NULL;
   int hr = module->common.methods->open(&module->common,
                                         HWVULKAN_DEVICE_0,
                                         &base_dev);
   if (hr || !base_dev) {
      fprintf(stderr, "FAIL HAL open=%d\n", hr);
      return 12;
   }

   hwvulkan_device_t *hal = (hwvulkan_device_t *)base_dev;

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "x940-zero-submit-timeline-probe",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkInstance instance = VK_NULL_HANDLE;
   VkResult vr = hal->CreateInstance(&ici, NULL, &instance);
   printf("vkCreateInstance=%d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 20;

#define GET_INST(name) \
   PFN_##name name = (PFN_##name)hal->GetInstanceProcAddr(instance, #name); \
   if (!(name)) { fprintf(stderr, "FAIL missing %s\n", #name); return 21; }

   GET_INST(vkEnumeratePhysicalDevices);
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
   if (vr != VK_SUCCESS)
      return 24;
   VkPhysicalDevice pdev = phys[0];

   VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
   };
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &timeline_features,
   };
   vkGetPhysicalDeviceFeatures2(pdev, &features2);
   printf("timelineSemaphore feature=%u\n", timeline_features.timelineSemaphore);
   if (!timeline_features.timelineSemaphore) {
      puts("FAIL timeline semaphore feature unavailable");
      return 25;
   }

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
   if (!qf_count)
      return 26;

   VkQueueFamilyProperties *qf = calloc(qf_count, sizeof(*qf));
   if (!qf)
      return 27;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qf);

   uint32_t qfi = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (qf[i].queueCount && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
         qfi = i;
         break;
      }
   }
   if (qfi == UINT32_MAX)
      return 28;

   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = qfi,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };

   timeline_features.timelineSemaphore = VK_TRUE;
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &timeline_features,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
   };

   VkDevice device = VK_NULL_HANDLE;
   vr = vkCreateDevice(pdev, &dci, NULL, &device);
   printf("vkCreateDevice=%d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 30;

#define GET_DEV(name) \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); \
   if (!(name)) { fprintf(stderr, "FAIL missing %s\n", #name); return 31; }

   GET_DEV(vkGetDeviceQueue);
   GET_DEV(vkCreateSemaphore);
   GET_DEV(vkGetSemaphoreCounterValue);
   GET_DEV(vkSignalSemaphore);
   GET_DEV(vkWaitSemaphores);
   GET_DEV(vkQueueSubmit);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, qfi, 0, &queue);

   VkSemaphoreTypeCreateInfo type_ci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0,
   };
   VkSemaphoreCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &type_ci,
   };

   VkSemaphore wait_sem = VK_NULL_HANDLE;
   VkSemaphore signal_sem = VK_NULL_HANDLE;

   vr = vkCreateSemaphore(device, &sci, NULL, &wait_sem);
   printf("vkCreateSemaphore(wait)=%d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 32;

   vr = vkCreateSemaphore(device, &sci, NULL, &signal_sem);
   printf("vkCreateSemaphore(signal)=%d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 33;

   const uint64_t wait_value = 1;
   const uint64_t signal_value = 1;
   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   VkTimelineSemaphoreSubmitInfo tsi = {
      .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
      .waitSemaphoreValueCount = 1,
      .pWaitSemaphoreValues = &wait_value,
      .signalSemaphoreValueCount = 1,
      .pSignalSemaphoreValues = &signal_value,
   };

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = &tsi,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &wait_sem,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 0,
      .pCommandBuffers = NULL,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &signal_sem,
   };

   puts("submitting ZERO command buffers: wait=1 -> signal=1");
   vr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
   printf("vkQueueSubmit=%d (%s)\n", vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 40;

   uint64_t counter = 0;
   vr = vkGetSemaphoreCounterValue(device, signal_sem, &counter);
   printf("signal counter immediately=%llu, result=%d (%s)\n",
          (unsigned long long)counter, vr, vk_result_name(vr));

   VkSemaphoreWaitInfo wait_out = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1,
      .pSemaphores = &signal_sem,
      .pValues = &signal_value,
   };

   vr = vkWaitSemaphores(device, &wait_out, 20 * 1000 * 1000ULL);
   printf("before satisfying input: vkWaitSemaphores(signal=1,20ms)=%d (%s)\n",
          vr, vk_result_name(vr));

   if (vr != VK_TIMEOUT) {
      puts("FAIL: zero-submit signal completed before its wait dependency.");
      return 41;
   }

   VkSemaphoreSignalInfo host_signal = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
      .semaphore = wait_sem,
      .value = wait_value,
   };

   vr = vkSignalSemaphore(device, &host_signal);
   printf("host vkSignalSemaphore(wait=1)=%d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS)
      return 42;

   vr = vkWaitSemaphores(device, &wait_out, 1000 * 1000 * 1000ULL);
   printf("after satisfying input: vkWaitSemaphores(signal=1,1s)=%d (%s)\n",
          vr, vk_result_name(vr));
   if (vr != VK_SUCCESS) {
      puts("FAIL: output timeline point did not follow the satisfied zero-submit dependency.");
      return 43;
   }

   counter = 0;
   vr = vkGetSemaphoreCounterValue(device, signal_sem, &counter);
   printf("final signal counter=%llu, result=%d (%s)\n",
          (unsigned long long)counter, vr, vk_result_name(vr));

   if (vr != VK_SUCCESS || counter < 1) {
      puts("FAIL: final timeline counter is wrong.");
      return 44;
   }

   puts("PASS: zero-submit timeline wait/signal ordering is correct.");
   return 0;
}
