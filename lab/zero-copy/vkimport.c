#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){ int sfd=atoi(argv[5]);
 VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai}; VkInstance inst; vkCreateInstance(&ici,0,&inst);
 uint32_t n=1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst,&n,&pd);
 float prio=1; VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&prio};
 VkPhysicalDeviceTimelineSemaphoreFeatures tf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.timelineSemaphore=VK_TRUE};
 const char*ext[]={"VK_KHR_external_semaphore_fd"}; VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&tf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=ext};
 VkDevice dev; vkCreateDevice(pd,&dci,0,&dev);
 VkSemaphoreTypeCreateInfo stc={VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE};
 VkSemaphoreCreateInfo sci={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,&stc}; VkSemaphore s; vkCreateSemaphore(dev,&sci,0,&s);
 PFN_vkImportSemaphoreFdKHR imp=(void*)vkGetDeviceProcAddr(dev,"vkImportSemaphoreFdKHR");
 VkImportSemaphoreFdInfoKHR ifi={VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=s,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,.fd=sfd};
 VkResult r=imp(dev,&ifi); printf("vk import semaphore fd %d -> %d\n",sfd,r);
 if(!r){ uint64_t v=2; VkSemaphoreWaitInfo wi={VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,.semaphoreCount=1,.pSemaphores=&s,.pValues=&v}; r=vkWaitSemaphores(dev,&wi,2000000000ull); printf("wait value 2 -> %d\n",r);}
 return r; }
