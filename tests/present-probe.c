/* Small visible compositor pacing probe. wl_shm triple buffers are never
 * overwritten until wl_buffer.release. Timings come from wp_presentation,
 * not frame callbacks (which only pace submission). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped=1; }

#define W 160
#define H 120
struct app;
struct slot { struct wl_buffer *buffer; uint32_t *pixels; bool busy; struct app *app; };
struct app {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wp_presentation *presentation;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;
    struct slot slots[3];
    void *mapping;
    bool configured, frame_ready, closing;
    unsigned frames, presented, discarded;
    uint32_t clock, output_id;
};
struct feedback { struct app *app; uint64_t submit_ns; uint32_t output_id; };
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void draw(struct app *a);
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct slot *s = data;
    s->busy = false;
    draw(s->app);
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};
static void frame_done(void *data, struct wl_callback *callback, uint32_t t) {
    (void)t;
    wl_callback_destroy(callback);
    struct app *a = data;
    a->frame_ready = true;
    draw(a);
}
static const struct wl_callback_listener frame_listener = {frame_done};
static void sync_output(void *data, struct wp_presentation_feedback *fb, struct wl_output *output) {
    (void)fb;
    ((struct feedback *)data)->output_id = wl_proxy_get_id((struct wl_proxy *)output);
}
static void presented(void *data, struct wp_presentation_feedback *fb,
                      uint32_t hi, uint32_t lo, uint32_t ns, uint32_t refresh,
                      uint32_t seqhi, uint32_t seqlo, uint32_t flags) {
    struct feedback *f = data;
    const uint64_t time = (((uint64_t)hi << 32) | lo) * 1000000000ull + ns;
    printf("presented,%llu,%llu,%u,%llu,%u,%u\n", (unsigned long long)f->submit_ns,
        (unsigned long long)time, refresh,
        (unsigned long long)(((uint64_t)seqhi << 32) | seqlo), flags, f->output_id);
    f->app->presented++;
    wp_presentation_feedback_destroy(fb);
    free(f);
}
static void discarded(void *data, struct wp_presentation_feedback *fb) {
    struct feedback *f = data;
    printf("discarded,%llu,0,0,0,0,%u\n", (unsigned long long)f->submit_ns, f->output_id);
    f->app->discarded++;
    wp_presentation_feedback_destroy(fb);
    free(f);
}
static const struct wp_presentation_feedback_listener feedback_listener = {sync_output, presented, discarded};
static void draw(struct app *a) {
    if (!a->configured || !a->frame_ready || a->closing) return;
    struct slot *s = NULL;
    for (unsigned i=0; i<3; i++) if (!a->slots[i].busy) { s = &a->slots[i]; break; }
    if (!s) return;
    struct feedback *f = calloc(1, sizeof(*f));
    if (!f) { a->closing = true; return; }
    for (unsigned y=0; y<H; y++) for (unsigned x=0; x<W; x++)
        s->pixels[y*W+x] = x == a->frames % W ? 0x00ffffff : 0x002b4155;
    s->busy = true;
    a->frames++;
    f->app = a;
    f->submit_ns = now_ns();
    f->output_id = a->output_id;
    struct wp_presentation_feedback *fb = wp_presentation_feedback(a->presentation, a->surface);
    wp_presentation_feedback_add_listener(fb, &feedback_listener, f);
    wl_surface_attach(a->surface, s->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0, W, H);
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    a->frame_ready = false;
    wl_surface_commit(a->surface);
}
static void configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct app *a = data;
    xdg_surface_ack_configure(surface, serial);
    a->configured = true;
    draw(a);
}
static const struct xdg_surface_listener xdg_listener = {configure};
static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)top; (void)w; (void)h; (void)states;
}
static void top_close(void *data, struct xdg_toplevel *top) { (void)top; ((struct app *)data)->closing=true; }
static const struct xdg_toplevel_listener top_listener = {.configure=top_configure, .close=top_close};
static void ping(void *data, struct xdg_wm_base *wm, uint32_t serial) { (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = {ping};
static void clock_id(void *data, struct wp_presentation *p, uint32_t id) {
    (void)p; ((struct app *)data)->clock=id;
    fprintf(stderr,"presentation_clock_id=%u host_clock_id=%d\n",id,CLOCK_MONOTONIC);
}
static const struct wp_presentation_listener presentation_listener = {clock_id};
static void enter(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)s; ((struct app *)data)->output_id=wl_proxy_get_id((struct wl_proxy *)o);
}
static void leave(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)s;
    struct app *a=data;
    if(a->output_id==wl_proxy_get_id((struct wl_proxy *)o)) a->output_id=0;
}
static const struct wl_surface_listener surface_listener={.enter=enter,.leave=leave};
static void geometry(void *d,struct wl_output *o,int32_t x,int32_t y,int32_t pw,int32_t ph,int32_t sub,const char *make,const char *model,int32_t transform) {
    (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)transform;
}
static void mode(void *d,struct wl_output *o,uint32_t flags,int32_t w,int32_t h,int32_t refresh) {
    (void)d;
    if(flags & WL_OUTPUT_MODE_CURRENT) fprintf(stderr,"output_id=%u size=%dx%d refresh_mhz=%d\n",wl_proxy_get_id((struct wl_proxy *)o),w,h,refresh);
}
static void done(void *d,struct wl_output *o) {(void)d;(void)o;}
static void scale(void *d,struct wl_output *o,int32_t scale) {(void)d;(void)o;(void)scale;}
static void name(void *d,struct wl_output *o,const char *name) {
    (void)d;fprintf(stderr,"output_id=%u name=%s\n",wl_proxy_get_id((struct wl_proxy *)o),name);
}
static void description(void *d,struct wl_output *o,const char *description) {(void)d;(void)o;(void)description;}
static const struct wl_output_listener output_listener={.geometry=geometry,.mode=mode,.done=done,.scale=scale,.name=name,.description=description};
static void global(void *data, struct wl_registry *r, uint32_t id, const char *interface, uint32_t version) {
    struct app *a = data;
    if (!strcmp(interface,"wl_compositor")) a->compositor=wl_registry_bind(r,id,&wl_compositor_interface,version<4?version:4);
    else if (!strcmp(interface,"wl_shm")) a->shm=wl_registry_bind(r,id,&wl_shm_interface,1);
    else if (!strcmp(interface,"xdg_wm_base")) {
        a->wm=wl_registry_bind(r,id,&xdg_wm_base_interface,1);
        xdg_wm_base_add_listener(a->wm,&wm_listener,a);
    } else if (!strcmp(interface,"wp_presentation")) {
        a->presentation=wl_registry_bind(r,id,&wp_presentation_interface,1);
        wp_presentation_add_listener(a->presentation,&presentation_listener,a);
    } else if (!strcmp(interface,"wl_output")) {
        /* Bind so feedback sync_output has a proxy; only its id is recorded. */
        struct wl_output *output=wl_registry_bind(r,id,&wl_output_interface,version<4?version:4);
        wl_output_add_listener(output,&output_listener,a);
    }
}
static void global_remove(void *data, struct wl_registry *r, uint32_t id) { (void)data;(void)r;(void)id; }
static const struct wl_registry_listener registry_listener={global,global_remove};
int main(int argc,char **argv) {
    signal(SIGTERM,stop);
    signal(SIGINT,stop);
    setvbuf(stdout,NULL,_IOLBF,0);
    char *end;
    long seconds=argc>1?strtol(argv[1],&end,10):10;
    if ((argc>1 && *end) || seconds<1 || seconds>3600) { fprintf(stderr,"usage: present-probe [seconds:1..3600]\n"); return 2; }
    struct app a={.frame_ready=true,.clock=UINT32_MAX};
    a.display=wl_display_connect(NULL);
    if (!a.display) { perror("wl_display_connect"); return 1; }
    struct wl_registry *registry=wl_display_get_registry(a.display);
    wl_registry_add_listener(registry,&registry_listener,&a);
    if (wl_display_roundtrip(a.display)<0 || wl_display_roundtrip(a.display)<0 ||
        !a.compositor || !a.shm || !a.wm || !a.presentation) {
        fprintf(stderr,"required Wayland globals unavailable\n"); return 1;
    }
    const size_t size=W*H*4*3;
    int fd=memfd_create("nvwd-presentation-probe",MFD_CLOEXEC);
    if (fd<0 || ftruncate(fd,(off_t)size)) { perror("memfd"); return 1; }
    a.mapping=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (a.mapping==MAP_FAILED) { perror("mmap");close(fd);return 1; }
    struct wl_shm_pool *pool=wl_shm_create_pool(a.shm,fd,(int)size);
    close(fd);
    for(unsigned i=0;i<3;i++) {
        a.slots[i].app=&a;
        a.slots[i].pixels=(uint32_t *)a.mapping+W*H*i;
        a.slots[i].buffer=wl_shm_pool_create_buffer(pool,W*H*4*i,W,H,W*4,WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(a.slots[i].buffer,&buffer_listener,&a.slots[i]);
    }
    wl_shm_pool_destroy(pool);
    a.surface=wl_compositor_create_surface(a.compositor);
    wl_surface_add_listener(a.surface,&surface_listener,&a);
    a.xdg=xdg_wm_base_get_xdg_surface(a.wm,a.surface);
    xdg_surface_add_listener(a.xdg,&xdg_listener,&a);
    a.top=xdg_surface_get_toplevel(a.xdg);
    xdg_toplevel_add_listener(a.top,&top_listener,&a);
    xdg_toplevel_set_title(a.top,"Waydroid pacing probe — keep visible");
    xdg_toplevel_set_app_id(a.top,"org.waydroid.nvidia.pacing-probe");
    xdg_toplevel_set_min_size(a.top,W,H);
    xdg_toplevel_set_max_size(a.top,W,H);
    wl_surface_commit(a.surface);
    puts("event,submit_ns,present_ns,refresh_ns,sequence,flags,output_id");
    const uint64_t deadline=now_ns()+(uint64_t)seconds*1000000000ull;
    bool failed=false;
    while(!a.closing && !stopped && now_ns()<deadline) {
        while(wl_display_prepare_read(a.display)!=0) {
            if(wl_display_dispatch_pending(a.display)<0) { failed=true;break; }
        }
        if(failed) break;
        int flush=wl_display_flush(a.display);
        struct pollfd pfd={.fd=wl_display_get_fd(a.display),.events=POLLIN};
        if(flush<0 && errno==EAGAIN) pfd.events|=POLLOUT;
        else if(flush<0) { wl_display_cancel_read(a.display);failed=true;break; }
        int ready=poll(&pfd,1,100);
        if(ready>0 && (pfd.revents&POLLIN)) {
            if(wl_display_read_events(a.display)<0) {failed=true;break;}
        } else wl_display_cancel_read(a.display);
        if((ready<0 && errno!=EINTR) || (pfd.revents&(POLLERR|POLLHUP|POLLNVAL)) || wl_display_dispatch_pending(a.display)<0) {failed=true;break;}
    }
    a.closing=true;
    fprintf(stderr,"presented=%u discarded=%u failed=%d\n",a.presented,a.discarded,failed);
    wl_display_disconnect(a.display);
    munmap(a.mapping,size);
    return failed || a.presented<2 ? 1:0;
}
