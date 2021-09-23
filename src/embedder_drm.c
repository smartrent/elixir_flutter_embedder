#define _GNU_SOURCE

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
#include <sys/eventfd.h>

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

#define DEBUG
#include "flutter_embedder.h"
#include "flutter_embedder.h"
#include "embedder_platform_message.h"

#ifdef DEBUG
#define LOG_PATH "/tmp/log.txt"
#define log_location stderr
#define debug(...)                          \
    do                                      \
    {                                       \
        fprintf(log_location, __VA_ARGS__); \
        fprintf(log_location, "\r\n");      \
        fflush(log_location);               \
    } while (0)
#define error(...)          \
    do                      \
    {                       \
        debug(__VA_ARGS__); \
    } while (0)
#else
#define debug(...)
#define error(...)                    \
    do                                \
    {                                 \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "");          \
    } while (0)
#endif

#define EGL_PLATFORM_GBM_KHR 0x31D7

typedef enum
{
    kVBlankRequest,
    kVBlankReply,
    kFlutterTask
} engine_task_type;

struct engine_task
{
    struct engine_task *next;
    engine_task_type type;
    union
    {
        FlutterTask task;
        struct
        {
            uint64_t vblank_ns;
            intptr_t baton;
        };
        struct
        {
            char *channel;
            const FlutterPlatformMessageResponseHandle *response_handle;
            size_t message_size;
            uint8_t *message;
        };
    };
    uint64_t target_time;
};

struct drm_fb
{
    struct gbm_bo *bo;
    uint32_t fb_id;
};

struct pageflip_data
{
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
static uint32_t refresh_rate = 60;
static uint32_t refresh_period_ns = 16666666;

/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
static double pixel_ratio = 0.0;

static struct
{
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

static struct
{
    struct gbm_device *device;
    struct gbm_surface *surface;
    uint32_t format;
    uint64_t modifier;
} gbm = {0};

static struct
{
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;

    bool modifiers_supported;
    char *renderer;

    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display, const EGLint *attrib_list);
    EGLSurface (*eglCreatePlatformWindowSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_window,
                                                    const EGLint *attrib_list);
    EGLSurface (*eglCreatePlatformPixmapSurfaceEXT)(EGLDisplay dpy, EGLConfig config, void *native_pixmap,
                                                    const EGLint *attrib_list);
} egl = {0};

static struct
{
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

static struct engine_task *tasklist = NULL;
static pthread_mutex_t tasklist_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t task_added = PTHREAD_COND_INITIALIZER;

static FlutterEngine engine;
static _Atomic bool engine_running = false;

// IO stuff

// position & pointer phase of a mouse pointer / multitouch slot
// A 10-finger multi-touch display has 10 slots and each of them have their own position, tracking id, etc.
// All mouses / touchpads share the same mouse pointer.
struct mousepointer_mtslot
{
    // the MT tracking ID used to track this touch.
    int id;
    int32_t flutter_slot_id;
    double x, y;
    FlutterPointerPhase phase;
};

static struct mousepointer_mtslot mousepointer;
static pthread_t io_thread_id;
static pthread_t comms_thread_id;
#define MAX_EVENTS_PER_READ 64
static struct input_event io_input_buffer[MAX_EVENTS_PER_READ];

static plat_msg_queue_t queue;
static struct erlcmd handler;

#define ERLCMD_FD_POLL 0
#define EVENTFD_FD_POLL 1
#define ENGINE_STDOUT_FD_POLL 2
#define ENGINE_STDERR_FD_POLL 3
#define NUM_POLLFDS 4
static int num_pollfds = NUM_POLLFDS;
static struct pollfd fdset[NUM_POLLFDS];
static int capstdout[2];
static int capstderr[2];
static char capstdoutbuffer[ERLCMD_BUF_SIZE];

static bool make_current(void *userdata)
{
    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE)
    {
        debug("make_current: could not make the context current.");
        return false;
    }

    return true;
}

static bool clear_current(void *userdata)
{
    if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE)
    {
        debug("clear_current: could not clear the current context.");
        return false;
    }

    return true;
}

static void pageflip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *userdata)
{
    FlutterEngineTraceEventInstant("pageflip");
    post_platform_task(&(struct engine_task){
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
    if (fb)
        return fb;

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
    for (int i = 0; i < num_planes; i++)
    {
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        handles[i] = gbm_bo_get_handle(bo).u32;
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifiers[0];
    }

    uint32_t flags = 0;
    if (modifiers[0])
    {
        flags = DRM_MODE_FB_MODIFIERS;
    }

    int ok = drmModeAddFB2WithModifiers(drm.fd, width, height, format, handles, strides, offsets, modifiers,
                                        &fb->fb_id, flags);

    if (ok)
    {
        if (flags)
            debug("drm_fb_get_from_bo: modifiers failed!");

        memcpy(handles, (uint32_t[4]){gbm_bo_get_handle(bo).u32, 0, 0, 0}, 16);

        memcpy(strides, (uint32_t[4]){gbm_bo_get_stride(bo), 0, 0, 0}, 16);

        memset(offsets, 0, 16);

        ok = drmModeAddFB2(drm.fd, width, height, format, handles, strides, offsets, &fb->fb_id, 0);
    }

    if (ok)
    {
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

    int ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
    if (ok)
    {
        perror("failed to queue page flip");
        return false;
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
    char *word_in_str = strstr(string, word);

    // check if the given word is surrounded by spaces in the string
    if (word_in_str && ((word_in_str == string) || (word_in_str[-1] == ' ')) && ((word_in_str[word_length] == 0) || (word_in_str[word_length] == ' ')))
    {
        if (word_in_str[word_length] == ' ')
            word_length++;

        int i = 0;
        do
        {
            word_in_str[i] = word_in_str[i + word_length];
        } while (word_in_str[i++ + word_length] != 0);
    }
}

static const GLubyte *hacked_glGetString(GLenum name)
{
    static GLubyte *extensions = NULL;

    if (name != GL_EXTENSIONS)
        return glGetString(name);

    if (extensions == NULL)
    {
        GLubyte *orig_extensions = (GLubyte *)glGetString(GL_EXTENSIONS);

        extensions = malloc(strlen((const char *)orig_extensions) + 1);
        if (!extensions)
        {
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
    void *address;

    /*
     * The mesa V3D driver reports some OpenGL ES extensions as supported and working
     * even though they aren't. hacked_glGetString is a workaround for this, which will
     * cut out the non-working extensions from the list of supported extensions.
     */

    if (name == NULL)
        return NULL;

    // first detect if we're running on a VideoCore 4 / using the VC4 driver.
    if ((is_VC4 == -1) && (is_VC4 = strcmp(egl.renderer, "VC4 V3D 2.1") == 0))
        printf("detected VideoCore IV as underlying graphics chip, and VC4 as the driver.\n"
               "Reporting modified GL_EXTENSIONS string that doesn't contain non-working extensions.");

    // if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
    if (is_VC4 && (strcmp(name, "glGetString") == 0))
        return hacked_glGetString;

    if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
        return address;

    debug("proc_resolver: could not resolve symbol \"%s\"\n", name);

    return NULL;
}

static void vsync_callback(void *userdata, intptr_t baton)
{
    post_platform_task(&(struct engine_task){
        .type = kVBlankRequest,
        .target_time = 0,
        .baton = baton});
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
    while (true)
    {
        pthread_mutex_lock(&tasklist_lock);

        // wait for a task to be inserted into the list
        while (tasklist == NULL)
            pthread_cond_wait(&task_added, &tasklist_lock);

        // wait for a task to be ready to be run
        uint64_t currenttime;
        while (tasklist->target_time > (currenttime = FlutterEngineGetCurrentTime()))
        {
            struct timespec abstargetspec;
            clock_gettime(CLOCK_REALTIME, &abstargetspec);
            uint64_t abstarget = abstargetspec.tv_nsec + abstargetspec.tv_sec * 1000000000ull +
                                 (tasklist->target_time - currenttime);
            abstargetspec.tv_nsec = abstarget % 1000000000;
            abstargetspec.tv_sec = abstarget / 1000000000;

            pthread_cond_timedwait(&task_added, &tasklist_lock, &abstargetspec);
        }

        struct engine_task *task = tasklist;
        tasklist = tasklist->next;

        pthread_mutex_unlock(&tasklist_lock);
        if (task->type == kVBlankRequest)
        {
            if (scheduled_frames == 0)
            {
                uint64_t ns;
                drmCrtcGetSequence(drm.fd, drm.crtc_id, NULL, &ns);
                FlutterEngineOnVsync(engine, task->baton, ns, ns + refresh_period_ns);
            }
            else
            {
                batons[(i_batons + (scheduled_frames - 1)) & 63] = task->baton;
            }
            scheduled_frames++;
        }
        else if (task->type == kVBlankReply)
        {
            if (scheduled_frames > 1)
            {
                intptr_t baton = batons[i_batons];
                i_batons = (i_batons + 1) & 63;
                uint64_t ns = task->vblank_ns;
                FlutterEngineOnVsync(engine, baton, ns, ns + refresh_period_ns);
            }
            scheduled_frames--;
        }
        else if (task->type == kFlutterTask)
        {
            if (FlutterEngineRunTask(engine, &task->task) != kSuccess)
            {
                debug("Error running platform task");
                return false;
            }
        }

        free(task);
    }

    return true;
}

static void post_platform_task(struct engine_task *task)
{
    struct engine_task *to_insert = malloc(sizeof(struct engine_task));
    if (!to_insert)
        return;

    memcpy(to_insert, task, sizeof(struct engine_task));
    pthread_mutex_lock(&tasklist_lock);
    if (tasklist == NULL || to_insert->target_time < tasklist->target_time)
    {
        to_insert->next = tasklist;
        tasklist = to_insert;
    }
    else
    {
        struct engine_task *prev = tasklist;
        struct engine_task *current = tasklist->next;
        while (current != NULL && to_insert->target_time > current->target_time)
        {
            prev = current;
            current = current->next;
        }
        to_insert->next = current;
        prev->next = to_insert;
    }

    pthread_mutex_unlock(&tasklist_lock);
    pthread_cond_signal(&task_added);
}

static void flutter_post_platform_task(FlutterTask task, uint64_t target_time, void *userdata)
{
    post_platform_task(&(struct engine_task){
        .type = kFlutterTask,
        .task = task,
        .target_time = target_time});
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
    if (!path_exists(flutter.asset_bundle_path))
    {
        debug("Asset Bundle Directory \"%s\" does not exist\n", flutter.asset_bundle_path);
        return false;
    }

    snprintf(flutter.kernel_blob_path, sizeof(flutter.kernel_blob_path), "%s/kernel_blob.bin",
             flutter.asset_bundle_path);
    if (!path_exists(flutter.kernel_blob_path))
    {
        debug("Kernel blob does not exist inside Asset Bundle Directory.");
        return false;
    }

    if (!path_exists(flutter.icu_data_path))
    {
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
    int ok;

    if (!drm.has_device)
    {
        debug("Finding a suitable DRM device, since none is given...");
        drmDevicePtr devices[64] = {NULL};
        int fd = -1;

        int num_devices = drmGetDevices2(0, devices, sizeof(devices) / sizeof(drmDevicePtr));
        if (num_devices < 0)
        {
            debug("could not query drm device list: %s\n", strerror(-num_devices));
            return false;
        }

        debug("looking for a suitable DRM device from %d available DRM devices...\n", num_devices);
        for (int i = 0; i < num_devices; i++)
        {
            drmDevicePtr device = devices[i];

            debug("  devices[%d]: \n", i);

            debug("    available nodes: ");
            if (device->available_nodes & (1 << DRM_NODE_PRIMARY))
                debug("DRM_NODE_PRIMARY, ");
            if (device->available_nodes & (1 << DRM_NODE_CONTROL))
                debug("DRM_NODE_CONTROL, ");
            if (device->available_nodes & (1 << DRM_NODE_RENDER))
                debug("DRM_NODE_RENDER");

            for (int j = 0; j < DRM_NODE_MAX; j++)
            {
                if (device->available_nodes & (1 << j))
                {
                    debug("    nodes[%s] = \"%s\"\n",
                          j == DRM_NODE_PRIMARY ? "DRM_NODE_PRIMARY" : j == DRM_NODE_CONTROL ? "DRM_NODE_CONTROL"
                                                                   : j == DRM_NODE_RENDER    ? "DRM_NODE_RENDER"
                                                                                             : "unknown",
                          device->nodes[j]);
                }
            }

            debug("    bustype: %s\n",
                  device->bustype == DRM_BUS_PCI ? "DRM_BUS_PCI" : device->bustype == DRM_BUS_USB    ? "DRM_BUS_USB"
                                                               : device->bustype == DRM_BUS_PLATFORM ? "DRM_BUS_PLATFORM"
                                                               : device->bustype == DRM_BUS_HOST1X   ? "DRM_BUS_HOST1X"
                                                                                                     : "unknown");

            if (device->bustype == DRM_BUS_PLATFORM)
            {
                debug("    businfo.fullname: %s\n", device->businfo.platform->fullname);
                // seems like deviceinfo.platform->compatible is not really used.
                //debug("    deviceinfo.compatible: %s\n", device->deviceinfo.platform->compatible);
            }

            // we want a device that's DRM_NODE_PRIMARY and that we can call a drmModeGetResources on.
            if (drm.has_device)
                continue;
            if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
                continue;

            debug("    opening DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
            fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
            if (fd < 0)
            {
                debug("      could not open DRM device candidate at \"%s\": %s\n", device->nodes[DRM_NODE_PRIMARY],
                      strerror(errno));
                continue;
            }

            debug("    getting resources of DRM device candidate at \"%s\"...\n", device->nodes[DRM_NODE_PRIMARY]);
            resources = drmModeGetResources(fd);
            if (resources == NULL)
            {
                debug("      could not query DRM resources for DRM device candidate at \"%s\":",
                      device->nodes[DRM_NODE_PRIMARY]);
                if ((errno = EOPNOTSUPP) || (errno = EINVAL))
                    debug("doesn't look like a modeset device.");
                else
                    debug("%s\n", strerror(errno));
                close(fd);
                continue;
            }

            // we found our DRM device.
            debug("    chose \"%s\" as its DRM device.\n", device->nodes[DRM_NODE_PRIMARY]);
            drm.fd = fd;
            drm.has_device = true;
            snprintf(drm.device, sizeof(drm.device) - 1, "%s", device->nodes[DRM_NODE_PRIMARY]);
        }

        if (!drm.has_device)
        {
            debug("couldn't find a usable DRM device");
            return false;
        }
    }

    if (drm.fd <= 0)
    {
        debug("Opening DRM device...");
        drm.fd = open(drm.device, O_RDWR);
        if (drm.fd < 0)
        {
            debug("Could not open DRM device");
            return false;
        }
    }

    if (!resources)
    {
        debug("Getting DRM resources...");
        resources = drmModeGetResources(drm.fd);
        if (resources == NULL)
        {
            if ((errno == EOPNOTSUPP) || (errno = EINVAL))
                debug("%s doesn't look like a modeset device\n", drm.device);
            else
                debug("drmModeGetResources failed: %s\n", strerror(errno));

            return false;
        }
    }

    debug("Finding a connected connector from %d available connectors...\n", resources->count_connectors);
    connector = NULL;
    for (int i = 0; i < resources->count_connectors; i++)
    {
        drmModeConnector *conn = drmModeGetConnector(drm.fd, resources->connectors[i]);

        debug("  connectors[%d]: connected? %s, type: 0x%02X%s, %umm x %umm\n",
              i,
              (conn->connection == DRM_MODE_CONNECTED) ? "yes" : (conn->connection == DRM_MODE_DISCONNECTED) ? "no"
                                                                                                             : "unknown",
              conn->connector_type,
              (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA) ? " (HDMI-A)" : (conn->connector_type == DRM_MODE_CONNECTOR_HDMIB)     ? " (HDMI-B)"
                                                                             : (conn->connector_type == DRM_MODE_CONNECTOR_DSI)         ? " (DSI)"
                                                                             : (conn->connector_type == DRM_MODE_CONNECTOR_DisplayPort) ? " (DisplayPort)"
                                                                                                                                        : "",
              conn->mmWidth, conn->mmHeight);

        if ((connector == NULL) && (conn->connection == DRM_MODE_CONNECTED))
        {
            connector = conn;

            // only update the physical size of the display if the values
            //   are not yet initialized / not set with a commandline option
            if ((width_mm == 0) && (height_mm == 0))
            {
                if ((conn->mmWidth == 160) && (conn->mmHeight == 90))
                {
                    // if width and height is exactly 160mm x 90mm, the values are probably bogus.
                    width_mm = 0;
                    height_mm = 0;
                }
                else if ((conn->connector_type == DRM_MODE_CONNECTOR_DSI) && (conn->mmWidth == 0) && (conn->mmHeight == 0))
                {
                    // if it's connected via DSI, and the width & height are 0,
                    //   it's probably the official 7 inch touchscreen.
                    width_mm = 155;
                    height_mm = 86;
                }
                else
                {
                    width_mm = conn->mmWidth;
                    height_mm = conn->mmHeight;
                }
            }
        }
        else
        {
            drmModeFreeConnector(conn);
        }
    }
    if (!connector)
    {
        debug("could not find a connected connector!");
        return false;
    }

    debug("Choosing DRM mode from %d available modes...\n", connector->count_modes);
    bool found_preferred = false;
    for (int i = 0, area = 0; i < connector->count_modes; i++)
    {
        drmModeModeInfo *current_mode = &connector->modes[i];

        debug("  modes[%d]: name: \"%s\", %ux%u%s, %uHz, type: %u, flags: %u\n",
              i, current_mode->name, current_mode->hdisplay, current_mode->vdisplay,
              (current_mode->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "p",
              current_mode->vrefresh, current_mode->type, current_mode->flags);

        if (found_preferred)
            continue;

        // we choose the highest resolution with the highest refresh rate, preferably non-interlaced (= progressive) here.
        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if ((current_area > area) ||
            ((current_area == area) && (current_mode->vrefresh > refresh_rate)) ||
            ((current_area == area) && (current_mode->vrefresh == refresh_rate) && ((current_mode->flags & DRM_MODE_FLAG_INTERLACE) == 0)) ||
            (current_mode->type & DRM_MODE_TYPE_PREFERRED))
        {

            drm.mode = current_mode;
            width = current_mode->hdisplay;
            height = current_mode->vdisplay;
            refresh_rate = current_mode->vrefresh;
            refresh_period_ns = 1000000000ul / refresh_rate;

            area = current_area;

            // if the preferred DRM mode is bogus, we're screwed.
            if (current_mode->type & DRM_MODE_TYPE_PREFERRED)
            {
                debug("    this mode is preferred by DRM. (DRM_MODE_TYPE_PREFERRED)");
                found_preferred = true;
            }
        }
    }

    if (!drm.mode)
    {
        debug("could not find a suitable DRM mode!");
        return false;
    }

    // calculate the pixel ratio
    if (pixel_ratio == 0.0)
    {
        if ((width_mm == 0) || (height_mm == 0))
        {
            pixel_ratio = 1.0;
        }
        else
        {
            pixel_ratio = (10.0 * width) / (width_mm * 38.0);
            if (pixel_ratio < 1.0)
                pixel_ratio = 1.0;
        }
    }

    debug("Display properties:\n  %u x %u, %uHz\n  %umm x %umm\n  pixel_ratio = %f\n", width, height,
          refresh_rate, width_mm, height_mm, pixel_ratio);

    debug("Finding DRM encoder...");
    for (int i = 0; i < resources->count_encoders; i++)
    {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder)
    {
        drm.crtc_id = encoder->crtc_id;
    }
    else
    {
        debug("could not find a suitable crtc!");
        return false;
    }

    for (int i = 0; i < resources->count_crtcs; i++)
    {
        if (resources->crtcs[i] == drm.crtc_id)
        {
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

    if (!gbm.surface)
    {
        if (gbm.modifier != DRM_FORMAT_MOD_LINEAR)
        {
            debug("GBM Surface creation modifiers requested but not supported by GBM");
            return false;
        }
        gbm.surface = gbm_surface_create(gbm.device, width, height, gbm.format,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!gbm.surface)
    {
        debug("failed to create GBM surface");
        return false;
    }

    /**********************
     * EGL INITIALIZATION *
     **********************/
    EGLint major, minor;

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE};

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SAMPLES, 0,
        EGL_NONE};

    const char *egl_exts_client, *egl_exts_dpy, *gl_exts;

    debug("Querying EGL client extensions...");
    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    egl.eglGetPlatformDisplayEXT = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    debug("Getting EGL display for GBM device...");
    if (egl.eglGetPlatformDisplayEXT)
        egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device,
                                                   NULL);
    else
        egl.display = eglGetDisplay((void *)gbm.device);

    if (!egl.display)
    {
        debug("Couldn't get EGL display");
        return false;
    }

    debug("Initializing EGL...");
    if (!eglInitialize(egl.display, &major, &minor))
    {
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
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        debug("failed to bind OpenGL ES API");
        return false;
    }

    debug("Choosing EGL config...");
    EGLint count = 0, matched = 0;
    EGLConfig *configs;
    bool _found_matching_config = false;

    if (!eglGetConfigs(egl.display, NULL, 0, &count) || count < 1)
    {
        debug("No EGL configs to choose from.");
        return false;
    }

    configs = malloc(count * sizeof(EGLConfig));
    if (!configs)
        return false;

    debug("Finding EGL configs with appropriate attributes...");
    if (!eglChooseConfig(egl.display, config_attribs, configs, count, &matched) || !matched)
    {
        debug("No EGL configs with appropriate attributes.");
        free(configs);
        return false;
    }
    debug("eglChooseConfig done");

    if (!gbm.format)
    {
        debug("!gbm.format");
        _found_matching_config = true;
    }
    else
    {
        debug("gbm.format");
        for (int i = 0; i < count; i++)
        {
            EGLint id;
            debug("checking id=%d\n", id);
            if (!eglGetConfigAttrib(egl.display, configs[i], EGL_NATIVE_VISUAL_ID, &id))
                continue;

            if (id == gbm.format)
            {
                debug("gbm.format=%d\n", id);

                egl.config = configs[i];
                _found_matching_config = true;
                break;
            }
        }
    }
    free(configs);

    if (!_found_matching_config)
    {
        debug("Could not find context with appropriate attributes and matching native visual ID.");
        return false;
    }

    debug("Creating EGL context...");
    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);
    if (egl.context == NULL)
    {
        debug("failed to create EGL context");
        return false;
    }

    debug("Creating EGL window surface...");
    egl.surface = eglCreateWindowSurface(egl.display, egl.config, (EGLNativeWindowType)gbm.surface, NULL);
    if (egl.surface == EGL_NO_SURFACE)
    {
        debug("failed to create EGL window surface");
        return false;
    }

    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context))
    {
        debug("Could not make EGL context current to get OpenGL information");
        return false;
    }

    egl.renderer = (char *)glGetString(GL_RENDERER);

    gl_exts = (char *)glGetString(GL_EXTENSIONS);
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

    drm.evctx = (drmEventContext){
        .version = 4,
        .vblank_handler = NULL,
        .page_flip_handler = pageflip_handler,
        .page_flip_handler2 = NULL,
        .sequence_handler = NULL};

    debug("Swapping buffers...");
    eglSwapBuffers(egl.display, egl.surface);

    debug("Locking front buffer...");
    drm.previous_bo = gbm_surface_lock_front_buffer(gbm.surface);

    debug("getting new framebuffer for BO...");
    struct drm_fb *fb = drm_fb_get_from_bo(drm.previous_bo);
    if (!fb)
    {
        debug("failed to get a new framebuffer BO");
        return false;
    }

    debug("Setting CRTC...");
    ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    if (ok)
    {
        debug("failed to set mode: %s\n", strerror(errno));
        return false;
    }

    debug("Clearing current context...");
    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
    {
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

static void on_platform_message(
    const FlutterPlatformMessage *message,
    void *userdata)
{
    debug("got platform message");
    plat_msg_push(&queue, message);
    eventfd_write(fdset[EVENTFD_FD_POLL].fd, 1);
    debug("platform message complete");
}

void platch_on_response_internal(const uint8_t *buffer, size_t size, void *userdata)
{
}

static void send_platform_message(
    const char *channel,
    const uint8_t *restrict message,
    size_t message_size)
{
    FlutterEngineResult result;
    FlutterPlatformMessageResponseHandle *response_handle = NULL;
    result = FlutterPlatformMessageCreateResponseHandle(engine, platch_on_response_internal, NULL, &response_handle);
    if (result != kSuccess)
    {
        error("FlutterPlatformMessageCreateResponseHandle");
        return;
    }
    result = FlutterEngineSendPlatformMessage(engine,
                                              &(const FlutterPlatformMessage){
                                                  .struct_size = sizeof(FlutterPlatformMessage),
                                                  .channel = channel,
                                                  .message = message,
                                                  .message_size = message_size,
                                                  .response_handle = response_handle});
    if (result != kSuccess)
        error("FlutterEngineSendPlatformMessage");

    FlutterPlatformMessageReleaseResponseHandle(engine, response_handle);
    debug("send_platform_message complete");
}

static bool init_application(void)
{
    // configure flutter rendering
    flutter.renderer_config.type = kOpenGL;
    flutter.renderer_config.open_gl.struct_size = sizeof(flutter.renderer_config.open_gl);
    flutter.renderer_config.open_gl.make_current = make_current;
    flutter.renderer_config.open_gl.clear_current = clear_current;
    flutter.renderer_config.open_gl.present = present;
    flutter.renderer_config.open_gl.fbo_callback = fbo_callback;
    flutter.renderer_config.open_gl.gl_proc_resolver = proc_resolver;
    flutter.renderer_config.open_gl.surface_transformation = NULL;

    // configure flutter
    flutter.args.struct_size = sizeof(FlutterProjectArgs);
    flutter.args.assets_path = flutter.asset_bundle_path;
    flutter.args.icu_data_path = flutter.icu_data_path;
    flutter.args.isolate_snapshot_data_size = 0;
    flutter.args.isolate_snapshot_data = NULL;
    flutter.args.isolate_snapshot_instructions_size = 0;
    flutter.args.isolate_snapshot_instructions = NULL;
    flutter.args.vm_snapshot_data_size = 0;
    flutter.args.vm_snapshot_data = NULL;
    flutter.args.vm_snapshot_instructions_size = 0;
    flutter.args.vm_snapshot_instructions = NULL;
    flutter.args.command_line_argc = flutter.engine_argc;
    flutter.args.command_line_argv = flutter.engine_argv;
    flutter.args.platform_message_callback = on_platform_message; // Not needed yet.
    flutter.args.vsync_callback =
        vsync_callback; // See flutter-pi fix if display driver doesn't provide vblank timestamps
    flutter.args.custom_task_runners = &(FlutterCustomTaskRunners){
        .struct_size = sizeof(FlutterCustomTaskRunners),
        .platform_task_runner = &(FlutterTaskRunnerDescription){
            .struct_size = sizeof(FlutterTaskRunnerDescription),
            .user_data = NULL,
            .runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
            .post_task_callback = &flutter_post_platform_task}};

    // spin up the engine
    FlutterEngineResult _result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &flutter.renderer_config,
                                                   &flutter.args, NULL, &engine);
    if (_result != kSuccess)
    {
        debug("Could not run the flutter engine");
        return false;
    }
    else
    {
        debug("flutter engine successfully started up.");
    }

    engine_running = true;

    // update window size
    int ok = FlutterEngineSendWindowMetricsEvent(
                 engine,
                 &(FlutterWindowMetricsEvent){
                     .struct_size = sizeof(FlutterWindowMetricsEvent), .width = width, .height = height, .pixel_ratio = pixel_ratio}) == kSuccess;

    if (!ok)
    {
        debug("Could not update Flutter application size.");
        return false;
    }

    return true;
}

static void destroy_application(void)
{
    if (engine != NULL)
    {
        if (FlutterEngineShutdown(engine) != kSuccess)
            debug("Could not shutdown the flutter engine.");

        engine = NULL;
    }
}

static bool init_io(void)
{
    FlutterPointerEvent flutterevents[16] = {0};
    size_t i_flutterevent = 0;
    int n_flutter_slots = 0;

    // add the mouse slot
    mousepointer = (struct mousepointer_mtslot){
        .id = 0,
        .flutter_slot_id = n_flutter_slots++,
        .x = 0,
        .y = 0,
        .phase = kCancel};

    flutterevents[i_flutterevent++] = (FlutterPointerEvent){
        .struct_size = sizeof(FlutterPointerEvent),
        .phase = kAdd,
        .timestamp = (size_t)(FlutterEngineGetCurrentTime() * 1000),
        .x = 0,
        .y = 0,
        .signal_kind = kFlutterPointerSignalKindNone,
        .device_kind = kFlutterPointerDeviceKindTouch,
        .device = mousepointer.flutter_slot_id,
        .buttons = 0};
    return FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) == kSuccess;
}

static void process_io_events(int fd)
{
    // Read as many the input events as possible
    ssize_t rd = read(fd, io_input_buffer, sizeof(io_input_buffer));
    if (rd < 0)
        error("read failed");
    if (rd % sizeof(struct input_event))
        error("read returned %d which is not a multiple of %d!", (int)rd, (int)sizeof(struct input_event));

    FlutterPointerEvent flutterevents[64] = {0};
    size_t i_flutterevent = 0;

    size_t event_count = rd / sizeof(struct input_event);
    for (size_t i = 0; i < event_count; i++)
    {
        if (io_input_buffer[i].type == EV_ABS)
        {
            if (io_input_buffer[i].code == ABS_X)
            {
                mousepointer.x = io_input_buffer[i].value;
            }
            else if (io_input_buffer[i].code == ABS_Y)
            {
                mousepointer.y = io_input_buffer[i].value;
            }
            else if (io_input_buffer[i].code == ABS_MT_TRACKING_ID && io_input_buffer[i].value == -1)
            {
            }

            if (mousepointer.phase == kDown)
            {
                flutterevents[i_flutterevent++] = (FlutterPointerEvent){
                    .struct_size = sizeof(FlutterPointerEvent),
                    .phase = kMove,
                    .timestamp = io_input_buffer[i].time.tv_sec * 1000000 + io_input_buffer[i].time.tv_usec,
                    .x = mousepointer.x,
                    .y = mousepointer.y,
                    .device = mousepointer.flutter_slot_id,
                    .signal_kind = kFlutterPointerSignalKindNone,
                    .device_kind = kFlutterPointerDeviceKindTouch,
                    .buttons = 0};
            }
        }
        else if (io_input_buffer[i].type == EV_KEY)
        {
            if (io_input_buffer[i].code == BTN_TOUCH)
            {
                mousepointer.phase = io_input_buffer[i].value ? kDown : kUp;
            }
            else
            {
                debug("unknown EV_KEY code=%d value=%d\r\n", io_input_buffer[i].code, io_input_buffer[i].value);
            }
        }
        else if (io_input_buffer[i].type == EV_SYN && io_input_buffer[i].code == SYN_REPORT)
        {
            // we don't want to send an event to flutter if nothing changed.
            if (mousepointer.phase == kCancel)
                continue;

            flutterevents[i_flutterevent++] = (FlutterPointerEvent){
                .struct_size = sizeof(FlutterPointerEvent),
                .phase = mousepointer.phase,
                .timestamp = io_input_buffer[i].time.tv_sec * 1000000 + io_input_buffer[i].time.tv_usec,
                .x = mousepointer.x,
                .y = mousepointer.y,
                .device = mousepointer.flutter_slot_id,
                .signal_kind = kFlutterPointerSignalKindNone,
                .device_kind = kFlutterPointerDeviceKindTouch,
                .buttons = 0};
            if (mousepointer.phase == kUp)
                mousepointer.phase = kCancel;
        }
        else
        {
            debug("unknown input_event type=%d\r\n", io_input_buffer[i].type);
        }
    }

    if (i_flutterevent == 0)
        return;

    // now, send the data to the flutter engine
    if (FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) != kSuccess)
    {
        debug("could not send pointer events to flutter engine\r\n");
    }
}

static void *io_loop(void *userdata)
{
    const char *input_path = "/dev/input/event0";
    int fd = open(input_path, O_RDONLY);
    if (errno == EACCES && getuid() != 0)
        error("You do not have access to %s.", input_path);

    while (engine_running)
    {
        // debug("io poll");
        // NOTE: this is not the same fdset as the globally named one
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
    if (ok != 0)
    {
        error("couldn't create  io thread: [%s]", strerror(ok));
        return false;
    }

    ok = pthread_setname_np(io_thread_id, "io");
    if (ok != 0)
    {
        error("couldn't set name of io thread: [%s]", strerror(ok));
        return false;
    }

    return true;
}

static void handle_from_elixir(const uint8_t *buffer, size_t length, void *cookie)
{
    uint8_t type = buffer[sizeof(uint32_t)];
    // response
    if (type == 0x1)
    {
        debug("found cookie=%d", buffer[sizeof(uint32_t) + 1]);
        debug("message length=%zu", length - 4);
        plat_msg_process(&queue, engine, buffer, length);
        return;
    }
    if (type == 0x0)
    {
        uint8_t cd1 = buffer[sizeof(uint32_t) + 1 + 1];
        uint8_t cd2 = buffer[sizeof(uint32_t) + 1 + 1 + 1];
        uint16_t channel_size = ((uint16_t)cd2 << 8) | cd1;
        debug("response channel_size=%d", channel_size);

        uint8_t md1 = buffer[sizeof(uint32_t) + 1 + 1 + sizeof(uint16_t) + channel_size];
        uint8_t md2 = buffer[sizeof(uint32_t) + 1 + 1 + sizeof(uint16_t) + channel_size + 1];
        uint16_t message_size = ((uint16_t)md2 << 8) | md1;
        debug("response message_size=%d", message_size);

        const char *channel = malloc(channel_size + 1);
        memset((void *)channel, 0x0, channel_size + 1);
        memcpy(channel, buffer + sizeof(uint32_t) + 1 + 1 + sizeof(uint16_t), channel_size);

        const char *message = malloc(message_size);
        memset((void *)message, 0x0, message_size);
        memcpy(message, buffer + sizeof(uint32_t) + 1 + 1 + sizeof(uint16_t) + channel_size + sizeof(uint16_t), message_size);

        send_platform_message(channel, message, message_size);
    }
}

void *comms_thread(void *vargp)
{
    for (;;)
    {
        for (int i = 0; i < num_pollfds; i++)
            fdset[i].revents = 0;
        int rc = poll(fdset, num_pollfds, 0);
        if (rc < 0)
        {
            // Retry if EINTR
            if (errno == EINTR)
                continue;
            error("poll failed with %d", errno);
        }

        // Erlang closed the port
        if (fdset[ERLCMD_FD_POLL].revents & POLLHUP)
            exit(2);

        // from elixir
        if (fdset[ERLCMD_FD_POLL].revents & POLLIN)
            erlcmd_process(&handler);

        // trigger from other thread about messages being ready for dispatch
        if (fdset[EVENTFD_FD_POLL].revents & (POLLIN | POLLHUP))
        {
            eventfd_t event;
            eventfd_read(fdset[EVENTFD_FD_POLL].fd, &event);
            size_t r;
            r = plat_msg_dispatch_all(&queue, &handler);
            if (r < 0)
                error("Failed to dispatch platform messages: %zu", r);
        }

        // Engine STDOUT
        if (fdset[ENGINE_STDOUT_FD_POLL].revents & POLLIN)
        {
            memset(capstdoutbuffer, 0, ERLCMD_BUF_SIZE);
            capstdoutbuffer[sizeof(uint32_t)] = 1;
            size_t nbytes = read(fdset[ENGINE_STDOUT_FD_POLL].fd, capstdoutbuffer + sizeof(uint32_t) + sizeof(uint32_t),
                                 ERLCMD_BUF_SIZE - sizeof(uint32_t) - sizeof(uint32_t));
            debug("stdout data from engine: %.*s", (int)nbytes, capstdoutbuffer + sizeof(uint32_t) + sizeof(uint32_t));
            if (nbytes < 0)
                error("Failed to read engine log buffer");
            erlcmd_send(&handler, capstdoutbuffer, nbytes + sizeof(uint32_t) + sizeof(uint32_t));
        }
        // Engine STDERR
        if (fdset[ENGINE_STDERR_FD_POLL].revents & POLLIN)
        {
            memset(capstdoutbuffer, 0, ERLCMD_BUF_SIZE);
            capstdoutbuffer[sizeof(uint32_t)] = 2;
            size_t nbytes = read(fdset[ENGINE_STDERR_FD_POLL].fd, capstdoutbuffer + sizeof(uint32_t) + sizeof(uint32_t),
                                 ERLCMD_BUF_SIZE - sizeof(uint32_t) - sizeof(uint32_t));
            debug("stderr data from engine: %.*s", (int)nbytes, capstdoutbuffer + sizeof(uint32_t) + sizeof(uint32_t));
            if (nbytes < 0)
                error("Failed to read engine log buffer");
            erlcmd_send(&handler, capstdoutbuffer, nbytes + sizeof(uint32_t) + sizeof(uint32_t));
        }
    }
}

static bool init_comms(void)
{
    fdset[EVENTFD_FD_POLL].fd = eventfd(0, 0);
    fdset[EVENTFD_FD_POLL].events = POLLIN;
    fdset[EVENTFD_FD_POLL].revents = 0;

    // setup for capturing stdout from the engine
    if (pipe2(capstdout, O_NONBLOCK) < 0)
    {
        error("pipe2");
        error("plat_msg_init");
    }
    dup2(capstdout[1], STDOUT_FILENO);

    fdset[ENGINE_STDOUT_FD_POLL].fd = capstdout[0];
    fdset[ENGINE_STDOUT_FD_POLL].events = POLLIN;
    fdset[ENGINE_STDOUT_FD_POLL].revents = 0;

    // setup for capturing stderr from the engine
    if (pipe2(capstderr, O_NONBLOCK) < 0)
    {
        error("pipe2");
        error("plat_msg_init");
    }
    dup2(capstderr[1], STDERR_FILENO);
    fdset[ENGINE_STDERR_FD_POLL].fd = capstderr[0];
    fdset[ENGINE_STDERR_FD_POLL].events = POLLIN;
    fdset[ENGINE_STDERR_FD_POLL].revents = 0;
    return true;
}

static bool run_comms_thread(void)
{
    pthread_create(&comms_thread_id, NULL, comms_thread, NULL);
    return true;
}

int main(int argc, char **argv)
{

#ifdef DEBUG
#ifdef LOG_PATH
    log_location = fopen(LOG_PATH, "w");
#endif
#endif

    if (argc < 3)
    {
        error("flutter_embedder <asset bundle path> <icu path> [other args]");
        exit(EXIT_FAILURE);
    }

    snprintf(flutter.asset_bundle_path, sizeof(flutter.asset_bundle_path), "%s", argv[1]);
    snprintf(flutter.icu_data_path, sizeof(flutter.icu_data_path), "%s", argv[2]);

    flutter.engine_argc = argc;
    flutter.engine_argv = (const char *const *)argv;
    for (int i = 0; i < flutter.engine_argc; i++)
    {
        debug("engine argv[%d]: %s", i, flutter.engine_argv[i]);
    }

    int erlcmd_writefd = dup(STDOUT_FILENO);
    int erlcmd_readfd = dup(STDIN_FILENO);
    debug("using %d %d for erlcmd", erlcmd_writefd, erlcmd_readfd);

    erlcmd_init(&handler, erlcmd_readfd, erlcmd_writefd, handle_from_elixir, NULL);
    if (plat_msg_queue_init(&queue) < 0)
    {
        error("plat_msg_init");
        exit(EXIT_FAILURE);
    }

    // Initialize the file descriptor set for polling
    memset(fdset, -1, sizeof(fdset));
    fdset[ERLCMD_FD_POLL].fd = erlcmd_readfd;
    fdset[ERLCMD_FD_POLL].events = POLLIN;
    fdset[ERLCMD_FD_POLL].revents = 0;

    debug("Initializing Comms with Elixir...");
    if (!init_comms())
    {
        error("init_comms failed");
        return EXIT_FAILURE;
    }

    // check if asset bundle path is valid
    if (!init_paths())
    {
        error("init_paths");
        return EXIT_FAILURE;
    }

    if (!init_message_loop())
    {
        error("init_message_loop");
        return EXIT_FAILURE;
    }

    // initialize display
    debug("initializing display...");
    if (!init_display())
    {
        error("init_display failed");
        return EXIT_FAILURE;
    }

    // initialize application
    debug("Initializing Application...");
    if (!init_application())
    {
        error("init_application failed");
        return EXIT_FAILURE;
    }

    debug("Initializing Input devices...");
    if (!init_io())
    {
        error("init_io failed");
        return EXIT_FAILURE;
    }

    // read events from elixir
    debug("Running Comms thread...");
    run_comms_thread();

    // read input events
    debug("Running IO thread...");
    run_io_thread();

    // run message loop
    debug("Running message loop...");
    run_message_loop();

    // exit
    pthread_join(comms_thread_id, NULL);
    plat_msg_queue_destroy(&queue);
    destroy_application();
    destroy_display();

    return EXIT_SUCCESS;
}
