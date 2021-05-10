#define  _GNU_SOURCE

#include <ctype.h>
#include <features.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include <time.h>
#include <poll.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <math.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "flutter_embedder.h"
#include "debug.h"

#define EGL_PLATFORM_GBM_KHR    0x31D7

typedef enum {
    kVBlankRequest,
    kVBlankReply,
    kFlutterTask
} engine_task_type;

struct engine_task {
    struct engine_task *next;
    engine_task_type type;
    union {
        FlutterTask task;
        struct {
            uint64_t vblank_ns;
            intptr_t baton;
        };
    };
    uint64_t target_time;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

struct pageflip_data {
    struct gbm_bo *releaseable_bo;
    intptr_t next_baton;
};


/// width & height of the display in pixels
// static uint32_t width, height;

// /// physical width & height of the display in millimeters
// /// the physical size can only be queried for HDMI displays (and even then, most displays will
// ///   probably return bogus values like 160mm x 90mm).
// /// for DSI displays, the physical size of the official 7-inch display will be set in init_display.
// /// init_display will only update width_mm and height_mm if they are set to zero, allowing you
// ///   to hardcode values for you individual display.
// static uint32_t width_mm = 0, height_mm = 0;
// static uint32_t refresh_rate = 60;
static uint32_t refresh_period_ns = 16666666;

/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
// static double pixel_ratio =  1.358234;

static struct {
    char device[PATH_MAX];
    bool has_device;
    int fd;
    uint32_t connector_id;
    drmModeModeInfo *mode;
    uint32_t crtc_id;
    size_t crtc_index;
    struct gbm_bo *previous_bo;
    drmEventContext evctx;
} drm = {0};

static struct {
    struct gbm_device  *device;
    struct gbm_surface *surface;
    uint32_t            format;
    uint64_t            modifier;
} gbm = {0};

static struct {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext context;
    EGLSurface surface;

    bool       modifiers_supported;
    char      *renderer;

    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);
    EGLSurface (*eglCreatePlatformWindowSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_window,
                                                    const EGLint *attrib_list);
    EGLSurface (*eglCreatePlatformPixmapSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_pixmap,
                                                    const EGLint *attrib_list);
} egl = {0};

// position & pointer phase of a mouse pointer / multitouch slot
// A 10-finger multi-touch display has 10 slots and each of them have their own position, tracking id, etc.
// All mouses / touchpads share the same mouse pointer.
struct mousepointer_mtslot {
    // the MT tracking ID used to track this touch.
    int     id;
    int32_t flutter_slot_id;
    double  x, y;
    FlutterPointerPhase phase;
};

static struct mousepointer_mtslot mousepointer;
static pthread_t io_thread_id;
#define MAX_EVENTS_PER_READ 64
static struct input_event io_input_buffer[MAX_EVENTS_PER_READ];

// Flutter VSync handles
// stored as a ring buffer. i_batons is the offset of the first baton (0 - 63)
// scheduled_frames - 1 is the number of total number of stored batons.
// (If 5 vsync events were asked for by the flutter engine, you only need to store 4 batons.
//  The baton for the first one that was asked for would've been returned immediately.)
static intptr_t batons[64];
static uint8_t i_batons = 0;
static int scheduled_frames = 0;
static pthread_t platform_thread_id;

static struct engine_task *tasklist = NULL;
static pthread_mutex_t tasklist_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  task_added = PTHREAD_COND_INITIALIZER;

extern FlutterEngine engine;
static _Atomic bool  engine_running = false;

static void _post_platform_task(struct engine_task *);

static void pageflip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *userdata)
{
    debug("pageflip start");
    FlutterEngineTraceEventInstant("pageflip");
    _post_platform_task(&(struct engine_task) {
        .type = kVBlankReply,
        .target_time = 0,
        .vblank_ns = sec * 1000000000ull + usec * 1000ull,
    });
    debug("pageflip end");
}

static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = data;

    if (fb->fb_id)
        drmModeRmFB(drm.fd, fb->fb_id);

    free(fb);
}

static struct drm_fb *drm_fb_get_from_bo(struct gbm_bo *bo)
{
    // if the buffer object already has some userdata associated with it,
    //   it's the framebuffer we allocated.
    struct drm_fb *fb = gbm_bo_get_user_data(bo);
    if (fb) return fb;

    // if there's no framebuffer for the bo, we need to create one.
    fb = calloc(1, sizeof(struct drm_fb));
    fb->bo = bo;

    int width = gbm_bo_get_width(bo);
    int height = gbm_bo_get_height(bo);
    int format = gbm_bo_get_format(bo);

    uint64_t modifiers[4] = {0};
    modifiers[0] = gbm_bo_get_modifier(bo);
    const int num_planes = gbm_bo_get_plane_count(bo);

    uint32_t strides[4] = {0}, handles[4] = {0}, offsets[4] = {0};
    for (int i = 0; i < num_planes; i++) {
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        handles[i] = gbm_bo_get_handle(bo).u32;
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifiers[0];
    }

    uint32_t flags = 0;
    if (modifiers[0]) {
        flags = DRM_MODE_FB_MODIFIERS;
    }

    int ok = drmModeAddFB2WithModifiers(drm.fd, width, height, format, handles, strides, offsets, modifiers,
                                        &fb->fb_id, flags);

    if (ok) {
        if (flags)
            debug("drm_fb_get_from_bo: modifiers failed!");

        memcpy(handles, (uint32_t [4]) {
            gbm_bo_get_handle(bo).u32, 0, 0, 0
        }, 16);

        memcpy(strides, (uint32_t [4]) {
            gbm_bo_get_stride(bo), 0, 0, 0
        }, 16);

        memset(offsets, 0, 16);

        ok = drmModeAddFB2(drm.fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
    }

    if (ok) {
        debug("drm_fb_get_from_bo: failed to create fb: %s\n", strerror(errno));
        free(fb);
        return NULL;
    }

    gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

    return fb;
}

static bool run_message_loop(void)
{
    debug("run_message_loop pre");
    while (true) {
        debug("run_message_loop start");
        pthread_mutex_lock(&tasklist_lock);

        // wait for a task to be inserted into the list
        while (tasklist == NULL)
            pthread_cond_wait(&task_added, &tasklist_lock);

        // wait for a task to be ready to be run
        uint64_t currenttime;
        while (tasklist->target_time > (currenttime = FlutterEngineGetCurrentTime())) {
            struct timespec abstargetspec;
            clock_gettime(CLOCK_REALTIME, &abstargetspec);
            uint64_t abstarget = abstargetspec.tv_nsec + abstargetspec.tv_sec * 1000000000ull +
                                 (tasklist->target_time - currenttime);
            abstargetspec.tv_nsec = abstarget % 1000000000;
            abstargetspec.tv_sec =  abstarget / 1000000000;

            pthread_cond_timedwait(&task_added, &tasklist_lock, &abstargetspec);
        }

        struct engine_task *task = tasklist;
        tasklist = tasklist->next;

        pthread_mutex_unlock(&tasklist_lock);
        if (task->type == kVBlankRequest) {
            if (scheduled_frames == 0) {
                uint64_t ns;
                drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
                FlutterEngineOnVsync(engine, task->baton, ns, ns + refresh_period_ns);
            } else {
                batons[(i_batons + (scheduled_frames - 1)) & 63] = task->baton;

            }
            scheduled_frames++;
        } else if (task->type == kVBlankReply) {
            if (scheduled_frames > 1) {
                intptr_t baton = batons[i_batons];
                i_batons = (i_batons + 1) & 63;
                uint64_t ns = task->vblank_ns;
                FlutterEngineOnVsync(engine, baton, ns, ns + refresh_period_ns);
            }
            scheduled_frames--;
        } else if (task->type == kFlutterTask) {
            if (FlutterEngineRunTask(engine, &task->task) != kSuccess) {
                debug("Error running platform task");
                return false;
            }
        }

        free(task);
        debug("run_message_loop end");
    }
    debug("run_message_loop exit");
    return true;
}

static drmModeConnector *find_connector(drmModeRes *resources)
{
    debug("find_connector");
    debug("find_connector %d", resources->count_connectors);
    // iterate the connectors
    for (int i = 0; i < resources->count_connectors; i++) {
        debug("checking connector");
        drmModeConnector *connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED)
            return connector;
        debug("not that one");
        drmModeFreeConnector(connector);
    }
    // no connector found
    return NULL;
}

static drmModeEncoder *find_encoder(drmModeRes *resources, drmModeConnector *connector)
{
    if (connector->encoder_id)
    return drmModeGetEncoder(drm.fd, connector->encoder_id);
    // no encoder found
    return NULL;
}

static size_t init_drm(void)
{
    drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    debug("opened card\r\n");

    debug("drmModeGetResources before");
    drmModeRes *resources = drmModeGetResources(drm.fd);
    if(!resources) {
        error("drmModeGetResources failed");
        return false;
    }
    debug("drmModeGetResources after");
    // find a connector
    debug("find_connector before");
    drmModeConnector *connector = find_connector(resources);
    debug("find_connector after");
    if (!connector) {
        debug("Failed to get connector");
        return -1;
    }
    debug("found connector");

    // save the connector_id
    drm.connector_id = connector->connector_id;

    // save the first mode
    drm.mode = &connector->modes[0]; 
    debug("resolution: %ix%i\n", drm.mode->hdisplay, drm.mode->vdisplay);

    // find an encoder
    drmModeEncoder *encoder = find_encoder(resources, connector);
    if (!encoder) {
        debug("failed to get encoder\r\n");
        return -1;
    }
    // find a CRTC
    if (encoder->crtc_id) {
        drm.crtc_id = encoder->crtc_id;
        // crtc = drmModeGetCrtc(drm.fd, encoder->crtc_id);
    }
    drmModeFreeEncoder(encoder);

    // fix this
    //drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return 0;
}

static size_t init_opengl(void)
{
    debug("setup opengl");
    gbm.device = gbm_create_device(drm.fd);
    egl.display = eglGetDisplay(gbm.device);

    if (!eglInitialize(egl.display, NULL, NULL)) {
		debug("failed to initialize egl");
		return -1;
	}

    // create an OpenGL context
    if(!eglBindAPI(EGL_OPENGL_API)){
        debug("failed to bind api");
        return -1;
    }

    const EGLint attributes[] = {
        EGL_RED_SIZE,8,
        EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8,
        EGL_ALPHA_SIZE,8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE,
    };

    const char *egl_exts_client;

    debug("Querying EGL client extensions...");
    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    debug("%s", egl_exts_client);

    egl.eglGetPlatformDisplayEXT = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
    debug("Getting EGL display for GBM device...");
    if (egl.eglGetPlatformDisplayEXT) egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device, NULL);
    else                              
    egl.display = eglGetDisplay((void *) gbm.device);

    if (!egl.display) {
        debug("Couldn't get EGL display");
        return -1;
    }

    EGLint num_config;
    if(!eglChooseConfig(egl.display, attributes, &egl.config, 1, &num_config)) {
        debug("failed to choose config");
        return -1;
    }

    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, NULL);
    if (!egl.context) {
        debug("Failed to create context");
        return -1;
    }

    gbm.format = DRM_FORMAT_XRGB8888;

    // create the GBM and EGL surface
    gbm.surface = gbm_surface_create(gbm.device, drm.mode->hdisplay, drm.mode->vdisplay, gbm.format, 1);
    if (!gbm.surface) {
        debug("Failed to create surface\r\n");
        return -1;
    }

    egl.surface = eglCreateWindowSurface(egl.display, egl.config, gbm.surface, NULL);
    if (!egl.surface) {
        debug("failed to create window surface");
        return -1;
    }
    
   if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
        debug("Could not make EGL context current to get OpenGL information");
        return -1;
    }
    debug("setup opengl complete");
    return 0;
}

static size_t init_display()
{
    debug("\r\ninit_display start");
    size_t result;
    result = init_drm();
    if(result != 0)
        return result;
    debug("init_drm result: %ld", result);

    result = init_opengl();
    if(result != 0)
        return result;

    debug("init_opengl result: %ld", result);

    drm.evctx = (drmEventContext) {
        .version = 4,
        .vblank_handler = NULL,
        .page_flip_handler = pageflip_handler,
        .page_flip_handler2 = NULL,
        .sequence_handler = NULL
    };

    debug("Swapping buffers...");
    eglSwapBuffers(egl.display, egl.surface);

    debug("Locking front buffer...");
    drm.previous_bo = gbm_surface_lock_front_buffer(gbm.surface);

    debug("getting new framebuffer for BO...");
    struct drm_fb *fb = drm_fb_get_from_bo(drm.previous_bo);
    if (!fb) {
        debug("failed to get a new framebuffer BO");
        return -1;
    }

    debug("Setting CRTC...");
    result = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    if (result) {
        debug("failed to set mode: %s\n", strerror(errno));
        return -1;
    }

    debug("Clearing current context...");
    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        debug("Could not clear EGL context");
        return -1;
    }

    debug("init_display end\r\n");
    return 0;
}

static size_t init_io(void)
{
    FlutterPointerEvent flutterevents[16] = {0};
    size_t i_flutterevent = 0;
    int n_flutter_slots = 0;

    // add the mouse slot
    mousepointer = (struct mousepointer_mtslot) {
        .id = 0,
        .flutter_slot_id = n_flutter_slots++,
        .x = 0, .y = 0,
        .phase = kCancel
    };

    flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
        .struct_size = sizeof(FlutterPointerEvent),
        .phase = kAdd,
        .timestamp = (size_t) (FlutterEngineGetCurrentTime() * 1000),
        .x = 0,
        .y = 0,
        .signal_kind = kFlutterPointerSignalKindNone,
        .device_kind = kFlutterPointerDeviceKindTouch,
        .device = mousepointer.flutter_slot_id,
        .buttons = 0
    };
    if(FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) != kSuccess) {
        return -1;
    }
    return 0;
}

static void process_io_events(int fd)
{
    // Read as many the input events as possible
    ssize_t rd = read(fd, io_input_buffer, sizeof(io_input_buffer));
    if (rd < 0)
        error("read failed");
    if (rd % sizeof(struct input_event))
        error("read returned %d which is not a multiple of %d!", (int) rd, (int) sizeof(struct input_event));

    FlutterPointerEvent flutterevents[64] = {0};
    size_t i_flutterevent = 0;

    size_t event_count = rd / sizeof(struct input_event);
    for (size_t i = 0; i < event_count; i++) {
        if (io_input_buffer[i].type == EV_ABS) {
            if (io_input_buffer[i].code == ABS_X) {
                mousepointer.x = io_input_buffer[i].value;
            } else if (io_input_buffer[i].code == ABS_Y) {
                mousepointer.y = io_input_buffer[i].value;
            } else if (io_input_buffer[i].code == ABS_MT_TRACKING_ID && io_input_buffer[i].value == -1) {
            }

            if (mousepointer.phase == kDown) {
                flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
                    .struct_size = sizeof(FlutterPointerEvent),
                    .phase = kMove,
                    .timestamp = io_input_buffer[i].time.tv_sec * 1000000 + io_input_buffer[i].time.tv_usec,
                    .x = mousepointer.x, .y = mousepointer.y,
                    .device = mousepointer.flutter_slot_id,
                    .signal_kind = kFlutterPointerSignalKindNone,
                    .device_kind = kFlutterPointerDeviceKindTouch,
                    .buttons = 0
                };
            }

        } else if (io_input_buffer[i].type == EV_KEY) {
            if (io_input_buffer[i].code == BTN_TOUCH) {
                mousepointer.phase = io_input_buffer[i].value ? kDown : kUp;
            } else {
                debug("unknown EV_KEY code=%d value=%d\r\n", io_input_buffer[i].code, io_input_buffer[i].value);
            }
        } else if (io_input_buffer[i].type == EV_SYN && io_input_buffer[i].code == SYN_REPORT) {
            // we don't want to send an event to flutter if nothing changed.
            if (mousepointer.phase == kCancel) continue;

            flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
                .struct_size = sizeof(FlutterPointerEvent),
                .phase = mousepointer.phase,
                .timestamp = io_input_buffer[i].time.tv_sec * 1000000 + io_input_buffer[i].time.tv_usec,
                .x = mousepointer.x, .y = mousepointer.y,
                .device = mousepointer.flutter_slot_id,
                .signal_kind = kFlutterPointerSignalKindNone,
                .device_kind = kFlutterPointerDeviceKindTouch,
                .buttons = 0
            };
            if (mousepointer.phase == kUp)
                mousepointer.phase = kCancel;
        } else {
            debug("unknown input_event type=%d\r\n", io_input_buffer[i].type);
        }
    }

    if (i_flutterevent == 0) return;

    // now, send the data to the flutter engine
    if (FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) != kSuccess) {
        debug("could not send pointer events to flutter engine\r\n");
    }
}

static void *io_loop(void *userdata)
{
    const char *input_path = "/dev/input/event0";
    int fd = open(input_path, O_RDONLY);
    if (errno == EACCES && getuid() != 0)
        error("You do not have access to %s.", input_path);

    struct pollfd fdset[2];
    memset(fdset, -1, sizeof(fdset));

    fdset[0].fd = fd;
    fdset[0].events = (POLLIN | POLLPRI | POLLHUP);
    fdset[0].revents = 0;

    fdset[1].fd = drm.fd;
    fdset[1].events = (POLLIN | POLLPRI | POLLHUP);
    fdset[1].revents = 0;
    while (engine_running) {

        int rc = poll(fdset, 2, -1);
        if (rc < 0)
            error("poll error");

        if (fdset[0].revents & (POLLIN | POLLHUP))
            process_io_events(fd);

        if (fdset[1].revents & (POLLIN | POLLHUP))
            drmHandleEvent(drm.fd, &drm.evctx);
    }
    return NULL;
}

static size_t run_io_thread()
{
    debug("run_io_thread start");
    int ok = pthread_create(&io_thread_id, NULL, &io_loop, NULL);
    if (ok != 0) {
        error("couldn't create  io thread: [%s]", strerror(ok));
        return false;
    }

    ok = pthread_setname_np(io_thread_id, "io");
    if (ok != 0) {
        error("couldn't set name of io thread: [%s]", strerror(ok));
        return false;
    }

    debug("run_io_thread end");
    return 0;
}
 
bool gfx_make_current(void *userdata)
{
    debug("gfx_make_current start");
    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
        debug("make_current: could not make the context current.");
        return false;
    }
    debug("gfx_make_current end");
    return true;
}

bool gfx_clear_current(void *userdata)
{
    debug("gfx_clear_current start");
    if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        debug("clear_current: could not clear the current context.");
        return false;
    }
    debug("gfx_clear_current end");
    return true;
}

bool gfx_present(void *userdata)
{
    debug("gfx_present start");
    FlutterEngineTraceEventDurationBegin("present");

    eglSwapBuffers(egl.display, egl.surface);
    struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(gbm.surface);
    struct drm_fb *fb = drm_fb_get_from_bo(next_bo);

    int ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
    if (ok) {
        perror("failed to queue page flip");
        return false;
    }

    gbm_surface_release_buffer(gbm.surface, drm.previous_bo);
    drm.previous_bo = next_bo;

    FlutterEngineTraceEventDurationEnd("present");
    debug("gfx_present end");
    return true;
}

uint32_t gfx_fbo_callback(void *userdata)
{
    return 0;
}

void gfx_vsync(void *userdata, intptr_t baton)
{
    debug("vsync start");
    _post_platform_task(&(struct engine_task) {
        .type = kVBlankRequest,
        .target_time = 0,
        .baton = baton
    });
    debug("vsync end");
}

void on_post_flutter_task(FlutterTask task, uint64_t target_time, void *userdata)
{
    debug("on_post_flutter_task start");
    _post_platform_task(&(struct engine_task) {
        .type = kFlutterTask,
        .task = task,
        .target_time = target_time
    });
    debug("on_post_flutter_task end");
}

bool runs_platform_tasks_on_current_thread(void *userdata)
{
    debug("runs_platform_tasks_on_current_thread");
    return pthread_equal(pthread_self(), platform_thread_id) != 0;
}

size_t gfx_init(FlutterEngine _engine)
{
    engine = _engine;
    platform_thread_id = pthread_self();

    // initialize display
    debug("initializing display...");
    if (init_display() < 0) {
        error("init_display failed");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void gfx_loop()
{
    debug("Initializing Input devices...");
    if (init_io() < 0) {
        error("init_io failed");
        return;
    }

    // read input events
    debug("Running IO thread...");
    if(run_io_thread() < 0) {
        error("run_io_thread failed");
        return;
    }

    // run message loop
    debug("Running message loop...");
    run_message_loop();
}

void gfx_terminate(void)
{
    debug("gfx_terminate not yet implemented");
}

static void _post_platform_task(struct engine_task *task)
{   
    debug("_post_platform_task start");
    struct engine_task *to_insert = malloc(sizeof(struct engine_task));
    if (!to_insert) return;

    memcpy(to_insert, task, sizeof(struct engine_task));
    pthread_mutex_lock(&tasklist_lock);
    if (tasklist == NULL || to_insert->target_time < tasklist->target_time) {
        to_insert->next = tasklist;
        tasklist = to_insert;
    } else {
        struct engine_task *prev = tasklist;
        struct engine_task *current = tasklist->next;
        while (current != NULL && to_insert->target_time > current->target_time) {
            prev = current;
            current = current->next;
        }
        to_insert->next = current;
        prev->next = to_insert;
    }

    pthread_mutex_unlock(&tasklist_lock);
    pthread_cond_signal(&task_added);
    debug("_post_platform_task end");
}

static void _cut_word_from_string(char *string, char *word)
{
    size_t word_length = strlen(word);
    char  *word_in_str = strstr(string, word);

    // check if the given word is surrounded by spaces in the string
    if (word_in_str
            && ((word_in_str == string) || (word_in_str[-1] == ' '))
            && ((word_in_str[word_length] == 0) || (word_in_str[word_length] == ' '))
       ) {
        if (word_in_str[word_length] == ' ') word_length++;

        int i = 0;
        do {
            word_in_str[i] = word_in_str[i + word_length];
        } while (word_in_str[i++ + word_length] != 0);
    }
}

static const GLubyte *_hacked_glGetString(GLenum name)
{
    static GLubyte *extensions = NULL;

    if (name != GL_EXTENSIONS)
        return glGetString(name);

    if (extensions == NULL) {
        GLubyte *orig_extensions = (GLubyte *) glGetString(GL_EXTENSIONS);

        extensions = malloc(strlen((const char *)orig_extensions) + 1);
        if (!extensions) {
            debug("Could not allocate memory for modified GL_EXTENSIONS string");
            return NULL;
        }

        strcpy((char *)extensions, (const char *)orig_extensions);

        /*
            * working (apparently)
            */
        //_cut_word_from_string(extensions, "GL_EXT_blend_minmax");
        //_cut_word_from_string(extensions, "GL_EXT_multi_draw_arrays");
        //_cut_word_from_string(extensions, "GL_EXT_texture_format_BGRA8888");
        //_cut_word_from_string(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
        //_cut_word_from_string(extensions, "GL_OES_depth24");
        //_cut_word_from_string(extensions, "GL_OES_texture_npot");
        //_cut_word_from_string(extensions, "GL_OES_vertex_half_float");
        //_cut_word_from_string(extensions, "GL_OES_EGL_image");
        //_cut_word_from_string(extensions, "GL_OES_depth_texture");
        //_cut_word_from_string(extensions, "GL_AMD_performance_monitor");
        //_cut_word_from_string(extensions, "GL_OES_EGL_image_external");
        //_cut_word_from_string(extensions, "GL_EXT_occlusion_query_boolean");
        //_cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_ldr");
        //_cut_word_from_string(extensions, "GL_EXT_compressed_ETC1_RGB8_sub_texture");
        //_cut_word_from_string(extensions, "GL_EXT_draw_elements_base_vertex");
        //_cut_word_from_string(extensions, "GL_EXT_texture_border_clamp");
        //_cut_word_from_string(extensions, "GL_OES_draw_elements_base_vertex");
        //_cut_word_from_string(extensions, "GL_OES_texture_border_clamp");
        //_cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_sliced_3d");
        //_cut_word_from_string(extensions, "GL_MESA_tile_raster_order");

        /*
        * should be working, but isn't
        */
        _cut_word_from_string((char *)extensions, "GL_EXT_map_buffer_range");

        /*
        * definitely broken
        */
        _cut_word_from_string((char *)extensions, "GL_OES_element_index_uint");
        _cut_word_from_string((char *)extensions, "GL_OES_fbo_render_mipmap");
        _cut_word_from_string((char *)extensions, "GL_OES_mapbuffer");
        _cut_word_from_string((char *)extensions, "GL_OES_rgb8_rgba8");
        _cut_word_from_string((char *)extensions, "GL_OES_stencil8");
        _cut_word_from_string((char *)extensions, "GL_OES_texture_3D");
        _cut_word_from_string((char *)extensions, "GL_OES_packed_depth_stencil");
        _cut_word_from_string((char *)extensions, "GL_OES_get_program_binary");
        _cut_word_from_string((char *)extensions, "GL_APPLE_texture_max_level");
        _cut_word_from_string((char *)extensions, "GL_EXT_discard_framebuffer");
        _cut_word_from_string((char *)extensions, "GL_EXT_read_format_bgra");
        _cut_word_from_string((char *)extensions, "GL_EXT_frag_depth");
        _cut_word_from_string((char *)extensions, "GL_NV_fbo_color_attachments");
        _cut_word_from_string((char *)extensions, "GL_OES_EGL_sync");
        _cut_word_from_string((char *)extensions, "GL_OES_vertex_array_object");
        _cut_word_from_string((char *)extensions, "GL_EXT_unpack_subimage");
        _cut_word_from_string((char *)extensions, "GL_NV_draw_buffers");
        _cut_word_from_string((char *)extensions, "GL_NV_read_buffer");
        _cut_word_from_string((char *)extensions, "GL_NV_read_depth");
        _cut_word_from_string((char *)extensions, "GL_NV_read_depth_stencil");
        _cut_word_from_string((char *)extensions, "GL_NV_read_stencil");
        _cut_word_from_string((char *)extensions, "GL_EXT_draw_buffers");
        _cut_word_from_string((char *)extensions, "GL_KHR_debug");
        _cut_word_from_string((char *)extensions, "GL_OES_required_internalformat");
        _cut_word_from_string((char *)extensions, "GL_OES_surfaceless_context");
        _cut_word_from_string((char *)extensions, "GL_EXT_separate_shader_objects");
        _cut_word_from_string((char *)extensions, "GL_KHR_context_flush_control");
        _cut_word_from_string((char *)extensions, "GL_KHR_no_error");
        _cut_word_from_string((char *)extensions, "GL_KHR_parallel_shader_compile");
    }

    return extensions;
}

void *proc_resolver(void *userdata, const char *name)
{
    static int is_VC4 = -1;
    void      *address;

    /*
     * The mesa V3D driver reports some OpenGL ES extensions as supported and working
     * even though they aren't. _hacked_glGetString is a workaround for this, which will
     * cut out the non-working extensions from the list of supported extensions.
     */

    if (name == NULL)
        return NULL;

    // first detect if we're running on a VideoCore 4 / using the VC4 driver.
    if ((is_VC4 == -1) && (is_VC4 = strcmp(egl.renderer, "VC4 V3D 2.1") == 0))
        printf( "detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
                "Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.");

    // if we do, and the symbol to resolve is glGetString, we return our _hacked_glGetString.
    if (is_VC4 && (strcmp(name, "glGetString") == 0))
        return _hacked_glGetString;

    if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
        return address;

    debug("proc_resolver: could not resolve symbol \"%s\"\n", name);

    return NULL;
}