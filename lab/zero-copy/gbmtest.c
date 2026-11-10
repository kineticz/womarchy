#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(){
 int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC); struct gbm_device*g=gbm_create_device(fd); printf("gbm backend: %s\n",g?gbm_device_get_backend_name(g):"none");
 PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
 EGLDisplay d=gpd(EGL_PLATFORM_GBM_KHR,g,NULL); EGLint ma,mi; if(!eglInitialize(d,&ma,&mi)){printf("eglInitialize failed 0x%x\n",eglGetError());return 1;}
 const char*e=eglQueryString(d,EGL_EXTENSIONS); printf("dma_buf_import: %d  modifiers: %d  dma_buf_export: %d\n",!!strstr(e,"EGL_EXT_image_dma_buf_import"),!!strstr(e,"image_dma_buf_import_modifiers"),!!strstr(e,"EGL_MESA_image_dma_buf_export"));
 eglBindAPI(EGL_OPENGL_ES_API); EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}; EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca); eglMakeCurrent(d,0,0,c);
 printf("renderer: %s\n",glGetString(GL_RENDERER));
 struct gbm_bo*bo=gbm_bo_create(g,1920,1080,GBM_FORMAT_ARGB8888,GBM_BO_USE_RENDERING);
 if(bo){ int bfd=gbm_bo_get_fd(bo); char p[64],t[128]; snprintf(p,64,"/proc/self/fd/%d",bfd); ssize_t l=readlink(p,t,127); printf("gbm_bo_create OK, fd -> %.*s, stride %u, modifier 0x%llx\n",(int)(l>0?l:0),t,gbm_bo_get_stride(bo),(unsigned long long)gbm_bo_get_modifier(bo)); }
 else printf("gbm_bo_create failed\n");
}
