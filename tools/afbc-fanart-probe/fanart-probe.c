// SPDX-License-Identifier: MIT
// Private AFBC formats are permitted only on the two byte-verified ARM32 builds.
#include "platform.h"
#include <limits.h>
#include <math.h>
#include <sched.h>
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define VERSION 2
#define GL_BLEND 0x0be2
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TIME_ELAPSED_EXT 0x88bf
#define GL_GPU_DISJOINT_EXT 0x8fbb
#define GL_QUERY_RESULT_EXT 0x8866

static void (*qGen)(GLsizei,GLuint *);
static void (*qDelete)(GLsizei,const GLuint *);
static void (*qBegin)(GLenum,GLuint);
static void (*qEnd)(GLenum);
static void (*qResult)(GLuint,GLenum,uint64_t *);
static void (*qInfo)(GLenum,GLenum,GLint *);
static int timers;
static unsigned width=3840,height=2160,frames=60,rounds=2,cycles=1,fps=60;
static const char *pattern="detail",*profile="afbc",*image_path[2];
static int local_control;
static size_t pixel_bytes;
static FILE *measurements;

static int memory_check;

static void *checked_malloc(size_t n)
{
    void *p=malloc(n);
    if(!p) fail("allocate CPU memory");
    return p;
}
static size_t align_up(size_t n,size_t alignment)
{
    if(n>SIZE_MAX-(alignment-1)) fail("layout size overflow");
    return (n+alignment-1)&~(alignment-1);
}
struct layout { size_t aw,ah,header,body,capacity; };
static struct layout afbc_layout(unsigned w,unsigned h)
{
    // Exact formats 0x360bb688/0x760bb688 only: 16x16 sparse RGBA8.
    // Checked against the unchanged driver's external-wrapper calculations.
    if(!w || !h || w>4096 || h>4096) fail("dimensions must be 1..4096");
    struct layout l={.aw=align_up(w,16),.ah=align_up(h,16)};
    l.header=align_up((l.aw/16)*(l.ah/16)*16,1024);
    l.body=l.aw*l.ah*4;
    l.capacity=align_up(l.header+l.body,4096)+GUARD;
    return l;
}
static uint64_t checksum(const uint8_t *data,size_t n)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<n;i++) { hash^=data[i];hash*=UINT64_C(1099511628211); }
    return hash;
}
struct difference { size_t pixels,channels; unsigned max_error; };
static struct difference compare_pixels(const uint8_t *a,const uint8_t *b,size_t bytes,unsigned tolerance)
{
    struct difference d={0};
    for(size_t i=0;i<bytes;i+=4) {
        int bad=0;
        for(unsigned c=0;c<4;c++) {
            unsigned delta=(unsigned)abs((int)a[i+c]-b[i+c]);
            if(delta>d.max_error) d.max_error=delta;
            if(delta>tolerance) { bad=1;d.channels++; }
        }
        if(bad) d.pixels++;
    }
    return d;
}
static uint32_t random_next(uint32_t *state)
{
    uint32_t x=*state;x^=x<<13;x^=x>>17;x^=x<<5;*state=x;return x;
}
static uint8_t *generate_image(unsigned which)
{
    uint8_t *data=checked_malloc(pixel_bytes);
    uint32_t rng=0x6d2b79f5u+which*0x1234567u;
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
        uint8_t *p=data+4*((size_t)y*width+x);
        if(!strcmp(pattern,"noise")) {
            uint32_t n=random_next(&rng);p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=255;
        } else {
            unsigned u=x*255/(width>1?width-1:1),v=y*255/(height>1?height-1:1);
            unsigned detail=!strcmp(pattern,"gradient")?0:((x/7+y/11+which*3)%17)*4;
            p[0]=(uint8_t)(u+detail+which*47);
            p[1]=(uint8_t)(v+detail+which*83);
            p[2]=(uint8_t)((u+v)/2+detail+which*29);
            p[3]=!strcmp(pattern,"alpha")?(uint8_t)((x*13+y*7+which*53)&255):255;
        }
    }
    return data;
}
static uint8_t *decode_image(const char *path)
{
    int w=0,h=0,channels=0;
    if(!stbi_info(path,&w,&h,&channels) || w<1 || h<1 || w>8192 || h>8192 ||
       (uint64_t)w*h>UINT64_C(33554432)) fail("JPEG/PNG dimensions invalid or over 32 megapixels");
    uint8_t *decoded=stbi_load(path,&w,&h,&channels,4);
    if(!decoded) { fprintf(stderr,"decode_error=%s\n",safe(stbi_failure_reason()));fail("decode image"); }
    printf("image_input=%s decoded=%dx%d components=%d exif_orientation_applied=0\n",path,w,h,channels);
    if((unsigned)w==width && (unsigned)h==height) return decoded;
    // Bilinear resize outside all measured GPU sections. Aspect ratio is stretched
    // to the requested size, intentionally identical for ordinary and AFBC routes.
    uint8_t *out=checked_malloc(pixel_bytes);
    for(unsigned y=0;y<height;y++) {
        double sy=((double)y+0.5)*h/height-0.5;
        if(sy<0) sy=0;
        unsigned y0=(unsigned)sy,y1=y0+1<(unsigned)h?y0+1:y0;
        double fy=sy-y0;
        for(unsigned x=0;x<width;x++) {
            double sx=((double)x+0.5)*w/width-0.5;
            if(sx<0) sx=0;
            unsigned x0=(unsigned)sx,x1=x0+1<(unsigned)w?x0+1:x0;
            double fx=sx-x0;
            for(unsigned c=0;c<4;c++) {
                double a=decoded[4*((size_t)y0*w+x0)+c]*(1-fx)+decoded[4*((size_t)y0*w+x1)+c]*fx;
                double b=decoded[4*((size_t)y1*w+x0)+c]*(1-fx)+decoded[4*((size_t)y1*w+x1)+c]*fx;
                out[4*((size_t)y*width+x)+c]=(uint8_t)(a*(1-fy)+b*fy+0.5);
            }
        }
    }
    stbi_image_free(decoded);return out;
}
static GLuint make_program(void)
{
    const char *vs="attribute vec2 position; varying highp vec2 uv; uniform vec4 transform;"
        "void main(){uv=(position*.5+.5)*transform.xy+transform.zw;gl_Position=vec4(position,0.,1.);}";
    const char *fs="precision highp float; varying highp vec2 uv; uniform sampler2D image;"
        "uniform float opacity; void main(){vec4 c=texture2D(image,uv);gl_FragColor=vec4(c.rgb,c.a*opacity);}";
    GLuint v=shader(0x8b31,vs),f=shader(0x8b30,fs),p=p_glCreateProgram();
    p_glAttachShader(p,v);p_glAttachShader(p,f);p_glLinkProgram(p);
    int ok=0;p_glGetProgramiv(p,0x8b82,&ok);
    if(!ok) { char log[2048]={0};p_glGetProgramInfoLog(p,sizeof(log)-1,NULL,log);fprintf(stderr,"program_log=%s\n",log);fail("link program"); }
    p_glDeleteShader(v);p_glDeleteShader(f);return p;
}
struct renderer { GLuint program; GLint position,opacity,transform; };
static struct renderer renderer;
static void init_renderer(void)
{
    renderer.program=make_program();
    renderer.position=p_glGetAttribLocation(renderer.program,"position");
    renderer.opacity=p_glGetUniformLocation(renderer.program,"opacity");
    renderer.transform=p_glGetUniformLocation(renderer.program,"transform");
    if(renderer.position<0 || renderer.opacity<0 || renderer.transform<0) fail("shader locations");
    p_glUseProgram(renderer.program);
    GLint sampler=p_glGetUniformLocation(renderer.program,"image");
    if(sampler<0) fail("sampler location");
    p_glUniform1i(sampler,0);
    const char *ext=(const char *)p_glGetString(0x1f03);
    timers=extension(ext,"GL_EXT_disjoint_timer_query");
#define OPTIONAL(pointer,name) do { void *ptr=symbol(name);memcpy(&pointer,&ptr,sizeof(ptr));if(!ptr) timers=0; } while(0)
    OPTIONAL(qGen,"glGenQueriesEXT");OPTIONAL(qDelete,"glDeleteQueriesEXT");
    OPTIONAL(qBegin,"glBeginQueryEXT");OPTIONAL(qEnd,"glEndQueryEXT");
    OPTIONAL(qResult,"glGetQueryObjectui64vEXT");
    OPTIONAL(qInfo,"glGetQueryivEXT");
#undef OPTIONAL
    GLint counter_bits=0;
    if(timers) { qInfo(GL_TIME_ELAPSED_EXT,0x8864,&counter_bits);if(counter_bits<1) timers=0; }
    check_gl("timer capability query");
    printf("gpu_timer_available=%d elapsed_counter_bits=%d\n",timers,counter_bits);
}
static void draw(GLuint texture,float opacity,int filtered)
{
    static const GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
    p_glUseProgram(renderer.program);p_glActiveTexture(0x84c0);
    p_glBindTexture(G_TEXTURE_2D,texture);
    p_glUniform1f(renderer.opacity,opacity);
    // Fractional crop exercises bilinear filtering and scaling at 4K.
    p_glUniform4f(renderer.transform,filtered?0.937f:1.f,filtered?0.913f:1.f,
                  filtered?0.0217f:0.f,filtered?0.0431f:0.f);
    p_glVertexAttribPointer((GLuint)renderer.position,2,0x1406,0,0,vertices);
    p_glEnableVertexAttribArray((GLuint)renderer.position);
    p_glDrawArrays(0x0005,0,4);
}
struct target {
    GLuint texture,fbo;
    EGLImage image;
    uint32_t mapping;
    struct native_pixmap pixmap; // Registry retains this exact object's address.
    struct allocation allocation;
};
static void create_target(struct target *t,int external)
{
    memset(t,0,sizeof(*t));t->mapping=UINT32_MAX;
    if(external) {
        struct layout l=afbc_layout(width,height);
        t->allocation=allocate_buffer(l.capacity);
        t->pixmap.width=width;t->pixmap.height=height;t->pixmap.plane[0].pitch=width*4;
        t->pixmap.format=!strcmp(profile,"afbc-split")?0x760bb688:0x360bb688;
        t->pixmap.fd[0]=t->allocation.fd;t->pixmap.fd[1]=t->pixmap.fd[2]=-1;
        t->mapping=create_mapping(&t->pixmap);
        if(t->mapping==UINT32_MAX) fail("register AFBC pixmap");
        const int attrs[]={0x30d2,0,E_NONE};
        t->image=p_eglCreateImageKHR(display,NULL,E_NATIVE_PIXMAP,(void *)(uintptr_t)t->mapping,attrs);
        check_egl(t->image!=NULL,"import AFBC image");
    }
    p_glGenTextures(1,&t->texture);p_glBindTexture(G_TEXTURE_2D,t->texture);texture_params();
    if(external) p_glEGLImageTargetTexture2DOES(G_TEXTURE_2D,t->image);
    else p_glTexImage2D(G_TEXTURE_2D,0,G_RGBA,width,height,0,G_RGBA,G_UNSIGNED_BYTE,NULL);
    p_glGenFramebuffers(1,&t->fbo);attach(t->fbo,t->texture);
}
static void destroy_target(struct target *t)
{
    p_glDeleteFramebuffers(1,&t->fbo);p_glDeleteTextures(1,&t->texture);
    if(t->image) check_egl(p_eglDestroyImageKHR(display,t->image),"destroy AFBC image");
    if(t->mapping!=UINT32_MAX) {
        if(!destroy_mapping(t->mapping)) fail("release pixmap mapping");
        release_buffer(&t->allocation);
    }
}
static void setup_frame(GLuint fbo)
{
    p_glBindFramebuffer(G_FRAMEBUFFER,fbo);p_glViewport(0,0,width,height);
    p_glDisable(G_SCISSOR_TEST);p_glDisable(0x0bd0);p_glColorMask(1,1,1,1);
    p_glClearColor(0,0,0,0);p_glClear(G_COLOR_BUFFER_BIT);
}
static void crossfade(const GLuint textures[2],GLuint output,float fade,int filtered)
{
    setup_frame(output);
    p_glEnable(GL_BLEND);
    // CGUITextureGLES::BeginDraw: RGB straight-alpha, alpha accumulates "over".
    p_glBlendFuncSeparate(0x0302,0x0303,0x0307,1);
    draw(textures[0],1.f-fade,filtered);draw(textures[1],fade,filtered);
}
static void read_pixels(uint8_t *pixels)
{
    p_glPixelStorei(0x0d05,1);p_glReadPixels(0,0,width,height,G_RGBA,G_UNSIGNED_BYTE,pixels);
    p_glFinish();check_gl("read complete output");
}
static int record_comparison(const char *name,const uint8_t *a,const uint8_t *b,unsigned tolerance)
{
    struct difference d=compare_pixels(a,b,pixel_bytes,tolerance);
    printf("correctness=%s pixels=%zu tolerance=%u mismatched_pixels=%zu max_channel_error=%u baseline_fnv1a=%016llx candidate_fnv1a=%016llx\n",
        name,pixel_bytes/4,tolerance,d.pixels,d.max_error,
        (unsigned long long)checksum(a,pixel_bytes),(unsigned long long)checksum(b,pixel_bytes));
    if(d.pixels) {
        for(size_t i=0,shown=0;i<pixel_bytes && shown<4;i+=4) {
            if(compare_pixels(a+i,b+i,4,tolerance).pixels) {
                printf("mismatch pixel=%zu baseline=%u,%u,%u,%u candidate=%u,%u,%u,%u\n",i/4,
                    a[i],a[i+1],a[i+2],a[i+3],b[i],b[i+1],b[i+2],b[i+3]);shown++;
            }
        }
    }
    return !d.pixels;
}
static int verify_targets(const GLuint ordinary[2],const GLuint candidate[2],GLuint output,int linear)
{
    uint8_t *a=checked_malloc(pixel_bytes),*b=checked_malloc(pixel_bytes);
    int ok=1;
    for(unsigned i=0;i<2;i++) {
        setup_frame(output);p_glDisable(GL_BLEND);draw(ordinary[i],1.f,0);read_pixels(a);
        setup_frame(output);p_glDisable(GL_BLEND);draw(candidate[i],1.f,0);read_pixels(b);
        // AFBC is lossless. No tolerance for the conversion/identity case.
        ok &= record_comparison(i?"identity-B":"identity-A",a,b,0);
    }
    static const float fades[]={0.f,0.125f,0.5f,0.875f,1.f};
    for(unsigned filtered=0;filtered<2;filtered++) for(unsigned f=0;f<5;f++) {
        crossfade(ordinary,output,fades[f],filtered);read_pixels(a);
        crossfade(candidate,output,fades[f],filtered);read_pixels(b);
        char name[80];snprintf(name,sizeof(name),"fade-%.3f-%s-%s",fades[f],linear?"bilinear":"nearest",filtered?"scaled":"identity");
        ok &= record_comparison(name,a,b,1);
    }
    free(a);free(b);return ok;
}
struct sample { double submit_ms,completed_ms,gpu_ms; unsigned valid; };
static int sort_double(const void *a,const void *b)
{
    double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);
}
static void report_stat(const char *route,const char *metric,double *values,size_t n)
{
    if(!n) { printf("timing route=%s metric=%s valid_samples=0\n",route,metric);return; }
    double total=0;size_t over=0;
    for(size_t i=0;i<n;i++) { total+=values[i];if(values[i]>1000./60) over++; }
    qsort(values,n,sizeof(*values),sort_double);
    printf("timing route=%s metric=%s valid_samples=%zu mean_ms=%.6f p50_ms=%.6f p95_ms=%.6f p99_ms=%.6f max_ms=%.6f over_16_667ms=%zu\n",
        route,metric,n,total/n,values[(n-1)/2],values[(95*n+99)/100-1],
        values[(99*n+99)/100-1],values[n-1],over);
}
static void pace(double start)
{
    if(!fps) return;
    double left=start+1./fps-monotonic_seconds();
    if(left>0) {
        struct timespec delay={(time_t)left,(long)((left-(time_t)left)*1e9)};
        while(nanosleep(&delay,&delay)<0 && errno==EINTR) {}
    }
}
static void benchmark(const GLuint ordinary[2],const GLuint candidate[2],GLuint output,unsigned cycle)
{
    size_t per_route=(size_t)rounds*2*frames;
    struct sample *samples[2]={checked_malloc(per_route*sizeof(struct sample)),checked_malloc(per_route*sizeof(struct sample))};
    size_t count[2]={0,0};GLuint query=0;
    if(timers) qGen(1,&query);
    for(unsigned r=0;r<rounds;r++) for(unsigned block=0;block<4;block++) {
        unsigned route=((block==1 || block==2)?1u:0u)^(r&1u);
        const GLuint *textures=route?candidate:ordinary;
        // ABBA then BAAB balances phase/temperature effects. Warm both routes.
        for(unsigned warm=0;warm<4;warm++) {
            double start=monotonic_seconds();crossfade(textures,output,0.5f,1);p_glFinish();pace(start);
        }
        for(unsigned f=0;f<frames;f++) {
            struct sample *s=&samples[route][count[route]++];memset(s,0,sizeof(*s));
            GLint pre_disjoint=0,post_disjoint=0;
            if(timers) { p_glGetIntegerv(GL_GPU_DISJOINT_EXT,&pre_disjoint);qBegin(GL_TIME_ELAPSED_EXT,query); }
            double start=monotonic_seconds();
            // Smooth triangular fade, identical sequence for both routes.
            unsigned phase=f%120;float fade=phase<60?phase/59.f:(119-phase)/59.f;
            crossfade(textures,output,fade,1);
            double submitted=monotonic_seconds();
            if(timers) qEnd(GL_TIME_ELAPSED_EXT);
            p_glFinish();double completed=monotonic_seconds();
            s->submit_ms=(submitted-start)*1000;s->completed_ms=(completed-start)*1000;
            if(timers) {
                uint64_t ns=0;qResult(query,GL_QUERY_RESULT_EXT,&ns);p_glGetIntegerv(GL_GPU_DISJOINT_EXT,&post_disjoint);
                if(!pre_disjoint && !post_disjoint && ns>0) { s->gpu_ms=(double)ns/1e6;s->valid=1; }
            }
            fprintf(measurements,"%u,%u,%u,%s,%u,%.9f,%.6f,%.6f,%.6f,%u\n",cycle,r,block,
                route?"candidate":"ordinary",f,start,s->submit_ms,s->completed_ms,s->gpu_ms,s->valid);
            pace(start);
        }
        check_gl("crossfade timing block");fflush(measurements);
    }
    if(timers) qDelete(1,&query);
    double *values=checked_malloc(per_route*sizeof(double));
    for(unsigned route=0;route<2;route++) {
        const char *name=route?(local_control?"ordinary-rendered-copy":profile):"ordinary-upload";
        for(unsigned metric=0;metric<3;metric++) {
            size_t n=0;
            for(size_t i=0;i<count[route];i++) {
                struct sample *s=&samples[route][i];
                if(metric!=2 || s->valid) values[n++]=metric==0?s->submit_ms:metric==1?s->completed_ms:s->gpu_ms;
            }
            report_stat(name,metric==0?"cpu-submit":metric==1?"wall-completed":"gpu-elapsed",values,n);
        }
        free(samples[route]);
    }
    free(values);
}
static int check_guard(struct target *t,unsigned index)
{
    if(local_control) return 1;
    struct allocation *a=&t->allocation;
    size_t errors=0,zeroed=0,first=0,last=0;unsigned first_value=0;sync_buffer(a->fd,1,0);
    for(size_t i=a->length-GUARD;i<a->length;i++) if(a->map[i]!=0xa5) {
        if(!errors) { first=i-(a->length-GUARD);first_value=a->map[i]; }
        last=i-(a->length-GUARD);zeroed+=a->map[i]==0;errors++;
    }
    // Header sample only; neither nonzero counts nor reserved bytes are ratios.
    size_t nonzero=0,header=afbc_layout(width,height).header;
    for(size_t i=0;i<header;i++) if(a->map[i]) nonzero++;
    sync_buffer(a->fd,0,0);
    printf("buffer=%u header_bytes=%zu header_nonzero_bytes=%zu reserved_bytes=%zu tail_guard_errors=%zu\n",index,header,nonzero,a->length,errors);
    if(errors) printf("guard_damage buffer=%u zeroed=%zu first_guard_offset=%zu last_guard_offset=%zu first_allocation_offset=%zu first_value=0x%02x expected=0xa5\n",index,zeroed,first,last,a->length-GUARD+first,first_value);
    // An all-zero header can represent an all-zero solid image; pixel equality
    // and the guard determine correctness, not the number of nonzero bytes.
    return errors==0;
}
static int run_cycle(unsigned cycle)
{
    struct target candidates[2],output;
    GLuint ordinary[2]={0},converted[2]={0};
    printf("cycle=%u start=1\n",cycle);
    create_target(&output,0);
    p_glGenTextures(2,ordinary);
    for(unsigned i=0;i<2;i++) {
        stage="decode-or-generate";double start=monotonic_seconds();
        uint8_t *pixels=!strcmp(pattern,"photos")?decode_image(image_path[i]):generate_image(i);
        double prepare_ms=(monotonic_seconds()-start)*1000;
        printf("image=%u prepare_cpu_ms=%.6f source_rgba_fnv1a=%016llx\n",i,prepare_ms,(unsigned long long)checksum(pixels,pixel_bytes));
        stage="ordinary-upload";p_glBindTexture(G_TEXTURE_2D,ordinary[i]);texture_params();
        // Exact texel conversion first. Fade uses bilinear filtering afterwards.
        p_glTexParameteri(G_TEXTURE_2D,0x2801,GL_NEAREST);p_glTexParameteri(G_TEXTURE_2D,0x2800,GL_NEAREST);
        p_glPixelStorei(0x0cf5,1);start=monotonic_seconds();
        p_glTexImage2D(G_TEXTURE_2D,0,G_RGBA,width,height,0,G_RGBA,G_UNSIGNED_BYTE,pixels);
        p_glFinish();check_gl("ordinary image upload");
        printf("image=%u upload_completed_ms=%.6f\n",i,(monotonic_seconds()-start)*1000);
        // Check upload orientation/channels independently of the AFBC comparison.
        uint8_t *readback=checked_malloc(pixel_bytes);
        setup_frame(output.fbo);p_glDisable(GL_BLEND);draw(ordinary[i],1,0);read_pixels(readback);
        if(!record_comparison(i?"CPU-upload-B":"CPU-upload-A",pixels,readback,0)) fail("ordinary upload changed pixels");
        free(readback);stbi_image_free(pixels);
        stage="allocate-and-import-candidate";start=monotonic_seconds();
        create_target(&candidates[i],!local_control);converted[i]=candidates[i].texture;
        printf("image=%u allocate_import_ms=%.6f\n",i,(monotonic_seconds()-start)*1000);
        stage="convert-once";start=monotonic_seconds();
        setup_frame(candidates[i].fbo);p_glDisable(GL_BLEND);draw(ordinary[i],1,0);
        p_glFinish();check_gl("one-time GPU conversion");
        printf("image=%u conversion_completed_ms=%.6f\n",i,(monotonic_seconds()-start)*1000);
        // Identity is checked with nearest first, avoiding interpolation rounding.
        p_glBindTexture(G_TEXTURE_2D,converted[i]);
        p_glTexParameteri(G_TEXTURE_2D,0x2801,GL_NEAREST);p_glTexParameteri(G_TEXTURE_2D,0x2800,GL_NEAREST);
    }
    stage="verify-nearest-and-blending";
    // First verification uses nearest source filtering, then bilinear verification.
    int ok=verify_targets(ordinary,converted,output.fbo,0);
    for(unsigned i=0;i<2;i++) {
        p_glBindTexture(G_TEXTURE_2D,ordinary[i]);texture_params();
        p_glBindTexture(G_TEXTURE_2D,converted[i]);texture_params();
    }
    stage="verify-bilinear-and-blending";ok &= verify_targets(ordinary,converted,output.fbo,1);
    if(ok) { stage="measure-crossfades";benchmark(ordinary,converted,output.fbo,cycle); }
    stage="inspect-allocation-guards";p_glFinish();
    for(unsigned i=0;i<2;i++) ok &= check_guard(&candidates[i],i);
    stage="release-cycle";p_glBindFramebuffer(G_FRAMEBUFFER,0);p_glBindTexture(G_TEXTURE_2D,0);
    destroy_target(&output);
    for(unsigned i=0;i<2;i++) destroy_target(&candidates[i]);
    p_glDeleteTextures(2,ordinary);check_gl("cycle cleanup");
    printf("cycle=%u result=%s\n",cycle,ok?"PASS":"FAIL");return ok;
}
static unsigned number(const char *text,unsigned low,unsigned high)
{
    char *end=NULL;errno=0;unsigned long value=strtoul(text,&end,10);
    if(!*text || *end || errno || value<low || value>high) fail("numeric argument out of range");
    return (unsigned)value;
}

static size_t memory_errors(struct allocation *a,const char *when)
{
    stage=when;sync_buffer(a->fd,1,0);
    size_t bad=0,zeroed=0,first=0,last=0;unsigned first_value=0;
    for(size_t i=0;i<a->length;i++) if(a->map[i]!=0xa5) {
        if(!bad) { first=i;first_value=a->map[i]; }
        bad++;zeroed+=a->map[i]==0;last=i;
    }
    sync_buffer(a->fd,0,0);
    printf("memory_check stage=%s checked_bytes=%zu changed_bytes=%zu zeroed_bytes=%zu first_offset=%zu last_offset=%zu first_value=0x%02x expected=0xa5\n",when,a->length,bad,zeroed,first,last,first_value);
    return bad;
}

static int test_memory(void)
{
    // Pin only this watchdog child, before allocation/zeroing, so cached kernel
    // zeros and subsequent cache pressure are produced on the same CPU.
    stage="memory-affinity";
    int cpu=sched_getcpu();cpu_set_t affinity;CPU_ZERO(&affinity);
    if(cpu<0 || cpu>=CPU_SETSIZE) fail("get current CPU");
    CPU_SET(cpu,&affinity);
    if(sched_setaffinity(0,sizeof(affinity),&affinity)<0) fail("pin memory-test child");
    const size_t pressure_bytes=32u*1024u*1024u;
    uint8_t *pressure=checked_malloc(pressure_bytes);
    memset(pressure,0,pressure_bytes); // Allocate/prefault BEFORE fresh ION pages.
    struct layout layout=afbc_layout(width,height);
    printf("fanart_probe_version=%d memory_only=1 cpu=%d size=%ux%u allocation_bytes=%zu pressure_bytes=%zu egl_loaded=0 mali_loaded=0\n",VERSION,cpu,width,height,layout.capacity,pressure_bytes);
    struct allocation a=allocate_buffer(layout.capacity);
    stage="memory-fill";sync_buffer(a.fd,1,1);memset(a.map,0xa5,a.length);
    __sync_synchronize();sync_buffer(a.fd,0,1);
    size_t before=memory_errors(&a,"before-cache-pressure");
    volatile uint8_t *p=pressure;
    for(unsigned round=0;round<8;round++)
        for(size_t i=0;i<pressure_bytes;i+=64) p[i]=(uint8_t)(p[i]+1);
    __sync_synchronize();
    size_t after=memory_errors(&a,"after-cache-pressure");
    release_buffer(&a);free(pressure);
    printf("RESULT=%s test=uncached-ion-memory scope=cpu-only-no-import-no-gpu\n",before || after?"FAIL":"PASS");
    return before || after?1:0;
}
static void self_test(void)
{
    struct layout l=afbc_layout(3840,2160);
    if(l.header!=519168 || l.body!=33177600 || l.capacity!=33763328) fail("4K allocation formula");
    l=afbc_layout(3839,2159);if(l.header!=519168 || l.aw!=3840 || l.ah!=2160) fail("odd dimensions layout");
    l=afbc_layout(1920,1080);if(l.header!=131072 || l.body!=8355840) fail("1080p layout");
    l=afbc_layout(129,129);if(l.header!=2048 || l.aw!=144 || l.ah!=144) fail("padding layout");
    uint8_t a[8]={0,1,2,255,10,20,30,40},b[8];memcpy(b,a,8);
    if(compare_pixels(a,b,8,0).pixels) fail("positive pixel comparison");
    b[5]+=2;if(compare_pixels(a,b,8,1).pixels!=1 || compare_pixels(a,b,8,1).max_error!=2) fail("negative pixel comparison");
    if(compare_pixels(a,b,8,2).pixels) fail("comparison tolerance");
    if(checksum((const uint8_t *)"hello",5)!=UINT64_C(0xa430d84680aabd0b)) fail("checksum self-test");
    if(!known_driver_sha(KNOWN_SHA) || !known_driver_sha(KNOWN_STRIPPED_SHA) || known_driver_sha("unknown")) fail("driver allowlist");
    if(!extension("ABC DEF","DEF") || extension("ABC DEFG","DEF")) fail("extension boundary");
    printf("RESULT=PASS test=self-test pointer_bits=%zu metadata_bytes=%zu\n",sizeof(void *)*8,sizeof(struct native_pixmap));
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    const char *library_path="/usr/lib/libMali.so",*sha="";unsigned timeout=180;int selftest=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--self-test")) selftest=1;
        else if(!strcmp(argv[i],"--memory-check")) memory_check=1;
        else if(!strcmp(argv[i],"--local")) local_control=1;
        else if(i+1<argc) {
            const char *key=argv[i],*value=argv[++i];
            if(!strcmp(key,"--library")) library_path=value;
            else if(!strcmp(key,"--driver-sha256")) sha=value;
            else if(!strcmp(key,"--width")) width=number(value,1,4096);
            else if(!strcmp(key,"--height")) height=number(value,1,4096);
            else if(!strcmp(key,"--frames")) frames=number(value,2,600);
            else if(!strcmp(key,"--rounds")) rounds=number(value,1,4);
            else if(!strcmp(key,"--cycles")) cycles=number(value,1,5);
            else if(!strcmp(key,"--fps")) fps=number(value,0,120);
            else if(!strcmp(key,"--watchdog")) timeout=number(value,1,900);
            else if(!strcmp(key,"--pattern")) pattern=value;
            else if(!strcmp(key,"--profile")) profile=value;
            else if(!strcmp(key,"--image-a")) image_path[0]=value;
            else if(!strcmp(key,"--image-b")) image_path[1]=value;
            else fail("unknown argument");
        } else fail("missing option value");
    }
    if(strcmp(pattern,"gradient") && strcmp(pattern,"detail") && strcmp(pattern,"noise") && strcmp(pattern,"alpha") && strcmp(pattern,"photos")) fail("unknown pattern");
    if(strcmp(profile,"afbc") && strcmp(profile,"afbc-split")) fail("unknown AFBC profile");
    if(!strcmp(pattern,"photos") && (!image_path[0] || !image_path[1])) fail("photos requires --image-a and --image-b");
    if(!selftest && !memory_check && fps && (double)cycles*rounds*4*(frames+4)/fps+10>timeout)
        fail("requested pacing duration exceeds watchdog; increase --watchdog or shorten the run");
    pixel_bytes=(size_t)width*height*4;
    int supervised=supervise(timeout);if(supervised>=0) return supervised;
    if(selftest) { self_test();return 0; }
    if(memory_check) return test_memory();
    if(!local_control && (sizeof(void *)!=4 || !known_driver_sha(sha))) {
        puts("RESULT=SKIP reason=private-ABI-requires-exact-known-32-bit-library");return 2;
    }
    struct layout l=afbc_layout(width,height);
    printf("fanart_probe_version=%d size=%ux%u pattern=%s candidate=%s frames_per_block=%u rounds=%u cycles=%u pacing_fps=%u\n",VERSION,width,height,pattern,local_control?"ordinary-rendered-copy":profile,frames,rounds,cycles,fps);
    printf("layout padded=%zux%zu header=%zu body=%zu bytes_per_allocation_with_guard=%zu\n",l.aw,l.ah,l.header,l.body,l.capacity);
    puts("scope=offscreen-source-texture-comparison presented_frames=0 measured_memory_bandwidth=0");
    stage="initialize";load_api(library_path);query_ion();init_egl();
    GLint max_texture=0;p_glGetIntegerv(0x0d33,&max_texture);
    if(width>(unsigned)max_texture || height>(unsigned)max_texture) fail("requested size exceeds GL_MAX_TEXTURE_SIZE");
    if(!local_control && (!create_mapping || !destroy_mapping)) fail("private pixmap exports unavailable");
    init_renderer();measurements=fopen("frames.csv","w");if(!measurements) fail("create frames.csv");
    fputs("cycle,round,block,route,frame,monotonic_start_s,cpu_submit_ms,wall_completed_ms,gpu_ms,gpu_valid\n",measurements);
    int ok=1;
    for(unsigned c=0;c<cycles && ok;c++) ok=run_cycle(c);
    if(fclose(measurements)) fail("close measurements");
    p_glDeleteProgram(renderer.program);term_egl();
    printf("RESULT=%s test=fanart-crossfade candidate=%s scope=offscreen-upload-convert-blend-readback-timing\n",ok?"PASS":"FAIL",local_control?"ordinary-rendered-copy":profile);
    return ok?0:1;
}
