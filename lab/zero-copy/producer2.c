// Vulkan (dzn) producer with a shared timeline semaphore. Work: [GPU waits value 1] clear orange, signal 2.
// The host releases value 1 only after 300 ms, so a consumer that really GPU-waits on 2 sees orange; one that doesn't sees black.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#define CK(x) do{VkResult r_=(x); if(r_){fprintf(stderr,"%s -> %d\n",#x,r_);exit(1);}}while(0)
static VkDevice dev; static VkQueue q; static VkCommandPool cp;
static VkCommandBuffer clear_cmd(VkImage img, float r,float g,float b, VkImageLayout from){
 VkCommandBufferAllocateInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1}; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbi,&cb));
 VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb,&bi);
 VkImageSubresourceRange rng={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageMemoryBarrier b1={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=from,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
  .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,.subresourceRange=rng};
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b1);
 VkClearColorValue col={.float32={r,g,b,1}}; vkCmdClearColorImage(cb,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&col,1,&rng);
 VkImageMemoryBarrier b2=b1; b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask=0; b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_GENERAL;
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,0,0,0,1,&b2);
 vkEndCommandBuffer(cb); return cb; }
int main(int argc,char**argv){
 const unsigned W=256,H=256; const char*mode=argc>2?argv[2]:"wait";
 VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_2};
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai}; VkInstance inst; CK(vkCreateInstance(&ici,0,&inst));
 uint32_t n=1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst,&n,&pd);
 float prio=1; VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&prio};
 VkPhysicalDeviceTimelineSemaphoreFeatures tf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,.timelineSemaphore=VK_TRUE};
 const char*ext[]={"VK_KHR_external_memory_fd","VK_KHR_external_semaphore_fd"};
 VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&tf,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=2,.ppEnabledExtensionNames=ext};
 CK(vkCreateDevice(pd,&dci,0,&dev)); vkGetDeviceQueue(dev,0,0,&q);
 VkExternalMemoryImageCreateInfo emi={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,&emi,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,
  .samples=1,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
 VkImage img; CK(vkCreateImage(dev,&ii,0,&img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
 VkMemoryDedicatedAllocateInfo ded={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=img};
 VkExportMemoryAllocateInfo ema={VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,&ded,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&ema,.allocationSize=mr.size,.memoryTypeIndex=0};
 VkDeviceMemory mem; CK(vkAllocateMemory(dev,&mai,0,&mem)); CK(vkBindImageMemory(dev,img,mem,0));
 VkCommandPoolCreateInfo cpi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0}; CK(vkCreateCommandPool(dev,&cpi,0,&cp));
 // 1) initialise black and finish
 VkCommandBuffer cb0=clear_cmd(img,0,0,0,VK_IMAGE_LAYOUT_UNDEFINED);
 VkSubmitInfo s0={VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb0}; CK(vkQueueSubmit(q,1,&s0,0)); vkQueueWaitIdle(q);
 // 2) exportable timeline semaphore
 VkSemaphoreTypeCreateInfo stc={VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE,.initialValue=0};
 VkExportSemaphoreCreateInfo esc={VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,&stc,.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkSemaphoreCreateInfo sci={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,&esc}; VkSemaphore sem; CK(vkCreateSemaphore(dev,&sci,0,&sem));
 PFN_vkGetSemaphoreFdKHR getsfd=(void*)vkGetDeviceProcAddr(dev,"vkGetSemaphoreFdKHR");
 VkSemaphoreGetFdInfoKHR sgi={VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,.semaphore=sem,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
 int sfd=-1; CK(getsfd(dev,&sgi,&sfd)); fcntl(sfd,F_SETFD,0);
 PFN_vkGetMemoryFdKHR getfd=(void*)vkGetDeviceProcAddr(dev,"vkGetMemoryFdKHR");
 VkMemoryGetFdInfoKHR gfi={VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,.memory=mem,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 int fd=-1; CK(getfd(dev,&gfi,&fd)); fcntl(fd,F_SETFD,0);
 { char p[64],t[128]; snprintf(p,64,"/proc/self/fd/%d",sfd); ssize_t l=readlink(p,t,127); fprintf(stderr,"semaphore fd -> %.*s\n",(int)(l>0?l:0),t); }
 // 3) gated work: wait 1, clear orange, signal 2
 VkCommandBuffer cb1=clear_cmd(img,1,0.5f,0.25f,VK_IMAGE_LAYOUT_GENERAL);
 uint64_t wv=1, sv=2; VkPipelineStageFlags ws=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
 VkTimelineSemaphoreSubmitInfo tsi={VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,.waitSemaphoreValueCount=1,.pWaitSemaphoreValues=&wv,.signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&sv};
 VkSubmitInfo s1={VK_STRUCTURE_TYPE_SUBMIT_INFO,&tsi,.waitSemaphoreCount=1,.pWaitSemaphores=&sem,.pWaitDstStageMask=&ws,.commandBufferCount=1,.pCommandBuffers=&cb1,.signalSemaphoreCount=1,.pSignalSemaphores=&sem};
 CK(vkQueueSubmit(q,1,&s1,0));
 char a1[16],a2[32],a5[16]; snprintf(a1,16,"%d",fd); snprintf(a2,32,"%llu",(unsigned long long)mr.size); snprintf(a5,16,"%d",sfd);
 pid_t p=fork(); if(!p){execl(argv[1],argv[1],a1,a2,"256","256",a5,mode,(char*)0); _exit(127);}
 usleep(300000);
 VkSemaphoreSignalInfo ssi={VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,.semaphore=sem,.value=1}; CK(vkSignalSemaphore(dev,&ssi));
 fprintf(stderr,"producer: released value 1 after 300 ms\n");
 int st; waitpid(p,&st,0); vkQueueWaitIdle(q);
 return WIFEXITED(st)?WEXITSTATUS(st):128;
}
