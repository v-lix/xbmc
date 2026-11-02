// SPDX-License-Identifier: MIT
// Standalone fanart upload -> AFBC conversion -> blended sampling benchmark.
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <signal.h>
#include <time.h>

#define KNOWN_SHA "949b043bd373a7779d4338a87ecdd929e0430dfddcb092de7a4e6cda3a905397"
#define KNOWN_STRIPPED_SHA "789adf85dc9c630bb049249bf7e28ed40380e919ae1f379a352167d3107ec445"
#define GUARD (64u * 1024u)
#define E_NONE 0x3038
#define E_NATIVE_PIXMAP 0x30b0
#define E_EXTENSIONS 0x3055
#define G_TEXTURE_2D 0x0de1
#define G_RGBA 0x1908
#define G_UNSIGNED_BYTE 0x1401
#define G_FRAMEBUFFER 0x8d40
#define G_COLOR_ATTACHMENT0 0x8ce0
#define G_FRAMEBUFFER_COMPLETE 0x8cd5
#define G_COLOR_BUFFER_BIT 0x4000
#define G_SCISSOR_TEST 0x0c11

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef void *EGLImage;
typedef int EGLint;
typedef unsigned EGLBoolean;
typedef unsigned GLenum;
typedef unsigned GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;

#define API_LIST(X) \
 X(EGLDisplay,eglGetDisplay,(void *)) \
 X(EGLBoolean,eglInitialize,(EGLDisplay,EGLint *,EGLint *)) \
 X(const char *,eglQueryString,(EGLDisplay,EGLint)) \
 X(EGLint,eglGetError,(void)) \
 X(EGLBoolean,eglBindAPI,(unsigned)) \
 X(EGLBoolean,eglChooseConfig,(EGLDisplay,const EGLint *,EGLConfig *,EGLint,EGLint *)) \
 X(EGLContext,eglCreateContext,(EGLDisplay,EGLConfig,EGLContext,const EGLint *)) \
 X(EGLSurface,eglCreatePbufferSurface,(EGLDisplay,EGLConfig,const EGLint *)) \
 X(EGLBoolean,eglMakeCurrent,(EGLDisplay,EGLSurface,EGLSurface,EGLContext)) \
 X(EGLBoolean,eglDestroyContext,(EGLDisplay,EGLContext)) \
 X(EGLBoolean,eglDestroySurface,(EGLDisplay,EGLSurface)) \
 X(EGLBoolean,eglTerminate,(EGLDisplay)) \
 X(void *,eglGetProcAddress,(const char *)) \
 X(EGLImage,eglCreateImageKHR,(EGLDisplay,EGLContext,unsigned,void *,const EGLint *)) \
 X(EGLBoolean,eglDestroyImageKHR,(EGLDisplay,EGLImage)) \
 X(const unsigned char *,glGetString,(GLenum)) \
 X(GLenum,glGetError,(void)) \
 X(void,glGenTextures,(GLsizei,GLuint *)) \
 X(void,glDeleteTextures,(GLsizei,const GLuint *)) \
 X(void,glBindTexture,(GLenum,GLuint)) \
 X(void,glTexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *)) \
 X(void,glTexParameteri,(GLenum,GLenum,GLint)) \
 X(void,glGenFramebuffers,(GLsizei,GLuint *)) \
 X(void,glDeleteFramebuffers,(GLsizei,const GLuint *)) \
 X(void,glBindFramebuffer,(GLenum,GLuint)) \
 X(void,glFramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint)) \
 X(GLenum,glCheckFramebufferStatus,(GLenum)) \
 X(void,glViewport,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,glDisable,(GLenum)) \
 X(void,glEnable,(GLenum)) \
 X(void,glScissor,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,glColorMask,(GLboolean,GLboolean,GLboolean,GLboolean)) \
 X(void,glClearColor,(GLfloat,GLfloat,GLfloat,GLfloat)) \
 X(void,glClear,(unsigned)) \
 X(void,glFinish,(void)) \
 X(void,glReadPixels,(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *)) \
 X(void,glPixelStorei,(GLenum,GLint)) \
 X(GLuint,glCreateShader,(GLenum)) \
 X(void,glShaderSource,(GLuint,GLsizei,const char *const *,const GLint *)) \
 X(void,glCompileShader,(GLuint)) \
 X(void,glGetShaderiv,(GLuint,GLenum,GLint *)) \
 X(void,glGetShaderInfoLog,(GLuint,GLsizei,GLsizei *,char *)) \
 X(void,glDeleteShader,(GLuint)) \
 X(GLuint,glCreateProgram,(void)) \
 X(void,glAttachShader,(GLuint,GLuint)) \
 X(void,glLinkProgram,(GLuint)) \
 X(void,glGetProgramiv,(GLuint,GLenum,GLint *)) \
 X(void,glGetProgramInfoLog,(GLuint,GLsizei,GLsizei *,char *)) \
 X(void,glDeleteProgram,(GLuint)) \
 X(void,glUseProgram,(GLuint)) \
 X(GLint,glGetAttribLocation,(GLuint,const char *)) \
 X(void,glVertexAttribPointer,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void *)) \
 X(void,glEnableVertexAttribArray,(GLuint)) \
 X(GLint,glGetUniformLocation,(GLuint,const char *)) \
 X(void,glUniform1i,(GLint,GLint)) \
 X(void,glUniform1f,(GLint,GLfloat)) \
 X(void,glUniform4f,(GLint,GLfloat,GLfloat,GLfloat,GLfloat)) \
 X(void,glGetIntegerv,(GLenum,GLint *)) \
 X(void,glBlendFuncSeparate,(GLenum,GLenum,GLenum,GLenum)) \
 X(void,glActiveTexture,(GLenum)) \
 X(void,glDrawArrays,(GLenum,GLint,GLsizei)) \
 X(void,glEGLImageTargetTexture2DOES,(GLenum,void *))
#define DECL(ret,name,args) static ret (*p_##name) args;
API_LIST(DECL)
#undef DECL
static uint32_t (*create_mapping)(void *);
static int (*destroy_mapping)(uint32_t);

// Internal registry object, not the public fbdev_pixmap structure.
// Reconstructed ONLY for KNOWN_SHA. See ABI.md and emulation evidence.
struct native_pixmap {
    uint32_t width,height;
    struct { uint32_t pitch,unused,offset; } plane[3];
    uint32_t reserved;
    uint32_t format,format_flags;
    int32_t fd[3];
};
_Static_assert(offsetof(struct native_pixmap,format)==0x30,"format ABI");
_Static_assert(offsetof(struct native_pixmap,fd)==0x38,"FD ABI");
_Static_assert(sizeof(struct native_pixmap)==0x44,"metadata size");

struct ion_allocation { size_t len,align; uint32_t heap_mask,flags; int handle; };
struct ion_fd { int handle,fd; };
struct ion_handle { int handle; };
struct ion_heap { char name[32]; uint32_t type,id,reserved[3]; };
struct ion_query { uint32_t count,reserved0; uint64_t heaps; uint32_t reserved1,reserved2; };
struct dma_heap_allocation { uint64_t len; uint32_t fd,fd_flags; uint64_t heap_flags; };
#define ION_ALLOC _IOWR('I',0,struct ion_allocation)
#define ION_FREE _IOWR('I',1,struct ion_handle)
#define ION_SHARE _IOWR('I',4,struct ion_fd)
#define ION_QUERY _IOWR('I',8,struct ion_query)
#define DMA_HEAP_ALLOC _IOWR('H',0,struct dma_heap_allocation)
#define DMA_BUF_SYNC _IOW('b',0,uint64_t)

static void *library;
static EGLDisplay display;
static EGLContext context;
static EGLSurface surface;
static const char *egl_extensions;
static const char *stage="startup";
static int allocation_uncached=0;
static uint32_t requested_heap_mask=1; // ION system heap; no framebuffer heap.


static void fail(const char *why)
{
    fprintf(stderr,"RESULT=FAIL stage=%s detail=%s errno=%d (%s)\n",stage,why,errno,strerror(errno));
    exit(1);
}
static int known_driver_sha(const char *sha)
{
    return !strcmp(sha,KNOWN_SHA) || !strcmp(sha,KNOWN_STRIPPED_SHA);
}

static volatile sig_atomic_t supervisor_signal;
static void supervise_signal(int sig) { supervisor_signal=sig; }
static double monotonic_seconds(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)<0) fail("clock_gettime");
    return (double)now.tv_sec+(double)now.tv_nsec/1000000000.0;
}
static void terminate_test(pid_t child)
{
    // Only this probe's dedicated child process group; never Kodi or the caller.
    kill(-child,SIGKILL);
    kill(child,SIGKILL);
    for(int i=0;i<50;i++) {
        int status=0;
        pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child || (done<0 && errno==ECHILD)) return;
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }
    // An uninterruptible kernel/GPU wait cannot be repaired by userspace.
    fprintf(stderr,"watchdog_child_still_pending=1 child_pid=%ld\n",(long)child);
}
// Parent returns an exit status. Child returns -1 and executes the actual test.
static int supervise(unsigned seconds)
{
    pid_t parent=getpid();
    double deadline=monotonic_seconds()+seconds;
    pid_t child=fork();
    if(child<0) fail("fork watchdog child");
    if(!child) {
        if(setpgid(0,0)<0) fail("create test process group");
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)<0) fail("set watchdog parent-death signal");
        if(getppid()!=parent) _exit(125);
        return -1;
    }
    // The child also sets its group before touching the driver, closing the race.
    (void)setpgid(child,child);
    struct sigaction action;
    memset(&action,0,sizeof(action));action.sa_handler=supervise_signal;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL)<0 || sigaction(SIGTERM,&action,NULL)<0 ||
       sigaction(SIGHUP,&action,NULL)<0) {
        terminate_test(child);fail("install watchdog signal handlers");
    }
    printf("watchdog_seconds=%u watchdog_child_pid=%ld\n",seconds,(long)child);
    for(;;) {
        int status=0;
        pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child) {
            if(WIFEXITED(status)) return WEXITSTATUS(status);
            if(WIFSIGNALED(status)) {
                fprintf(stderr,"RESULT=CRASH stage=watchdog child_signal=%d\n",WTERMSIG(status));
                return 128+WTERMSIG(status);
            }
        } else if(done<0 && errno!=EINTR) {
            terminate_test(child);fail("waitpid watchdog child");
        }
        if(supervisor_signal) {
            int sig=supervisor_signal;
            terminate_test(child);
            fprintf(stderr,"RESULT=INTERRUPTED stage=watchdog signal=%d\n",sig);
            return 128+sig;
        }
        if(monotonic_seconds()>=deadline) {
            terminate_test(child);
            fprintf(stderr,"RESULT=TIMEOUT stage=watchdog seconds=%u\n",seconds);
            return 124;
        }
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }
}
static const char *safe(const char *s) { return s?s:"(null)"; }
static int extension(const char *list,const char *word)
{
    if(!list || !*word || strchr(word,' ')) return 0;
    size_t n=strlen(word);
    const char *p=list;
    while((p=strstr(p,word))) {
        if((p==list || p[-1]==' ') && (!p[n] || p[n]==' ')) return 1;
        p+=n;
    }
    return 0;
}
static void check_gl(const char *why)
{
    unsigned error=p_glGetError();
    if(error) { fprintf(stderr,"gl_error=0x%x operation=%s\n",error,why);fail(why); }
}
static void check_egl(int success,const char *why)
{
    if(!success) { fprintf(stderr,"egl_error=0x%x operation=%s\n",p_eglGetError(),why);fail(why); }
}
static void *symbol(const char *name)
{
    void *p=dlsym(library,name);
    if(!p && p_eglGetProcAddress) p=p_eglGetProcAddress(name);
    return p;
}
static void load_api(const char *path)
{
    stage="load-library";
    library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!library) { fprintf(stderr,"dlopen=%s\n",dlerror());fail("dlopen libMali"); }
    p_eglGetProcAddress=dlsym(library,"eglGetProcAddress");
#define LOAD(ret,name,args) do { void *p=symbol(#name); memcpy(&p_##name,&p,sizeof(p)); \
    if(!p) { fprintf(stderr,"missing_symbol=%s\n",#name);fail("required API symbol missing"); } } while(0);
    API_LIST(LOAD)
#undef LOAD
    create_mapping=dlsym(library,"egl_create_pixmap_ID_mapping");
    destroy_mapping=dlsym(library,"egl_destroy_pixmap_ID_mapping");
    printf("private_registry_symbols=%s\n",create_mapping && destroy_mapping?"present":"missing");
}
static void init_egl(void)
{
    stage="egl-initialize";
    display=p_eglGetDisplay(NULL);
    check_egl(display!=NULL,"eglGetDisplay");
    int major=0,minor=0;
    check_egl(p_eglInitialize(display,&major,&minor),"eglInitialize");
    printf("egl_version=%d.%d\negl_vendor=%s\n",major,minor,safe(p_eglQueryString(display,0x3053)));
    egl_extensions=p_eglQueryString(display,E_EXTENSIONS);
    printf("egl_extensions=%s\n",safe(egl_extensions));
    printf("standard_dmabuf_import=%d\nstandard_dmabuf_modifiers=%d\n",
        extension(egl_extensions,"EGL_EXT_image_dma_buf_import"),
        extension(egl_extensions,"EGL_EXT_image_dma_buf_import_modifiers"));
    check_egl(p_eglBindAPI(0x30a0),"eglBindAPI GLES");
    EGLConfig config=NULL;
    int count=0;
    int attrs[]={0x3033,1,0x3040,4,0x3024,8,0x3023,8,0x3022,8,0x3021,8,E_NONE};
    check_egl(p_eglChooseConfig(display,attrs,&config,1,&count),"eglChooseConfig");
    int use_pbuffer=count>0;
    if(!use_pbuffer) {
        if(!extension(egl_extensions,"EGL_KHR_surfaceless_context")) fail("no pbuffer or surfaceless context");
        attrs[1]=0;
        check_egl(p_eglChooseConfig(display,attrs,&config,1,&count) && count>0,"choose surfaceless config");
    }
    const int ctx_attrs[]={0x3098,2,E_NONE};
    context=p_eglCreateContext(display,config,NULL,ctx_attrs);
    check_egl(context!=NULL,"eglCreateContext");
    if(use_pbuffer) {
        const int pb_attrs[]={0x3057,1,0x3056,1,E_NONE};
        surface=p_eglCreatePbufferSurface(display,config,pb_attrs);
        check_egl(surface!=NULL,"eglCreatePbufferSurface");
    }
    check_egl(p_eglMakeCurrent(display,surface,surface,context),"eglMakeCurrent");
    printf("gl_vendor=%s\ngl_renderer=%s\ngl_version=%s\ngl_extensions=%s\n",
        safe((const char *)p_glGetString(0x1f00)),safe((const char *)p_glGetString(0x1f01)),
        safe((const char *)p_glGetString(0x1f02)),safe((const char *)p_glGetString(0x1f03)));
    check_gl("initial GL state");
}
static void term_egl(void)
{
    if(display) {
        p_eglMakeCurrent(display,NULL,NULL,NULL);
        if(surface) p_eglDestroySurface(display,surface);
        if(context) p_eglDestroyContext(display,context);
        p_eglTerminate(display);
    }
    // No dlclose: proprietary driver's background threads may outlive eglTerminate.
}
static void query_ion(void)
{
    int ion=open("/dev/ion",O_RDWR|O_CLOEXEC);
    if(ion<0) { printf("ion_open=%s\n",strerror(errno));return; }
    struct ion_query q={0};
    if(ioctl(ion,ION_QUERY,&q)<0) { printf("ion_query=%s\n",strerror(errno));close(ion);return; }
    if(!q.count || q.count>64) { printf("ion_query_count=%u (not used)\n",q.count);close(ion);return; }
    struct ion_heap heaps[64]; memset(heaps,0,sizeof(heaps));
    q.heaps=(uintptr_t)heaps;
    if(ioctl(ion,ION_QUERY,&q)<0) { printf("ion_query_data=%s\n",strerror(errno));close(ion);return; }
    if(q.count>64) fail("ION heap count grew beyond buffer");
    for(unsigned i=0;i<q.count;i++) printf("ion_heap id=%u type=%u name=%.32s\n",heaps[i].id,heaps[i].type,heaps[i].name);
    close(ion);
}
struct allocation { int ion,handle,fd; uint8_t *map; size_t length; };
static void sync_buffer(int fd,int start,int write_access)
{
    // Linux DMA_BUF_SYNC_{READ=1,WRITE=2,START=0,END=4}.
    uint64_t flags=(write_access?2u:1u)|(start?0u:4u);
    int result;
    do { result=ioctl(fd,DMA_BUF_SYNC,&flags); } while(result<0 && errno==EINTR);
    if(result<0) {
        int saved=errno;
        // Our ION allocation is explicitly uncached (flags=0).
        if(allocation_uncached && saved==ENOTTY) printf("cpu_sync stage=%s operation=%s_%s result=ENOTTY fallback=uncached-ion\n",stage,start?"START":"END",write_access?"WRITE":"READ");
        else {
            fprintf(stderr,"cpu_sync stage=%s operation=%s_%s flags=%llu errno=%d (%s)\n",stage,start?"START":"END",write_access?"WRITE":"READ",(unsigned long long)flags,saved,strerror(saved));
            errno=saved;fail("DMA_BUF_IOCTL_SYNC");
        }
    } else {
        printf("cpu_sync stage=%s operation=%s_%s result=PASS\n",stage,start?"START":"END",write_access?"WRITE":"READ");
    }
}
static struct allocation allocate_buffer(size_t length)
{
    stage="allocate-dmabuf";
    struct allocation a={.ion=-1,.handle=-1,.fd=-1,.map=MAP_FAILED,.length=length};
    // Prefer CoreELEC's legacy ION API, using only the system heap by default.
    a.ion=open("/dev/ion",O_RDWR|O_CLOEXEC);
    if(a.ion>=0) {
        struct ion_allocation request={.len=length,.align=4096,.heap_mask=requested_heap_mask,.flags=0,.handle=-1};
        printf("allocator=legacy-ion heap_mask=0x%x uncached=1 bytes=%zu\n",requested_heap_mask,length);
        if(ioctl(a.ion,ION_ALLOC,&request)<0) fail("ION_IOC_ALLOC");
        a.handle=request.handle;allocation_uncached=1;
        struct ion_fd share={a.handle,-1};
        if(ioctl(a.ion,ION_SHARE,&share)<0) fail("ION_IOC_SHARE");
        a.fd=share.fd;
    } else {
        int heap=open("/dev/dma_heap/system",O_RDWR|O_CLOEXEC);
        if(heap<0) fail("neither /dev/ion nor /dev/dma_heap/system available");
        struct dma_heap_allocation request={.len=length,.fd_flags=O_RDWR|O_CLOEXEC};
        if(ioctl(heap,DMA_HEAP_ALLOC,&request)<0) fail("DMA_HEAP_IOCTL_ALLOC");
        a.fd=(int)request.fd;close(heap);
        printf("allocator=dma-heap-system bytes=%zu\n",length);
    }
    a.map=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,a.fd,0);
    if(a.map==MAP_FAILED) fail("mmap allocated DMA-BUF");
    sync_buffer(a.fd,1,1);
    memset(a.map,0,length-GUARD);memset(a.map+length-GUARD,0xa5,GUARD);
    sync_buffer(a.fd,0,1);
    return a;
}
static void release_buffer(struct allocation *a)
{
    munmap(a->map,a->length);close(a->fd);
    if(a->ion>=0) { struct ion_handle h={a->handle};ioctl(a->ion,ION_FREE,&h);close(a->ion); }
}
static void texture_params(void)
{
    p_glTexParameteri(G_TEXTURE_2D,0x2801,0x2601); // MIN_FILTER LINEAR
    p_glTexParameteri(G_TEXTURE_2D,0x2800,0x2601); // MAG_FILTER LINEAR
    p_glTexParameteri(G_TEXTURE_2D,0x2802,0x812f); // CLAMP_TO_EDGE
    p_glTexParameteri(G_TEXTURE_2D,0x2803,0x812f);
}
static void attach(GLuint fbo,GLuint tex)
{
    p_glBindFramebuffer(G_FRAMEBUFFER,fbo);
    p_glFramebufferTexture2D(G_FRAMEBUFFER,G_COLOR_ATTACHMENT0,G_TEXTURE_2D,tex,0);
    check_gl("attach texture");
    unsigned status=p_glCheckFramebufferStatus(G_FRAMEBUFFER);
    printf("framebuffer_status=0x%x\n",status);
    if(status!=G_FRAMEBUFFER_COMPLETE) fail("framebuffer incomplete");
}
static GLuint shader(unsigned type,const char *source)
{
    GLuint s=p_glCreateShader(type);
    p_glShaderSource(s,1,&source,NULL);p_glCompileShader(s);
    int ok=0;p_glGetShaderiv(s,0x8b81,&ok);
    if(!ok) { char log[2048]={0};p_glGetShaderInfoLog(s,sizeof(log)-1,NULL,log);fprintf(stderr,"shader_log=%s\n",log);fail("shader compile"); }
    return s;
}
