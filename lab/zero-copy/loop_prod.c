// Frame loop: Vulkan producer renders frame i (clear to colour i) into a shared 1920x1080 image, CPU-waits for its GPU work,
// then "commits" over a pipe; the GL consumer samples it (no copy) and acks. Measures per-frame cost and checks content.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#define CK(x) do{VkResult r_=(x); if(r_){fprintf(stderr,"%s -> %d\n",#x,r_);exit(1);}}while(0)
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
int main(int argc,char**argv){
 const unsigned W=1920,H=1080; int N=300;
 VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai}; VkInstance inst; CK(vkCreateInstance(&ici,0,&inst));
 uint32_t n=1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst,&n,&pd);
 float prio=1; VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&prio};
 const char*ext[]={"VK_KHR_external_memory_fd"}; VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=ext};
 VkDevice dev; CK(vkCreateDevice(pd,&dci,0,&dev)); VkQueue q; vkGetDeviceQueue(dev,0,0,&q);
 VkExternalMemoryImageCreateInfo emi={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,&emi,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,
  .samples=1,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
 VkImage img; CK(vkCreateImage(dev,&ii,0,&img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
 VkMemoryDedicatedAllocateInfo ded={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=img};
 VkExportMemoryAllocateInfo ema={VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,&ded,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&ema,.allocationSize=mr.size,.memoryTypeIndex=0};
 VkDeviceMemory mem; CK(vkAllocateMemory(dev,&mai,0,&mem)); CK(vkBindImageMemory(dev,img,mem,0));
 PFN_vkGetMemoryFdKHR getfd=(void*)vkGetDeviceProcAddr(dev,"vkGetMemoryFdKHR");
 VkMemoryGetFdInfoKHR gfi={VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,.memory=mem,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 int fd=-1; CK(getfd(dev,&gfi,&fd)); fcntl(fd,F_SETFD,0);
 VkCommandPoolCreateInfo cpi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=0}; VkCommandPool cp; CK(vkCreateCommandPool(dev,&cpi,0,&cp));
 VkCommandBufferAllocateInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.commandBufferCount=1}; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev,&cbi,&cb));
 VkFenceCreateInfo fci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CK(vkCreateFence(dev,&fci,0,&fence));
 int toC[2],toP[2]; pipe(toC); pipe(toP);
 char a1[16],a2[32],a3[16],a4[16]; snprintf(a1,16,"%d",fd); snprintf(a2,32,"%llu",(unsigned long long)mr.size); snprintf(a3,16,"%d",toC[0]); snprintf(a4,16,"%d",toP[1]);
 pid_t p=fork(); if(!p){execl(argv[1],argv[1],a1,a2,a3,a4,(char*)0); _exit(127);}
 VkImageSubresourceRange rng={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; double tr=0,tc=0; int bad=0; VkImageLayout lay=VK_IMAGE_LAYOUT_UNDEFINED;
 close(toC[0]); close(toP[1]); char ready; if(read(toP[0],&ready,1)!=1){int st; waitpid(p,&st,0); fprintf(stderr,"consumer died: status 0x%x\n",st); return 2;}
 for(int i=0;i<N;i++){
  double t0=now();
  vkResetCommandBuffer(cb,0); VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb,&bi);
  VkImageMemoryBarrier b1={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=lay,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=img,.subresourceRange=rng};
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&b1);
  VkClearColorValue col={.float32={(i%256)/255.0f,((i*7)%256)/255.0f,1,1}}; vkCmdClearColorImage(cb,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&col,1,&rng);
  VkImageMemoryBarrier b2=b1; b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask=0; b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,0,0,0,1,&b2); lay=VK_IMAGE_LAYOUT_GENERAL;
  vkEndCommandBuffer(cb);
  VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb}; CK(vkQueueSubmit(q,1,&si,fence));
  CK(vkWaitForFences(dev,1,&fence,1,UINT64_MAX)); vkResetFences(dev,1,&fence);   // CPU wait instead of a shared fence
double t1=now(); unsigned char msg=(unsigned char)i; write(toC[1],&msg,1); unsigned char ok; read(toP[0],&ok,1); double t2=now();
  if(!ok) bad++; tr+=t1-t0; tc+=t2-t1;
 }
 kill(p,SIGTERM); int st; waitpid(p,&st,0);
 printf("%d frames 1080p: producer render+cpu-wait %.2f ms, commit->consumer sampled+ack %.2f ms, wrong frames %d\n",N,tr/N,tc/N,bad);
 return bad!=0;
}
