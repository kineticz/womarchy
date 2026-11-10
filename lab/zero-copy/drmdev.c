#include <xf86drm.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
int main(){int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC); drmDevicePtr dev=NULL; int r=drmGetDevice2(fd,0,&dev);
 printf("drmGetDevice2 -> %d\n",r); if(!r){printf("bustype %d, nodes avail 0x%x, render node %s, primary %s\n",dev->bustype,dev->available_nodes,(dev->available_nodes&(1<<DRM_NODE_RENDER))?dev->nodes[DRM_NODE_RENDER]:"-",(dev->available_nodes&(1<<DRM_NODE_PRIMARY))?dev->nodes[DRM_NODE_PRIMARY]:"-");}
 drmVersionPtr v=drmGetVersion(fd); printf("driver %s\n",v?v->name:"?"); uint64_t cap=0; printf("syncobj timeline cap: %d (%llu)\n",drmGetCap(fd,DRM_CAP_SYNCOBJ_TIMELINE,&cap),(unsigned long long)cap);
 struct stat st; fstat(fd,&st); printf("dev_t %u:%u\n",major(st.st_rdev),minor(st.st_rdev));}
