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

#include "flutter_embedder.h"

#define DEBUG

#ifdef DEBUG
// #define LOG_PATH "/tmp/log.txt"
#define log_location stderr
#define debug(...) do { fprintf(log_location, __VA_ARGS__); fprintf(log_location, "\r\n"); fflush(log_location); } while(0)
#define error(...) do { debug(__VA_ARGS__); } while (0)
#else
#define debug(...)
#define error(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, ""); } while(0)
#endif

#define EGL_PLATFORM_GBM_KHR    0x31D7

enum device_orientation {
    kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight
};

#define ANGLE_FROM_ORIENTATION(o) \
    ((o) == kPortraitUp ? 0 : \
     (o) == kLandscapeLeft ? 90 : \
     (o) == kPortraitDown ? 180 : \
     (o) == kLandscapeRight ? 270 : 0)

#define FLUTTER_ROTATION_TRANSFORMATION(deg) ((FlutterTransformation) \
            {.scaleX = cos(((double) (deg))/180.0*M_PI), .skewX  = -sin(((double) (deg))/180.0*M_PI), .transX = 0, \
             .skewY  = sin(((double) (deg))/180.0*M_PI), .scaleY = cos(((double) (deg))/180.0*M_PI),  .transY = 0, \
             .pers0  = 0,                   .pers1  = 0,                    .pers2  = 1})


typedef enum {
    kVBlankRequest,
    kVBlankReply,
    kUpdateOrientation,
    kSendPlatformMessage,
    kRespondToPlatformMessage,
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
        enum device_orientation orientation;
        struct {
            char *channel;
            const FlutterPlatformMessageResponseHandle *responsehandle;
            size_t message_size;
            uint8_t *message;
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

static void post_platform_task(struct engine_task *task);

/// width & height of the display in pixels
static uint32_t width, height;

/// physical width & height of the display in millimeters
/// the physical size can only be queried for HDMI displays (and even then, most displays will
///   probably return bogus values like 160mm x 90mm).
/// for DSI displays, the physical size of the official 7-inch display will be set in init_display.
/// init_display will only update width_mm and height_mm if they are set to zero, allowing you
///   to hardcode values for you individual display.
static uint32_t width_mm = 0, height_mm = 0;
static uint32_t refresh_rate;

/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
static double pixel_ratio = 0.0;

/// The current device orientation.
/// The initial device orientation is based on the width & height data from drm.
static enum device_orientation orientation;

/// The angle between the initial device orientation and the current device orientation in degrees.
/// (applied as a rotation to the flutter window in transformation_callback, and also
/// is used to determine if width/height should be swapped when sending a WindowMetrics event to flutter)
static int rotation = 0;

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
    bool disable_vsync;
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

static struct {
    char asset_bundle_path[240];
    char kernel_blob_path[256];
    char executable_path[256];
    char icu_data_path[256];
    FlutterRendererConfig renderer_config;
    FlutterProjectArgs args;
    int engine_argc;
    const char *const *engine_argv;
} flutter = {0};

// Flutter VSync handles
// stored as a ring buffer. i_batons is the offset of the first baton (0 - 63)
// scheduled_frames - 1 is the number of total number of stored batons.
// (If 5 vsync events were asked for by the flutter engine, you only need to store 4 batons.
//  The baton for the first one that was asked for would've been returned immediately.)
static intptr_t batons[64];
static uint8_t i_batons = 0;
static int scheduled_frames = 0;
static pthread_t platform_thread_id;

static struct engine_task tasklist = {
    .next = NULL,
    .type = kFlutterTask,
    .target_time = 0,
    .task = {.runner = NULL, .task = 0}
};

static pthread_mutex_t tasklist_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  task_added = PTHREAD_COND_INITIALIZER;

static FlutterEngine engine;
static _Atomic bool  engine_running = false;

// IO stuff

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

static bool make_current(void *userdata)
{
    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
        debug("make_current: could not make the context current.");
        return false;
    }

    return true;
}

static bool clear_current(void *userdata)
{
    if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        debug("clear_current: could not clear the current context.");
        return false;
    }

    return true;
}

static void pageflip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *userdata)
{
    FlutterEngineTraceEventInstant("pageflip");
    post_platform_task(&(struct engine_task) {
        .type = kVBlankReply,
        .target_time = 0,
        .vblank_ns = sec * 1000000000ull + usec * 1000ull,
    });
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

static bool present(void *userdata)
{
    FlutterEngineTraceEventDurationBegin("present");

    eglSwapBuffers(egl.display, egl.surface);
    struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(gbm.surface);
    struct drm_fb *fb = drm_fb_get_from_bo(next_bo);

    // workaround for #38
    if (!drm.disable_vsync) {
        int ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, drm.previous_bo);
        if (ok) {
            perror("failed to queue page flip");
            return false;
        }
    } else {
        int ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
        if (ok == -1) {
            perror("failed swap buffers");
            return false;
        }
    }

    gbm_surface_release_buffer(gbm.surface, drm.previous_bo);
    drm.previous_bo = next_bo;

    FlutterEngineTraceEventDurationEnd("present");

    return true;
}

static uint32_t fbo_callback(void *userdata)
{
    return 0;
}

static void cut_word_from_string(char *string, char *word)
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

static const GLubyte *hacked_glGetString(GLenum name)
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
        //cut_word_from_string(extensions, "GL_EXT_blend_minmax");
        //cut_word_from_string(extensions, "GL_EXT_multi_draw_arrays");
        //cut_word_from_string(extensions, "GL_EXT_texture_format_BGRA8888");
        //cut_word_from_string(extensions, "GL_OES_compressed_ETC1_RGB8_texture");
        //cut_word_from_string(extensions, "GL_OES_depth24");
        //cut_word_from_string(extensions, "GL_OES_texture_npot");
        //cut_word_from_string(extensions, "GL_OES_vertex_half_float");
        //cut_word_from_string(extensions, "GL_OES_EGL_image");
        //cut_word_from_string(extensions, "GL_OES_depth_texture");
        //cut_word_from_string(extensions, "GL_AMD_performance_monitor");
        //cut_word_from_string(extensions, "GL_OES_EGL_image_external");
        //cut_word_from_string(extensions, "GL_EXT_occlusion_query_boolean");
        //cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_ldr");
        //cut_word_from_string(extensions, "GL_EXT_compressed_ETC1_RGB8_sub_texture");
        //cut_word_from_string(extensions, "GL_EXT_draw_elements_base_vertex");
        //cut_word_from_string(extensions, "GL_EXT_texture_border_clamp");
        //cut_word_from_string(extensions, "GL_OES_draw_elements_base_vertex");
        //cut_word_from_string(extensions, "GL_OES_texture_border_clamp");
        //cut_word_from_string(extensions, "GL_KHR_texture_compression_astc_sliced_3d");
        //cut_word_from_string(extensions, "GL_MESA_tile_raster_order");

        /*
        * should be working, but isn't
        */
        cut_word_from_string((char *)extensions, "GL_EXT_map_buffer_range");

        /*
        * definitely broken
        */
        cut_word_from_string((char *)extensions, "GL_OES_element_index_uint");
        cut_word_from_string((char *)extensions, "GL_OES_fbo_render_mipmap");
        cut_word_from_string((char *)extensions, "GL_OES_mapbuffer");
        cut_word_from_string((char *)extensions, "GL_OES_rgb8_rgba8");
        cut_word_from_string((char *)extensions, "GL_OES_stencil8");
        cut_word_from_string((char *)extensions, "GL_OES_texture_3D");
        cut_word_from_string((char *)extensions, "GL_OES_packed_depth_stencil");
        cut_word_from_string((char *)extensions, "GL_OES_get_program_binary");
        cut_word_from_string((char *)extensions, "GL_APPLE_texture_max_level");
        cut_word_from_string((char *)extensions, "GL_EXT_discard_framebuffer");
        cut_word_from_string((char *)extensions, "GL_EXT_read_format_bgra");
        cut_word_from_string((char *)extensions, "GL_EXT_frag_depth");
        cut_word_from_string((char *)extensions, "GL_NV_fbo_color_attachments");
        cut_word_from_string((char *)extensions, "GL_OES_EGL_sync");
        cut_word_from_string((char *)extensions, "GL_OES_vertex_array_object");
        cut_word_from_string((char *)extensions, "GL_EXT_unpack_subimage");
        cut_word_from_string((char *)extensions, "GL_NV_draw_buffers");
        cut_word_from_string((char *)extensions, "GL_NV_read_buffer");
        cut_word_from_string((char *)extensions, "GL_NV_read_depth");
        cut_word_from_string((char *)extensions, "GL_NV_read_depth_stencil");
        cut_word_from_string((char *)extensions, "GL_NV_read_stencil");
        cut_word_from_string((char *)extensions, "GL_EXT_draw_buffers");
        cut_word_from_string((char *)extensions, "GL_KHR_debug");
        cut_word_from_string((char *)extensions, "GL_OES_required_internalformat");
        cut_word_from_string((char *)extensions, "GL_OES_surfaceless_context");
        cut_word_from_string((char *)extensions, "GL_EXT_separate_shader_objects");
        cut_word_from_string((char *)extensions, "GL_KHR_context_flush_control");
        cut_word_from_string((char *)extensions, "GL_KHR_no_error");
        cut_word_from_string((char *)extensions, "GL_KHR_parallel_shader_compile");
    }

    return extensions;
}

static void *proc_resolver(void *userdata, const char *name)
{
    static int is_VC4 = -1;
    void      *address;

    /*
     * The mesa V3D driver reports some OpenGL ES extensions as supported and working
     * even though they aren't. hacked_glGetString is a workaround for this, which will
     * cut out the non-working extensions from the list of supported extensions.
     */

    if (name == NULL)
        return NULL;

    // first detect if we're running on a VideoCore 4 / using the VC4 driver.
    if ((is_VC4 == -1) && (is_VC4 = strcmp(egl.renderer, "VC4 V3D 2.1") == 0))
        printf( "detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
                "Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.");

    // if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
    if (is_VC4 && (strcmp(name, "glGetString") == 0))
        return hacked_glGetString;

    if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
        return address;

    debug("proc_resolver: could not resolve symbol \"%s\"\n", name);

    return NULL;
}

static void on_platform_message(const FlutterPlatformMessage *message, void *userdata)
{
    // FlutterEngineRunTask(engine, &task);
    // FlutterEngineSendPlatformMessageResponse(engine, message->response_handle, message->message, message->message_size);
    // debug("platform message stub");
    FlutterEngineSendPlatformMessage(engine, message);
}

static void vsync_callback(void *userdata, intptr_t baton)
{
    post_platform_task(&(struct engine_task) {
        .type = kVBlankRequest,
        .target_time = 0,
        .baton = baton
    });
}

static FlutterTransformation transformation_callback(void *userdata)
{
    // report a transform based on the current device orientation.
    static bool _transformsInitialized = false;
    static FlutterTransformation rotate0, rotate90, rotate180, rotate270;

    if (!_transformsInitialized) {
        rotate0 = (FlutterTransformation) {
            .scaleX = 1, .skewX  = 0, .transX = 0,
            .skewY  = 0, .scaleY = 1, .transY = 0,
            .pers0  = 0, .pers1  = 0, .pers2  = 1
        };

        rotate90 = FLUTTER_ROTATION_TRANSFORMATION(90);
        rotate90.transX = width;
        rotate180 = FLUTTER_ROTATION_TRANSFORMATION(180);
        rotate180.transX = width;
        rotate180.transY = height;
        rotate270 = FLUTTER_ROTATION_TRANSFORMATION(270);
        rotate270.transY = height;

        _transformsInitialized = true;
    }

    if (rotation == 0) return rotate0;
    else if (rotation == 90) return rotate90;
    else if (rotation == 180) return rotate180;
    else if (rotation == 270) return rotate270;
    else return rotate0;
}

/************************
 * PLATFORM TASK-RUNNER *
 ************************/
static bool init_message_loop()
{
    platform_thread_id = pthread_self();
    return true;
}

static bool run_message_loop(void)
{
    while (true) {
        pthread_mutex_lock(&tasklist_lock);

        // wait for a task to be inserted into the list
        while (tasklist.next == NULL)
            pthread_cond_wait(&task_added, &tasklist_lock);

        // wait for a task to be ready to be run
        uint64_t currenttime;
        while (tasklist.target_time > (currenttime = FlutterEngineGetCurrentTime())) {
            struct timespec abstargetspec;
            clock_gettime(CLOCK_REALTIME, &abstargetspec);
            uint64_t abstarget = abstargetspec.tv_nsec + abstargetspec.tv_sec * 1000000000ull - currenttime;
            abstargetspec.tv_nsec = abstarget % 1000000000;
            abstargetspec.tv_sec =  abstarget / 1000000000;

            pthread_cond_timedwait(&task_added, &tasklist_lock, &abstargetspec);
        }

        struct engine_task *task = tasklist.next;
        tasklist.next = tasklist.next->next;

        pthread_mutex_unlock(&tasklist_lock);
        if (task->type == kVBlankRequest || task->type == kVBlankReply) {
            intptr_t baton;
            bool     has_baton = false;
            uint64_t ns;

            if (task->type == kVBlankRequest) {
                if (scheduled_frames == 0) {
                    baton = task->baton;
                    has_baton = true;
                    drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
                } else {
                    batons[(i_batons + (scheduled_frames - 1)) & 63] = task->baton;
                }
                scheduled_frames++;
            } else if (task->type == kVBlankReply) {
                if (scheduled_frames > 1) {
                    baton = batons[i_batons];
                    has_baton = true;
                    i_batons = (i_batons + 1) & 63;
                    ns = task->vblank_ns;
                }
                scheduled_frames--;
            }

            if (has_baton) {
                FlutterEngineOnVsync(engine, baton, ns, ns + (1000000000ull / refresh_rate));
            }

        } else if (task->type == kUpdateOrientation) {
            rotation += ANGLE_FROM_ORIENTATION(task->orientation) - ANGLE_FROM_ORIENTATION(orientation);
            if (rotation < 0) rotation += 360;
            else if (rotation >= 360) rotation -= 360;

            orientation = task->orientation;

            // send updated window metrics to flutter
            FlutterEngineSendWindowMetricsEvent(engine, &(const FlutterWindowMetricsEvent) {
                .struct_size = sizeof(FlutterWindowMetricsEvent),

                // we send swapped width/height if the screen is rotated 90 or 270 degrees.
                .width = (rotation == 0) || (rotation == 180) ? width : height,
                .height = (rotation == 0) || (rotation == 180) ? height : width,
                .pixel_ratio = pixel_ratio
            });

        } else if (task->type == kSendPlatformMessage || task->type == kRespondToPlatformMessage) {
            if (task->type == kSendPlatformMessage) {
                FlutterEngineSendPlatformMessage(
                    engine,
                &(const FlutterPlatformMessage) {
                    .struct_size = sizeof(FlutterPlatformMessage),
                    .channel = task->channel,
                    .message = task->message,
                    .message_size = task->message_size,
                    .response_handle = task->responsehandle
                }
                );

                free(task->channel);
            } else if (task->type == kRespondToPlatformMessage) {
                FlutterEngineSendPlatformMessageResponse(
                    engine,
                    task->responsehandle,
                    task->message,
                    task->message_size
                );
            }

            free(task->message);
        } else if (FlutterEngineRunTask(engine, &task->task) != kSuccess) {
            debug("Error running platform task");
            return false;
        };

        free(task);
    }

    return true;
}

static void post_platform_task(struct engine_task *task)
{
    struct engine_task *to_insert = malloc(sizeof(struct engine_task));
    if (!to_insert) return;

    memcpy(to_insert, task, sizeof(struct engine_task));
    pthread_mutex_lock(&tasklist_lock);
    struct engine_task *this = &tasklist;
    while ((this->next) != NULL && (to_insert->target_time > this->next->target_time))
        this = this->next;

    to_insert->next = this->next;
    this->next = to_insert;
    pthread_mutex_unlock(&tasklist_lock);
    pthread_cond_signal(&task_added);
}

static void flutter_post_platform_task(FlutterTask task, uint64_t target_time, void *userdata)
{
    post_platform_task(&(struct engine_task) {
        .type = kFlutterTask,
        .task = task,
        .target_time = target_time
    });
}

static bool runs_platform_tasks_on_current_thread(void *userdata)
{
    return pthread_equal(pthread_self(), platform_thread_id) != 0;
}

static bool path_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

static bool init_paths(void)
{
    if (!path_exists(flutter.asset_bundle_path)) {
        debug("Asset Bundle Directory \"%s\" does not exist\n", flutter.asset_bundle_path);
        return false;
    }

    snprintf(flutter.kernel_blob_path, sizeof(flutter.kernel_blob_path), "%s/kernel_blob.bin",
             flutter.asset_bundle_path);
    if (!path_exists(flutter.kernel_blob_path)) {
        debug("Kernel blob does not exist inside Asset Bundle Directory.");
        return false;
    }

    if (!path_exists(flutter.icu_data_path)) {
        debug("ICU Data file not find at %s.\n", flutter.icu_data_path);
        return false;
    }
    return true;
}

static bool init_display(void)
{
    /**********************
     * DRM INITIALIZATION *
     **********************/

    drmModeRes *resources = NULL;
    drmModeConnector *connector;
    drmModeEncoder *encoder = NULL;
    int ok, area;

    if (!drm.has_device) {
        debug("Finding a suitable DRM device, since none is given...");
        drmDevicePtr devices[64] = { NULL };
        int fd = -1;

        int num_devices = drmGetDevices2(0, devices, sizeof(devices) / sizeof(drmDevicePtr));
        if (num_devices < 0) {
            debug("could not query drm device list: %s\n", strerror(-num_devices));
            return false;
        }

        debug("looking for a suitable DRM device from %d available DRM devices...\n", num_devices);
        for (int i = 0; i < num_devices; i++) {
            drmDevicePtr device = devices[i];

            debug("  devices[%d]: \n", i);

            debug("    available nodes: ");
            if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) debug("DRM_NODE_PRIMARY, ");
            if (device->available_nodes & (1 << DRM_NODE_CONTROL)) debug("DRM_NODE_CONTROL, ");
            if (device->available_nodes & (1 << DRM_NODE_RENDER))  debug("DRM_NODE_RENDER");
            debug("");

            for (int j = 0; j < DRM_NODE_MAX; j++) {
                if (device->available_nodes & (1 << j)) {
                    debug("    nodes[%s] = \"%s\"\n",
                          j == DRM_NODE_PRIMARY ? "DRM_NODE_PRIMARY" :
                          j == DRM_NODE_CONTROL ? "DRM_NODE_CONTROL" :
                          j == DRM_NODE_RENDER  ? "DRM_NODE_RENDER" : "unknown",
                          device->nodes[j]
                         );
                }
            }

            debug("    bustype: %s\n",
                  device->bustype == DRM_BUS_PCI ? "DRM_BUS_PCI" :
                  device->bustype == DRM_BUS_USB ? "DRM_BUS_USB" :
                  device->bustype == DRM_BUS_PLATFORM ? "DRM_BUS_PLATFORM" :
                  device->bustype == DRM_BUS_HOST1X ? "DRM_BUS_HOST1X" :
                  "unknown"
                 );

            if (device->bustype == DRM_BUS_PLATFORM) {
                debug("    businfo.fullname: %s\n", device->businfo.platform->fullname);
                // seems like deviceinfo.platform->compatible is not really used.
                //debug("    deviceinfo.compatible: %s\n", device->deviceinfo.platform->compatible);
            }

            // we want a device that's DRM_NODE_PRIMARY and that we can call a drmModeGetResources on.
            if (drm.has_device) continue;
            if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY))) continue;

            debug("    opening DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
            fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
            if (fd < 0) {
                debug("      could not open DRM device candidate at \"%s\": %s\n", device->nodes[DRM_NODE_PRIMARY],
                      strerror(errno));
                continue;
            }

            debug("    getting resources of DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
            resources = drmModeGetResources(fd);
            if (resources == NULL) {
                debug("      could not query DRM resources for DRM device candidate at \"%s\":",
                      device->nodes[DRM_NODE_PRIMARY]);
                if ((errno = EOPNOTSUPP) || (errno = EINVAL)) debug("doesn't look like a modeset device.");
                else                                          debug("%s\n", strerror(errno));
                close(fd);
                continue;
            }

            // we found our DRM device.
            debug("    chose \"%s\" as its DRM device.\n", device->nodes[DRM_NODE_PRIMARY]);
            drm.fd = fd;
            drm.has_device = true;
            snprintf(drm.device, sizeof(drm.device) - 1, "%s", device->nodes[DRM_NODE_PRIMARY]);
        }

        if (!drm.has_device) {
            debug("couldn't find a usable DRM device");
            return false;
        }
    }

    if (drm.fd <= 0) {
        debug("Opening DRM device...");
        drm.fd = open(drm.device, O_RDWR);
        if (drm.fd < 0) {
            debug("Could not open DRM device");
            return false;
        }
    }

    if (!resources) {
        debug("Getting DRM resources...");
        resources = drmModeGetResources(drm.fd);
        if (resources == NULL) {
            if ((errno == EOPNOTSUPP) || (errno = EINVAL))
                debug("%s doesn't look like a modeset device\n", drm.device);
            else
                debug("drmModeGetResources failed: %s\n", strerror(errno));

            return false;
        }
    }

    debug("Finding a connected connector from %d available connectors...\n", resources->count_connectors);
    connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm.fd, resources->connectors[i]);

        debug("  connectors[%d]: connected? %s, type: 0x%02X%s, %umm x %umm\n",
              i,
              (conn->connection == DRM_MODE_CONNECTED) ? "yes" :
              (conn->connection == DRM_MODE_DISCONNECTED) ? "no" : "unknown",
              conn->connector_type,
              (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA) ? " (HDMI-A)" :
              (conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) ? " (HDMI-B)" :
              (conn->connector_type == DRM_MODE_CONNECTOR_DSI) ? " (DSI)" :
              (conn->connector_type == DRM_MODE_CONNECTOR_DisplayPort) ? " (DisplayPort)" : "",
              conn->mmWidth, conn->mmHeight
             );

        if ((connector == NULL) && (conn->connection == DRM_MODE_CONNECTED)) {
            connector = conn;

            // only update the physical size of the display if the values
            //   are not yet initialized / not set with a commandline option
            if ((width_mm == 0) && (height_mm == 0)) {
                if ((conn->mmWidth == 160) && (conn->mmHeight == 90)) {
                    // if width and height is exactly 160mm x 90mm, the values are probably bogus.
                    width_mm = 0;
                    height_mm = 0;
                } else if ((conn->connector_type == DRM_MODE_CONNECTOR_DSI) && (conn->mmWidth == 0)
                           && (conn->mmHeight == 0)) {
                    // if it's connected via DSI, and the width & height are 0,
                    //   it's probably the official 7 inch touchscreen.
                    width_mm = 155;
                    height_mm = 86;
                } else {
                    width_mm = conn->mmWidth;
                    height_mm = conn->mmHeight;
                }
            }
        } else {
            drmModeFreeConnector(conn);
        }
    }
    if (!connector) {
        debug("could not find a connected connector!");
        return false;
    }

    debug("Choosing DRM mode from %d available modes...\n", connector->count_modes);
    bool found_preferred = false;
    for (int i = 0, area = 0; i < connector->count_modes; i++) {
        drmModeModeInfo *current_mode = &connector->modes[i];

        debug("  modes[%d]: name: \"%s\", %ux%u%s, %uHz, type: %u, flags: %u\n",
              i, current_mode->name, current_mode->hdisplay, current_mode->vdisplay,
              (current_mode->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "p",
              current_mode->vrefresh, current_mode->type, current_mode->flags
             );

        if (found_preferred) continue;

        // we choose the highest resolution with the highest refresh rate, preferably non-interlaced (= progressive) here.
        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (( current_area  > area) ||
                ((current_area == area) && (current_mode->vrefresh >  refresh_rate)) ||
                ((current_area == area) && (current_mode->vrefresh == refresh_rate)
                 && ((current_mode->flags & DRM_MODE_FLAG_INTERLACE) == 0)) ||
                ( current_mode->type & DRM_MODE_TYPE_PREFERRED)) {

            drm.mode = current_mode;
            width = current_mode->hdisplay;
            height = current_mode->vdisplay;
            refresh_rate = current_mode->vrefresh;
            area = current_area;
            orientation = width >= height ? kLandscapeLeft : kPortraitUp;

            // if the preferred DRM mode is bogus, we're screwed.
            if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
                debug("    this mode is preferred by DRM. (DRM_MODE_TYPE_PREFERRED)");
                found_preferred = true;
            }
        }
    }

    if (!drm.mode) {
        debug("could not find a suitable DRM mode!");
        return false;
    }

    // calculate the pixel ratio
    if (pixel_ratio == 0.0) {
        if ((width_mm == 0) || (height_mm == 0)) {
            pixel_ratio = 1.0;
        } else {
            pixel_ratio = (10.0 * width) / (width_mm * 38.0);
            if (pixel_ratio < 1.0) pixel_ratio = 1.0;
        }
    }

    debug("Display properties:\n  %u x %u, %uHz\n  %umm x %umm\n  pixel_ratio = %f\n", width, height,
          refresh_rate, width_mm, height_mm, pixel_ratio);

    debug("Finding DRM encoder...");
    for (int i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm.crtc_id = encoder->crtc_id;
    } else {
        debug("could not find a suitable crtc!");
        return false;
    }

    for (int i = 0; i < resources->count_crtcs; i++) {
        if (resources->crtcs[i] == drm.crtc_id) {
            drm.crtc_index = i;
            break;
        }
    }

    drmModeFreeResources(resources);

    drm.connector_id = connector->connector_id;

    /**********************
     * GBM INITIALIZATION *
     **********************/
    debug("Creating GBM device");
    gbm.device = gbm_create_device(drm.fd);
    gbm.format = DRM_FORMAT_RGB565;
    gbm.surface = NULL;
    gbm.modifier = DRM_FORMAT_MOD_LINEAR;

    gbm.surface = gbm_surface_create_with_modifiers(gbm.device, width, height, gbm.format, &gbm.modifier, 1);

    if (!gbm.surface) {
        if (gbm.modifier != DRM_FORMAT_MOD_LINEAR) {
            debug("GBM Surface creation modifiers requested but not supported by GBM");
            return false;
        }
        gbm.surface = gbm_surface_create(gbm.device, width, height, gbm.format,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!gbm.surface) {
        debug("failed to create GBM surface");
        return false;
    }

    /**********************
     * EGL INITIALIZATION *
     **********************/
    EGLint major, minor;

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE
    };

    const char *egl_exts_client, *egl_exts_dpy, *gl_exts;

    debug("Querying EGL client extensions...");
    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    egl.eglGetPlatformDisplayEXT = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
    debug("Getting EGL display for GBM device...");
    if (egl.eglGetPlatformDisplayEXT) egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device,
                                                                                     NULL);
    else                              egl.display = eglGetDisplay((void *) gbm.device);

    if (!egl.display) {
        debug("Couldn't get EGL display");
        return false;
    }

    debug("Initializing EGL...");
    if (!eglInitialize(egl.display, &major, &minor)) {
        debug("failed to initialize EGL");
        return false;
    }

    debug("Querying EGL display extensions...");
    egl_exts_dpy = eglQueryString(egl.display, EGL_EXTENSIONS);
    egl.modifiers_supported = strstr(egl_exts_dpy, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;

    debug("Using display %p with EGL version %d.%d\n", egl.display, major, minor);
    debug("===================================");
    debug("EGL information:");
    debug("  version: %s\n", eglQueryString(egl.display, EGL_VERSION));
    debug("  vendor: \"%s\"\n", eglQueryString(egl.display, EGL_VENDOR));
    debug("  client extensions: \"%s\"\n", egl_exts_client);
    debug("  display extensions: \"%s\"\n", egl_exts_dpy);
    debug("===================================");

    debug("Binding OpenGL ES API...");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        debug("failed to bind OpenGL ES API");
        return false;
    }


    debug("Choosing EGL config...");
    EGLint count = 0, matched = 0;
    EGLConfig *configs;
    bool _found_matching_config = false;

    if (!eglGetConfigs(egl.display, NULL, 0, &count) || count < 1) {
        debug("No EGL configs to choose from.");
        return false;
    }

    configs = malloc(count * sizeof(EGLConfig));
    if (!configs) return false;

    debug("Finding EGL configs with appropriate attributes...");
    if (!eglChooseConfig(egl.display, config_attribs, configs, count, &matched) || !matched) {
        debug("No EGL configs with appropriate attributes.");
        free(configs);
        return false;
    }
    debug("eglChooseConfig done");

    if (!gbm.format) {
        debug("!gbm.format");
        _found_matching_config = true;
    } else {
        debug("gbm.format");
        for (int i = 0; i < count; i++) {
            EGLint id;
            debug("checking id=%d\n", id);
            if (!eglGetConfigAttrib(egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &id))    continue;

            if (id == gbm.format) {
                debug("gbm.format=%d\n", id);

                egl.config = configs[i];
                _found_matching_config = true;
                break;
            }
        }
    }
    free(configs);

    if (!_found_matching_config) {
        debug("Could not find context with appropriate attributes and matching native visual ID.");
        return false;
    }

    debug("Creating EGL context...");
    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);
    if (egl.context == NULL) {
        debug("failed to create EGL context");
        return false;
    }

    debug("Creating EGL window surface...");
    egl.surface = eglCreateWindowSurface(egl.display, egl.config, (EGLNativeWindowType) gbm.surface, NULL);
    if (egl.surface == EGL_NO_SURFACE) {
        debug("failed to create EGL window surface");
        return false;
    }

    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
        debug("Could not make EGL context current to get OpenGL information");
        return false;
    }

    egl.renderer = (char *) glGetString(GL_RENDERER);

    gl_exts = (char *) glGetString(GL_EXTENSIONS);
    debug("===================================");
    debug("OpenGL ES information:");
    debug("  version: \"%s\"\n", glGetString(GL_VERSION));
    debug("  shading language version: \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    debug("  vendor: \"%s\"\n", glGetString(GL_VENDOR));
    debug("  renderer: \"%s\"\n", egl.renderer);
    debug("  extensions: \"%s\"\n", gl_exts);
    debug("===================================");

    if (strncmp(egl.renderer, "llvmpipe", sizeof("llvmpipe") - 1) == 0)
        debug("Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer. Make sure to run as root");

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
        return false;
    }

    debug("Setting CRTC...");
    ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    if (ok) {
        debug("failed to set mode: %s\n", strerror(errno));
        return false;
    }

    debug("Clearing current context...");
    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        debug("Could not clear EGL context");
        return false;
    }

    debug("finished display setup!");

    return true;
}

static void destroy_display(void)
{
    debug("destroy_display not yet implemented");
}

static bool init_application(void)
{
    int ok, _errno;

    // configure flutter rendering
    flutter.renderer_config.type = kOpenGL;
    flutter.renderer_config.open_gl.struct_size     = sizeof(flutter.renderer_config.open_gl);
    flutter.renderer_config.open_gl.make_current    = make_current;
    flutter.renderer_config.open_gl.clear_current   = clear_current;
    flutter.renderer_config.open_gl.present         = present;
    flutter.renderer_config.open_gl.fbo_callback    = fbo_callback;
    flutter.renderer_config.open_gl.gl_proc_resolver = proc_resolver;
    flutter.renderer_config.open_gl.surface_transformation = transformation_callback;

    // configure flutter
    flutter.args.struct_size                = sizeof(FlutterProjectArgs);
    flutter.args.assets_path                = flutter.asset_bundle_path;
    flutter.args.icu_data_path              = flutter.icu_data_path;
    flutter.args.isolate_snapshot_data_size = 0;
    flutter.args.isolate_snapshot_data      = NULL;
    flutter.args.isolate_snapshot_instructions_size = 0;
    flutter.args.isolate_snapshot_instructions   = NULL;
    flutter.args.vm_snapshot_data_size      = 0;
    flutter.args.vm_snapshot_data           = NULL;
    flutter.args.vm_snapshot_instructions_size = 0;
    flutter.args.vm_snapshot_instructions   = NULL;
    flutter.args.command_line_argc          = flutter.engine_argc;
    flutter.args.command_line_argv          = flutter.engine_argv;
    flutter.args.platform_message_callback  = on_platform_message;
    flutter.args.custom_task_runners        = &(FlutterCustomTaskRunners) {
        .struct_size = sizeof(FlutterCustomTaskRunners),
        .platform_task_runner = &(FlutterTaskRunnerDescription) {
            .struct_size = sizeof(FlutterTaskRunnerDescription),
            .user_data = NULL,
            .runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
            .post_task_callback = &flutter_post_platform_task
        }
    };

    // only enable vsync if the kernel supplies valid vblank timestamps
    uint64_t ns = 0;
    ok = drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
    if (ok != 0) _errno = errno;

    if ((ok == 0) && (ns != 0)) {
        drm.disable_vsync = false;
        flutter.args.vsync_callback = vsync_callback;
    } else {
        drm.disable_vsync = true;
        if (ok != 0) {
            debug("Could not get last vblank timestamp. %s", strerror(_errno));
        } else {
            debug("Kernel didn't return a valid vblank timestamp. (timestamp == 0)");
        }
        debug("VSync will be disabled");
    }

    // spin up the engine
    FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &flutter.renderer_config,
                                                   &flutter.args, NULL, &engine);
    if (_result != kSuccess) {
        debug("Could not run the flutter engine");
        return false;
    } else {
        debug("flutter engine successfully started up.");
    }

    engine_running = true;

    // update window size
    ok = FlutterEngineSendWindowMetricsEvent(
             engine,
    &(FlutterWindowMetricsEvent) {
        .struct_size = sizeof(FlutterWindowMetricsEvent), .width = width, .height = height, .pixel_ratio = pixel_ratio
    }
         ) == kSuccess;

    if (!ok) {
        debug("Could not update Flutter application size.");
        return false;
    }

    return true;
}

static void destroy_application(void)
{
    if (engine != NULL) {
        if (FlutterEngineShutdown(engine) != kSuccess)
            debug("Could not shutdown the flutter engine.");

        engine = NULL;
    }
}

static bool init_io(void)
{
    FlutterPointerEvent flutterevents[64] = {0};
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
		.timestamp = (size_t) (FlutterEngineGetCurrentTime()*1000),
		.x = 0,
		.y = 0,
		.signal_kind = kFlutterPointerSignalKindNone,
		.device_kind = kFlutterPointerDeviceKindTouch,
		.device = mousepointer.flutter_slot_id,
		.buttons = 0
	};
    return FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) == kSuccess;
}

static void process_io_events(int fd) {
    // Read as many the input events as possible
    ssize_t rd = read(fd, io_input_buffer, sizeof(io_input_buffer));
    if (rd < 0)
        error("read failed");
    if (rd % sizeof(struct input_event))
        error("read returned %d which is not a multiple of %d!", (int) rd, (int) sizeof(struct input_event));

    // code ABS_MT_TRACKING_ID = 0x39 (57)
    // code ABS_X              = 0x00
    // code ABS_Y              = 0x01
    // code ABS_Z              = 0x02

    FlutterPointerEvent flutterevents[64] = {0};
	size_t i_flutterevent = 0;

    size_t event_count = rd / sizeof(struct input_event);
    for (size_t i = 0; i < event_count; i++) {
        if (io_input_buffer[i].type == EV_ABS) {
            // debug("EV_ABS event code=%d value=%d\r\n", io_input_buffer[i].code, io_input_buffer[i].value);

            if (io_input_buffer[i].code == ABS_X) {
                mousepointer.x = io_input_buffer[i].value;
            } else if (io_input_buffer[i].code == ABS_Y) {
                mousepointer.y = io_input_buffer[i].value;
            } else if (io_input_buffer[i].code == ABS_MT_TRACKING_ID && io_input_buffer[i].value == -1) {
            }

        } else if(io_input_buffer[i].type == EV_KEY) {
            if (io_input_buffer[i].code == BTN_TOUCH) {
                mousepointer.phase = io_input_buffer[i].value ? kDown : kUp;
            } else {
                debug("unknown EV_KEY code=%d value=%d\r\n", io_input_buffer[i].code, io_input_buffer[i].value);
            }
        } else if(io_input_buffer[i].type == EV_SYN && io_input_buffer[i].code == SYN_REPORT) {
            // we don't want to send an event to flutter if nothing changed.
            if (mousepointer.phase == kCancel) continue;

            // convert raw pixel coordinates to flutter pixel coordinates
            // (raw pixel coordinates don't respect screen rotation)
            double flutterx, fluttery;
            if (rotation == 0) {
                flutterx = mousepointer.x;
                fluttery = mousepointer.y;
            } else if (rotation == 90) {
                flutterx = mousepointer.y;
                fluttery = width - mousepointer.x;
            } else if (rotation == 180) {
                flutterx = width - mousepointer.x;
                fluttery = height - mousepointer.y;
            } else if (rotation == 270) {
                flutterx = height - mousepointer.y;
                fluttery = mousepointer.x;
            }

            flutterevents[i_flutterevent++] = (FlutterPointerEvent) {
                .struct_size = sizeof(FlutterPointerEvent),
                .phase = mousepointer.phase,
                // .timestamp = FlutterEngineGetCurrentTime(),
                .timestamp = io_input_buffer[i].time.tv_sec*1000000 + io_input_buffer[i].time.tv_usec,
                .x = flutterx, .y = fluttery,
                .device = mousepointer.flutter_slot_id,
                .signal_kind = kFlutterPointerSignalKindNone,
                .scroll_delta_x = 0, .scroll_delta_y = 0,
                .device_kind = kFlutterPointerDeviceKindTouch,
                .buttons = 0
            };
            debug("flutterevent[%d]", i_flutterevent - 1);
            debug(" .x=%05f", flutterevents[i_flutterevent -1].x);
            debug(" .y=%05f", flutterevents[i_flutterevent -1].y);
            debug(" .phase=%d\r\n", flutterevents[i_flutterevent -1].phase);
            mousepointer.phase = kCancel;
        } else {
            debug("unknown input_event type=%d\r\n", io_input_buffer[i].type);
        }
    }

    if (i_flutterevent == 0) return;

	// now, send the data to the flutter engine
    debug("sending %d flutter pointer events\r\n", i_flutterevent);
	if (FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) != kSuccess) {
		debug("could not send pointer events to flutter engine\r\n");
	}
}
 // cmd("/root/flutter_embedder /srv/erlang/lib/nerves_example-0.1.0/priv/flutter_assets /srv/erlang/lib/flutter_embedder-0.1.0/priv/icudtl.dat")
static void *io_loop(void *userdata)
{
    const char *input_path = "/dev/input/event0";
    int fd = open(input_path, O_RDONLY);
    if (errno == EACCES && getuid() != 0)
        error("You do not have access to %s.", input_path);

    while (engine_running) {
        // debug("io poll");
        struct pollfd fdset[2];

        fdset[0].fd = fd;
        fdset[0].events = (POLLIN | POLLPRI | POLLHUP);
        fdset[0].revents = 0;

        fdset[1].fd = drm.fd;
        fdset[1].events = (POLLIN | POLLPRI | POLLHUP);
        fdset[1].revents = 0;

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

static bool run_io_thread(void)
{
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

    return true;
}

int main(int argc, char **argv)
{

#ifdef DEBUG
#ifdef LOG_PATH
    log_location = fopen(LOG_PATH, "w");
#endif
#endif

    if (argc != 3) {
        error("Invalid Arguments");
        exit(EXIT_FAILURE);
    }

    snprintf(flutter.asset_bundle_path, sizeof(flutter.asset_bundle_path), "%s", argv[1]);
    snprintf(flutter.icu_data_path, sizeof(flutter.icu_data_path), "%s", argv[2]);

    // check if asset bundle path is valid
    if (!init_paths()) {
        error("init_paths");
        return EXIT_FAILURE;
    }

    if (!init_message_loop()) {
        error("init_message_loop");
        return EXIT_FAILURE;
    }

    // initialize display
    debug("initializing display...");
    if (!init_display()) {
        return EXIT_FAILURE;
    }

    // initialize application
    debug("Initializing Application...");
    if (!init_application()) {
        debug("init_application failed");
        return EXIT_FAILURE;
    }

    debug("Initializing Input devices...");
    if (!init_io()) {
        debug("init_io failed");
        return EXIT_FAILURE;
    }

    // read input events
    debug("Running IO thread...");
    run_io_thread();

    // run message loop
    debug("Running message loop...");
    run_message_loop();

    // exit
    destroy_application();
    destroy_display();

    return EXIT_SUCCESS;
}
