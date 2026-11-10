#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define P(T,n) T n=(T)eglGetProcAddress(#n)
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,0);glCompileShader(x);return x;}
int main(int argc,char**argv){
 int fd=atoi(argv[1]); unsigned long long size=strtoull(argv[2],0,10); int rfd=atoi(argv[3]),wfd=atoi(argv[4]); const int W=1920,H=1080;
 PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
 EGLDisplay d=gpd(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL); eglInitialize(d,0,0); eglBindAPI(EGL_OPENGL_ES_API);
 EGLint ca[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_NONE}; EGLContext c=eglCreateContext(d,EGL_NO_CONFIG_KHR,EGL_NO_CONTEXT,ca); eglMakeCurrent(d,0,0,c);
 P(PFNGLCREATEMEMORYOBJECTSEXTPROC,glCreateMemoryObjectsEXT); P(PFNGLMEMORYOBJECTPARAMETERIVEXTPROC,glMemoryObjectParameterivEXT);
 P(PFNGLIMPORTMEMORYFDEXTPROC,glImportMemoryFdEXT); P(PFNGLTEXSTORAGEMEM2DEXTPROC,glTexStorageMem2DEXT);
 GLuint mo; glCreateMemoryObjectsEXT(1,&mo); GLint ded=GL_TRUE; glMemoryObjectParameterivEXT(mo,GL_DEDICATED_MEMORY_OBJECT_EXT,&ded);
 glImportMemoryFdEXT(mo,size,GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
 GLuint src; glGenTextures(1,&src); glBindTexture(GL_TEXTURE_2D,src); glTexStorageMem2DEXT(GL_TEXTURE_2D,1,GL_RGBA8,W,H,mo,0);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 // "compositor" output: sample the client texture into our own render target, like Hyprland does
 GLuint rt,fb; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt); glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,W,H);
 glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0); glViewport(0,0,W,H);
 GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,"#version 300 es\nin vec2 a;out vec2 v;void main(){v=a;gl_Position=vec4(a*2.0-1.0,0,1);}"));
 glAttachShader(pr,sh(GL_FRAGMENT_SHADER,"#version 300 es\nprecision mediump float;in vec2 v;uniform sampler2D t;out vec4 o;void main(){o=texture(t,v);}")); glBindAttribLocation(pr,0,"a"); glLinkProgram(pr); glUseProgram(pr);
 float qd[]={0,0,1,0,0,1,1,1}; glVertexAttribPointer(0,2,GL_FLOAT,0,0,qd); glEnableVertexAttribArray(0); glBindTexture(GL_TEXTURE_2D,src);
 fprintf(stderr,"consumer ready, err 0x%x\n",glGetError()); unsigned char one=1; write(wfd,&one,1);
 unsigned char i;
 while(read(rfd,&i,1)==1){
glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char px[4]; glReadPixels(W/2,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);   // stands in for the compositor's own readback/present
  unsigned char ok = px[0]==i && px[1]==(unsigned char)((i*7)%256) && px[2]==255;
  if(!ok) fprintf(stderr,"frame %u: got %u %u %u\n",i,px[0],px[1],px[2]);
  write(wfd,&ok,1);
 }
 return 0;
}
