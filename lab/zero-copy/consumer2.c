// Shared-fence test (fails on WSL: dxgkrnl rejects cross-process fence fds). GLES 3.0 consumer: import image + timeline semaphore fds, GPU-wait for value 2 (mode "wait") or not ("nowait"), read a pixel.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef GL_SEMAPHORE_TYPE_NV
#define GL_SEMAPHORE_TYPE_NV 0x95B3
#define GL_TIMELINE_SEMAPHORE_NV 0x95B5
#define GL_TIMELINE_SEMAPHORE_VALUE_NV 0x9595
#endif
typedef void (*CSNV)(GLsizei,GLuint*); typedef void (*SPNV)(GLuint,GLenum,const GLint*);
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
#define P(T,n) T n=(T)eglGetProcAddress(#n)
int main(int argc,char**argv){
 int fd=atoi(argv[1]); unsigned long long size=strtoull(argv[2],0,10); int W=atoi(argv[3]),H=atoi(argv[4]),sfd=atoi(argv[5]); int wait=!strcmp(argv[6],"wait");
 double t0=now(); { char p[64],t[128]; for(int f=3;f<8;f++){ snprintf(p,64,"/proc/self/fd/%d",f); ssize_t l=readlink(p,t,127); if(l>0) fprintf(stderr,"consumer fd %d -> %.*s\n",f,(int)l,t);} }
 PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
 EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL); eglInitialize(d,0,0); eglBindAPI(EGL_OPENGL_ES_API);
 EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_NONE}; EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca); eglMakeCurrent(d,0,0,c);
 P(PFNGLCREATEMEMORYOBJECTSEXTPROC,glCreateMemoryObjectsEXT); P(PFNGLMEMORYOBJECTPARAMETERIVEXTPROC,glMemoryObjectParameterivEXT);
 P(PFNGLIMPORTMEMORYFDEXTPROC,glImportMemoryFdEXT); P(PFNGLTEXSTORAGEMEM2DEXTPROC,glTexStorageMem2DEXT);
 P(CSNV,glCreateSemaphoresNV); P(SPNV,glSemaphoreParameterivNV); P(PFNGLIMPORTSEMAPHOREFDEXTPROC,glImportSemaphoreFdEXT);
 P(PFNGLSEMAPHOREPARAMETERUI64VEXTPROC,glSemaphoreParameterui64vEXT); P(PFNGLWAITSEMAPHOREEXTPROC,glWaitSemaphoreEXT);
 GLuint mo; glCreateMemoryObjectsEXT(1,&mo); GLint ded=GL_TRUE; glMemoryObjectParameterivEXT(mo,GL_DEDICATED_MEMORY_OBJECT_EXT,&ded);
 glImportMemoryFdEXT(mo,size,GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
 GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex); glTexStorageMem2DEXT(GL_TEXTURE_2D,1,GL_RGBA8,W,H,mo,0);
 GLuint fb; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 if(wait){ GLuint s; glCreateSemaphoresNV(1,&s); GLint ty=GL_SEMAPHORE_TYPE_TIMELINE_NV; glSemaphoreParameterivNV(s,GL_SEMAPHORE_TYPE_NV,&ty);
   glImportSemaphoreFdEXT(s,GL_HANDLE_TYPE_OPAQUE_FD_EXT,sfd); GLuint64 v=2; glSemaphoreParameterui64vEXT(s,GL_TIMELINE_SEMAPHORE_VALUE_NV,&v);
   GLenum lay=GL_LAYOUT_GENERAL_EXT; glWaitSemaphoreEXT(s,0,0,1,&tex,&lay); }
 fprintf(stderr,"consumer (%s): setup err 0x%x, fbo 0x%x at %.0f ms\n",wait?"wait":"nowait",glGetError(),glCheckFramebufferStatus(GL_FRAMEBUFFER),now()-t0);
 unsigned char px[4]={9,9,9,9}; glReadPixels(W/2,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
 printf("consumer (%s): pixel %u %u %u %u after %.0f ms\n",wait?"wait":"nowait",px[0],px[1],px[2],px[3],now()-t0);
 return !(px[0]==255 && px[2]==64);
}
