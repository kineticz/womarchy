// Standard-API zero-copy test: "client" renders into a GBM bo (EGL on the vgem render node, d3d12 underneath) and passes the
// dmabuf-style fd to a "compositor" process, which imports it with EGL_EXT_image_dma_buf_import and samples it.
// mode: flush  = client only glFlush()es before handing over (what a Wayland client does without implicit sync)
//       finish = client glFinish()es first (CPU wait for its own GPU work)
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);return x;}
static void send_fd(int sock,int fd,unsigned char b){struct msghdr m={0};char c[CMSG_SPACE(sizeof(int))];struct iovec io={&b,1};m.msg_iov=&io;m.msg_iovlen=1;
 if(fd>=0){m.msg_control=c;m.msg_controllen=sizeof c;struct cmsghdr*h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(h),&fd,sizeof fd);}
 sendmsg(sock,&m,0);}
static int recv_fd(int sock,unsigned char*b){struct msghdr m={0};char c[CMSG_SPACE(sizeof(int))];struct iovec io={b,1};m.msg_iov=&io;m.msg_iovlen=1;m.msg_control=c;m.msg_controllen=sizeof c;
 if(recvmsg(sock,&m,0)<=0)return -2;struct cmsghdr*h=CMSG_FIRSTHDR(&m);int fd=-1;if(h)memcpy(&fd,CMSG_DATA(h),sizeof fd);return fd;}
static EGLDisplay setup(struct gbm_device**gp){int fd=open("/dev/dri/renderD128",O_RDWR|O_CLOEXEC);*gp=gbm_create_device(fd);
 PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");EGLDisplay d=gpd(EGL_PLATFORM_GBM_KHR,*gp,NULL);eglInitialize(d,0,0);
 eglBindAPI(EGL_OPENGL_ES_API);EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca);eglMakeCurrent(d,0,0,c);return d;}
#define W 1920
#define H 1080
#define NBUF 2
int main(int argc,char**argv){
 int finish=argc>1&&!strcmp(argv[1],"finish"); int N=argc>2?atoi(argv[2]):200; int heavy=argc>3?atoi(argv[3]):40;
 int sv[2]; socketpair(AF_UNIX,SOCK_SEQPACKET,0,sv);
 pid_t p=fork();
 if(!p){ // ---------------- compositor
  close(sv[0]); struct gbm_device*g; EGLDisplay d=setup(&g);
  PFNEGLCREATEIMAGEKHRPROC ci=(void*)eglGetProcAddress("eglCreateImageKHR"); PFNGLEGLIMAGETARGETTEXTURE2DOESPROC tgt=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  GLuint tex[NBUF]; glGenTextures(NBUF,tex);
  for(int b=0;b<NBUF;b++){unsigned char x; int fd=recv_fd(sv[1],&x); unsigned stride=x?0:0; (void)stride;
   EGLint at[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_LINUX_DRM_FOURCC_EXT,GBM_FORMAT_ARGB8888,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,W*4,EGL_NONE};
   EGLImageKHR im=ci(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,at); if(!im){fprintf(stderr,"import failed 0x%x\n",eglGetError());_exit(3);}
   glBindTexture(GL_TEXTURE_2D,tex[b]); tgt(GL_TEXTURE_2D,im); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); close(fd);}
  GLuint rt,fb; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
  glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0); glViewport(0,0,W,H);
  GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,"attribute vec2 a;varying vec2 v;void main(){v=a;gl_Position=vec4(a*2.0-1.0,0,1);}"));
  glAttachShader(pr,sh(GL_FRAGMENT_SHADER,"precision mediump float;varying vec2 v;uniform sampler2D t;void main(){gl_FragColor=texture2D(t,v);}")); glBindAttribLocation(pr,0,"a"); glLinkProgram(pr); glUseProgram(pr);
  float q[]={0,0,1,0,0,1,1,1}; glVertexAttribPointer(0,2,GL_FLOAT,0,0,q); glEnableVertexAttribArray(0);
  unsigned char msg; while(recv_fd(sv[1],&msg)!=-2){ int b=msg&1; glBindTexture(GL_TEXTURE_2D,tex[b]); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
   unsigned char px[4]; glReadPixels(W/2,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px); unsigned char want=(unsigned char)(msg*5);
   unsigned char ok=(px[0]==want||px[2]==want); send_fd(sv[1],-1,ok);} _exit(0);
 }
 // ---------------- client
 close(sv[1]); struct gbm_device*g; EGLDisplay d=setup(&g);
 PFNEGLCREATEIMAGEKHRPROC ci=(void*)eglGetProcAddress("eglCreateImageKHR"); PFNGLEGLIMAGETARGETTEXTURE2DOESPROC tgt=(void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
 GLuint fb[NBUF];
 for(int b=0;b<NBUF;b++){ struct gbm_bo*bo=gbm_bo_create(g,W,H,GBM_FORMAT_ARGB8888,GBM_BO_USE_RENDERING); int fd=gbm_bo_get_fd(bo);
  if(gbm_bo_get_stride(bo)!=W*4) fprintf(stderr,"stride %u\n",gbm_bo_get_stride(bo));
  EGLint at[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_LINUX_DRM_FOURCC_EXT,GBM_FORMAT_ARGB8888,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,W*4,EGL_NONE};
  EGLImageKHR im=ci(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,at); GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t); tgt(GL_TEXTURE_2D,im);
  glGenFramebuffers(1,&fb[b]); glBindFramebuffer(GL_FRAMEBUFFER,fb[b]); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t,0);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fprintf(stderr,"client fbo incomplete\n");
  send_fd(sv[0],fd,0); close(fd);}
 // heavy fragment shader so the GPU work takes a few ms and a missing sync shows up
 GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,"attribute vec2 a;void main(){gl_Position=vec4(a*2.0-1.0,0,1);}"));
 glAttachShader(pr,sh(GL_FRAGMENT_SHADER,"precision highp float;uniform vec4 c;uniform int n;void main(){float s=0.0;for(int i=0;i<64;i++){s+=sin(gl_FragCoord.x*0.001*float(i));} gl_FragColor=c+vec4(s*1e-9);}"));
 glBindAttribLocation(pr,0,"a"); glLinkProgram(pr); glUseProgram(pr); GLint uc=glGetUniformLocation(pr,"c");
 float q[]={0,0,1,0,0,1,1,1}; glVertexAttribPointer(0,2,GL_FLOAT,0,0,q); glEnableVertexAttribArray(0); glViewport(0,0,W,H);
 int bad=0; double tr=0,tc=0;
 for(int i=0;i<N;i++){ int b=i&1; double t0=now(); glBindFramebuffer(GL_FRAMEBUFFER,fb[b]);
  for(int k=0;k<heavy;k++){ float v=(k==heavy-1)?((unsigned char)(i*5))/255.0f:0.3f; glUniform4f(uc,v,0.0f,v,1.0f); glDrawArrays(GL_TRIANGLE_STRIP,0,4);} 
  if(finish) glFinish(); else glFlush();
  double t1=now(); unsigned char msg=(unsigned char)i; send_fd(sv[0],-1,msg); unsigned char ok=0; recv_fd(sv[0],&ok); double t2=now();
  if(!ok) bad++; tr+=t1-t0; tc+=t2-t1; }
 printf("mode %-6s heavy=%d: client render %.2f ms, compositor sample %.2f ms, wrong frames %d/%d\n",finish?"finish":"flush",heavy,tr/N,tc/N,bad,N);
 kill(p,SIGTERM); waitpid(p,0,0); return 0;
}
