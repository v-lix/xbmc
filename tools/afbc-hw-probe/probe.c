// SPDX-License-Identifier: MIT
// Offscreen probe for the exact CoreELEC G52 r12p0 fbdev library.
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
#define W 128
#define H 128
#define CELL 8
#define BYTES (1024u * 1024u)
#define GUARD (64u * 1024u)
#define HEADER_BYTES ((W/16)*(H/16)*16)
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
static const uint8_t colors[4][4]={{255,0,0,255},{0,255,0,128},{0,0,255,64},{255,255,0,0}};

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
struct allocation { int ion,handle,fd; uint8_t *map; };
static void sync_buffer(int fd,int start,int write_access)
{
    // Linux DMA_BUF_SYNC_{READ=1,WRITE=2,START=0,END=4}.
    uint64_t flags=(write_access?2u:1u)|(start?0u:4u);
    if(ioctl(fd,DMA_BUF_SYNC,&flags)<0) {
        // Our ION allocation is explicitly uncached (flags=0).
        if(allocation_uncached && (errno==ENOTTY || errno==EINVAL)) printf("dmabuf_cpu_sync=%s (uncached allocation)\n",strerror(errno));
        else fail("DMA_BUF_IOCTL_SYNC");
    }
}
static struct allocation allocate_buffer(void)
{
    stage="allocate-dmabuf";
    struct allocation a={.ion=-1,.handle=-1,.fd=-1,.map=MAP_FAILED};
    // Prefer CoreELEC's legacy ION API, using only the system heap by default.
    a.ion=open("/dev/ion",O_RDWR|O_CLOEXEC);
    if(a.ion>=0) {
        struct ion_allocation request={.len=BYTES,.align=4096,.heap_mask=requested_heap_mask,.flags=0,.handle=-1};
        printf("allocator=legacy-ion heap_mask=0x%x uncached=1 bytes=%u\n",requested_heap_mask,BYTES);
        if(ioctl(a.ion,ION_ALLOC,&request)<0) fail("ION_IOC_ALLOC");
        a.handle=request.handle;allocation_uncached=1;
        struct ion_fd share={a.handle,-1};
        if(ioctl(a.ion,ION_SHARE,&share)<0) fail("ION_IOC_SHARE");
        a.fd=share.fd;
    } else {
        int heap=open("/dev/dma_heap/system",O_RDWR|O_CLOEXEC);
        if(heap<0) fail("neither /dev/ion nor /dev/dma_heap/system available");
        struct dma_heap_allocation request={.len=BYTES,.fd_flags=O_RDWR|O_CLOEXEC};
        if(ioctl(heap,DMA_HEAP_ALLOC,&request)<0) fail("DMA_HEAP_IOCTL_ALLOC");
        a.fd=(int)request.fd;close(heap);
        printf("allocator=dma-heap-system bytes=%u\n",BYTES);
    }
    a.map=mmap(NULL,BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,a.fd,0);
    if(a.map==MAP_FAILED) fail("mmap allocated DMA-BUF");
    sync_buffer(a.fd,1,1);
    memset(a.map,0,BYTES);memset(a.map+BYTES-GUARD,0xa5,GUARD);
    sync_buffer(a.fd,0,1);
    return a;
}
static void release_buffer(struct allocation *a)
{
    munmap(a->map,BYTES);close(a->fd);
    if(a->ion>=0) { struct ion_handle h={a->handle};ioctl(a->ion,ION_FREE,&h);close(a->ion); }
}
static void texture_params(void)
{
    p_glTexParameteri(G_TEXTURE_2D,0x2801,0x2600); // MIN_FILTER NEAREST
    p_glTexParameteri(G_TEXTURE_2D,0x2800,0x2600); // MAG_FILTER NEAREST
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
static GLuint program(void)
{
    const char *vs="attribute vec2 position; varying vec2 uv; void main(){uv=position*0.5+0.5;gl_Position=vec4(position,0.,1.);}";
    const char *fs="precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}";
    GLuint v=shader(0x8b31,vs),f=shader(0x8b30,fs),p=p_glCreateProgram();
    p_glAttachShader(p,v);p_glAttachShader(p,f);p_glLinkProgram(p);
    int ok=0;p_glGetProgramiv(p,0x8b82,&ok);
    if(!ok) { char log[2048]={0};p_glGetProgramInfoLog(p,sizeof(log)-1,NULL,log);fprintf(stderr,"program_log=%s\n",log);fail("program link"); }
    p_glDeleteShader(v);p_glDeleteShader(f);return p;
}
static unsigned color_index(int x,int y,int pass) { return (unsigned)(x/CELL+2*(y/CELL)+pass)%4; }
static void render_pattern(int pass)
{
    p_glViewport(0,0,W,H);p_glDisable(0x0be2);p_glDisable(0x0bd0); // blend, dither
    p_glDisable(G_SCISSOR_TEST);p_glColorMask(1,1,1,1);
    p_glClearColor(0,0,0,1);p_glClear(G_COLOR_BUFFER_BIT);
    p_glFinish();check_gl("initial complete clear");
    p_glEnable(G_SCISSOR_TEST);
    for(int y=0;y<H;y+=CELL) for(int x=0;x<W;x+=CELL) {
        const uint8_t *c=colors[color_index(x,y,pass)];
        p_glScissor(x,y,CELL,CELL);
        p_glClearColor(c[0]/255.f,c[1]/255.f,c[2]/255.f,c[3]/255.f);
        p_glClear(G_COLOR_BUFFER_BIT);
    }
    p_glDisable(G_SCISSOR_TEST);p_glFinish();check_gl("render imported target");
}
static void sample_and_read(GLuint source,GLuint output_fbo,GLuint p,uint8_t *pixels)
{
    p_glBindFramebuffer(G_FRAMEBUFFER,output_fbo);p_glViewport(0,0,W,H);
    p_glDisable(G_SCISSOR_TEST);p_glDisable(0x0be2);p_glDisable(0x0bd0);
    p_glUseProgram(p);p_glActiveTexture(0x84c0);p_glBindTexture(G_TEXTURE_2D,source);
    int location=p_glGetAttribLocation(p,"position");
    if(location<0) fail("missing shader position attribute");
    const GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
    p_glVertexAttribPointer((GLuint)location,2,0x1406,0,0,vertices);
    p_glEnableVertexAttribArray((GLuint)location);
    int image=p_glGetUniformLocation(p,"image");
    if(image<0) fail("missing shader sampler uniform");
    p_glUniform1i(image,0);p_glDrawArrays(0x0005,0,4);
    p_glPixelStorei(0x0d05,1);p_glReadPixels(0,0,W,H,G_RGBA,G_UNSIGNED_BYTE,pixels);
    p_glFinish();check_gl("sample imported texture into ordinary output target");
}
static int verify(const uint8_t *pixels,int pass)
{
    unsigned mismatches=0,max_error=0;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        const uint8_t *want=colors[color_index(x,y,pass)],*got=pixels+4*(y*W+x);
        int bad=0;
        for(int c=0;c<4;c++) { unsigned delta=(unsigned)abs((int)want[c]-got[c]);if(delta>max_error) max_error=delta;if(delta>1) bad=1; }
        if(bad && mismatches++<4) printf("pixel_mismatch x=%d y=%d got=%u,%u,%u,%u expected=%u,%u,%u,%u\n",x,y,got[0],got[1],got[2],got[3],want[0],want[1],want[2],want[3]);
    }
    printf("pass=%d pixels_checked=%d mismatched_pixels=%u max_channel_error=%u\n",pass,W*H,mismatches,max_error);
    return mismatches==0;
}
static void save(const char *file,const void *data,size_t size)
{
    FILE *f=fopen(file,"wb");if(!f) fail("open output dump");
    if(fwrite(data,1,size,f)!=size || fclose(f)) fail("write output dump");
}
static int test_target(const char *name,int local)
{
    int afbc=!strcmp(name,"afbc") || !strcmp(name,"afbc-split");
    uint32_t format=!strcmp(name,"afbc-split")?0x760bb688u:afbc?0x360bb688u:0x010bb688u;
    struct allocation a={0};struct native_pixmap pix={0};uint32_t id=UINT32_MAX;
    EGLImage image=NULL;GLuint textures[2]={0},fbos[2]={0};
    printf("test=%s target=%dx%d route=%s format=0x%08x\n",name,W,H,local?"local GLES control":"private native-pixmap",format);
    if(!extension((const char *)p_glGetString(0x1f03),"GL_OES_EGL_image") && !local) fail("GL_OES_EGL_image unavailable");
    if(!local) {
        if(!create_mapping || !destroy_mapping) fail("native pixmap registry exports unavailable");
        a=allocate_buffer();pix.width=W;pix.height=H;pix.plane[0].pitch=W*4;pix.format=format;
        pix.fd[0]=a.fd;pix.fd[1]=-1;pix.fd[2]=-1;
        stage="register-native-pixmap";
        id=create_mapping(&pix);
        printf("pixmap_id=%u\n",id);
        if(id==UINT32_MAX) fail("registry allocation");
        stage="egl-image-import";
        const int attrs[]={0x30d2,0,E_NONE}; // EGL_IMAGE_PRESERVED_KHR=false
        image=p_eglCreateImageKHR(display,NULL,E_NATIVE_PIXMAP,(void *)(uintptr_t)id,attrs);
        check_egl(image!=NULL,"eglCreateImageKHR native pixmap");
        puts("image_import=PASS");
    }
    p_glGenTextures(2,textures);p_glGenFramebuffers(2,fbos);
    stage="imported-target-fbo";
    p_glBindTexture(G_TEXTURE_2D,textures[0]);texture_params();
    if(local) p_glTexImage2D(G_TEXTURE_2D,0,G_RGBA,W,H,0,G_RGBA,G_UNSIGNED_BYTE,NULL);
    else p_glEGLImageTargetTexture2DOES(G_TEXTURE_2D,image);
    check_gl("bind imported image as texture");attach(fbos[0],textures[0]);
    puts("renderable_image=PASS");
    stage="output-target-fbo";
    p_glBindTexture(G_TEXTURE_2D,textures[1]);texture_params();
    p_glTexImage2D(G_TEXTURE_2D,0,G_RGBA,W,H,0,G_RGBA,G_UNSIGNED_BYTE,NULL);
    attach(fbos[1],textures[1]);GLuint p=program();
    uint8_t pixels[W*H*4];int all_ok=1;
    for(int pass=0;pass<2;pass++) {
        stage="render-pattern";p_glBindFramebuffer(G_FRAMEBUFFER,fbos[0]);render_pattern(pass);
        stage="sample-and-verify";sample_and_read(textures[0],fbos[1],p,pixels);
        all_ok &= verify(pixels,pass);
    }
    stage="buffer-inspection";
    if(!local) {
        sync_buffer(a.fd,1,0);
        unsigned changed_header=0,changed_total=0,guard_errors=0;
        for(unsigned i=0;i<BYTES-GUARD;i++) { if(a.map[i]) { changed_total++;if(i<HEADER_BYTES) changed_header++; } }
        for(unsigned i=BYTES-GUARD;i<BYTES;i++) if(a.map[i]!=0xa5) guard_errors++;
        printf("header_region_bytes=%d header_nonzero_bytes=%u allocation_nonzero_bytes=%u tail_guard_errors=%u\n",HEADER_BYTES,changed_header,changed_total,guard_errors);
        char file[96];snprintf(file,sizeof(file),"%s-buffer-first-16k.bin",name);save(file,a.map,16384);
        sync_buffer(a.fd,0,0);
        if(guard_errors) all_ok=0;
        if(afbc && !changed_header) { puts("afbc_header_observation=INCONCLUSIVE (header region unchanged)");all_ok=0; }
    }
    char file[96];snprintf(file,sizeof(file),"%s-sampled.rgba",name);save(file,pixels,sizeof(pixels));
    stage="cleanup";p_glFinish();p_glBindFramebuffer(G_FRAMEBUFFER,0);p_glBindTexture(G_TEXTURE_2D,0);
    p_glDeleteFramebuffers(2,fbos);p_glDeleteTextures(2,textures);p_glDeleteProgram(p);
    if(image) check_egl(p_eglDestroyImageKHR(display,image),"eglDestroyImageKHR");
    if(id!=UINT32_MAX && !destroy_mapping(id)) fail("release native pixmap registry entry");
    if(!local) release_buffer(&a);
    check_gl("cleanup");
    printf("RESULT=%s test=%s scope=offscreen-import-render-sample-readback\n",all_ok?"PASS":"FAIL",name);
    return all_ok?0:1;
}
static void self_test(void)
{
    if(!known_driver_sha(KNOWN_SHA) || !known_driver_sha(KNOWN_STRIPPED_SHA) ||
       known_driver_sha("unrecognized")) fail("driver identity allowlist self-test");
    if(!extension("ABC DEF GHI","DEF") || extension("ABC DEFG GHI","DEF") || extension("","DEF")) fail("extension matching self-test");
    uint8_t pixels[W*H*4];
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) memcpy(pixels+4*(y*W+x),colors[color_index(x,y,0)],4);
    if(!verify(pixels,0)) fail("positive pixel verifier self-test");
    pixels[0]=0;if(verify(pixels,0)) fail("negative pixel verifier self-test");
    struct native_pixmap pix={0};pix.width=W;pix.height=H;pix.plane[0].pitch=W*4;pix.format=0x360bb688;pix.fd[0]=3;pix.fd[1]=pix.fd[2]=-1;
    save("metadata-selftest.bin",&pix,sizeof(pix));
    printf("pointer_bits=%zu metadata_size=%zu format_offset=%zu fd_offset=%zu\n",8*sizeof(void *),sizeof(pix),offsetof(struct native_pixmap,format),offsetof(struct native_pixmap,fd));
    puts("RESULT=PASS test=self-test");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    const char *path="/usr/lib/libMali.so",*name="info",*sha="";
    unsigned watchdog_seconds=60;
    int selftest=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--self-test")) selftest=1;
        else if(!strcmp(argv[i],"--watchdog") && i+1<argc) {
            char *end=NULL;unsigned long seconds=strtoul(argv[++i],&end,10);
            if(!*argv[i] || *end || seconds<1 || seconds>300) fail("watchdog must be 1..300 seconds");
            watchdog_seconds=(unsigned)seconds;
        }
        else if(!strcmp(argv[i],"--library") && i+1<argc) path=argv[++i];
        else if(!strcmp(argv[i],"--case") && i+1<argc) name=argv[++i];
        else if(!strcmp(argv[i],"--driver-sha256") && i+1<argc) sha=argv[++i];
        else if(!strcmp(argv[i],"--heap-mask") && i+1<argc) {
            char *end=NULL;unsigned long mask=strtoul(argv[++i],&end,0);
            if(!*argv[i] || *end || !mask || mask>UINT32_MAX || (mask & ((1u<<6)|(1u<<7)))) fail("invalid heap mask or framebuffer/secure heap requested");
            requested_heap_mask=(uint32_t)mask;
        } else fail("usage: probe [--watchdog SECONDS] [--self-test | --case info|local|linear|afbc|afbc-split] [--library PATH] [--driver-sha256 HASH] [--heap-mask MASK]");
    }
    int supervised=supervise(watchdog_seconds);
    if(supervised>=0) return supervised;
    if(selftest) { self_test();return 0; }
    int info=!strcmp(name,"info"),local=!strcmp(name,"local");
    if(!info && !local && strcmp(name,"linear") && strcmp(name,"afbc") && strcmp(name,"afbc-split")) fail("unknown test case");
    if(!info && !local && (sizeof(void *)!=4 || !known_driver_sha(sha))) {
        puts("RESULT=SKIP reason=private-ABI-requires-exact-known-32-bit-library");return 2;
    }
    printf("probe_version=2 pointer_bits=%zu library=%s\n",8*sizeof(void *),path);
    load_api(path);query_ion();init_egl();
    int result=0;
    if(info) puts("RESULT=PASS test=info scope=capabilities-only");
    else result=test_target(name,local);
    term_egl();return result;
}
