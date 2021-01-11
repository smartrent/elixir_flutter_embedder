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
#define EGL_PLATFORM_GBM_KHR    0x31D7

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

typedef enum {
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


/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
static double pixel_ratio = 1.358234;

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

static pthread_t platform_thread_id;

static struct engine_task *tasklist = NULL;
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
    debug("make_current");
    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
        debug("make_current: could not make the context current. %d", eglGetError());
        return false;
    }

    return true;
}

static bool clear_current(void *userdata)
{
    debug("clear_current");
    if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        debug("clear_current: could not clear the current context.");
        return false;
    }

    return true;
}

static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    debug("drm_fb_destroy_callback");
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
    debug("present");
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

    return true;
}

static drmModeConnector *find_connector(drmModeRes *resources)
{
    // iterate the connectors
    for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector *connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
    if (connector->connection == DRM_MODE_CONNECTED)
        return connector;
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

static bool find_display_configuration()
{
    drmModeRes *resources = drmModeGetResources(drm.fd);
    // find a connector
    drmModeConnector *connector = find_connector(resources);
    if (!connector) {
        debug("Failed to get connector");
        return false;
    }

    // save the connector_id
    drm.connector_id = connector->connector_id;

    // save the first mode
    drm.mode = &connector->modes[0]; 
    printf("resolution: %ix%i\n", drm.mode->hdisplay, drm.mode->vdisplay);

    // find an encoder
    drmModeEncoder *encoder = find_encoder(resources, connector);
    if (!encoder) {
        debug("failed to get encoder\r\n");
        return false;
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
    return true;
}

static bool setup_opengl()
{
    debug("setup opengl");
    gbm.device = gbm_create_device(drm.fd);
    egl.display = eglGetDisplay(gbm.device);

    if (!eglInitialize(egl.display, NULL, NULL)) {
		debug("failed to initialize egl");
		return false;
	}

    // create an OpenGL context
    if(!eglBindAPI(EGL_OPENGL_API)){
        debug("failed to bind api");
        return false;
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

    // const EGLint attributes[] = {
    //     EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    //     EGL_RED_SIZE, 1,
    //     EGL_GREEN_SIZE, 1,
    //     EGL_BLUE_SIZE, 1,
    //     EGL_ALPHA_SIZE, 0,
    //     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    //     EGL_SAMPLES, 0,
    //     EGL_NONE
    // };

    const char *egl_exts_client, *egl_exts_dpy, *gl_exts;

    debug("Querying EGL client extensions...");
    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    egl.eglGetPlatformDisplayEXT = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
    debug("Getting EGL display for GBM device...");
    if (egl.eglGetPlatformDisplayEXT) egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device, NULL);
    else                              
    egl.display = eglGetDisplay((void *) gbm.device);

    if (!egl.display) {
        debug("Couldn't get EGL display");
        return false;
    }

    EGLint num_config;
    if(!eglChooseConfig(egl.display, attributes, &egl.config, 1, &num_config)) {
        debug("failed to choose config");
        return false;
    }

    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, NULL);
    if (!egl.context) {
        debug("Failed to create context");
        return false;
    }

    gbm.format = DRM_FORMAT_XRGB8888;

    // create the GBM and EGL surface
    gbm.surface = gbm_surface_create(gbm.device, drm.mode->hdisplay, drm.mode->vdisplay, gbm.format, 1);
    if (!gbm.surface) {
        debug("Failed to create surface\r\n");
        return false;
    }

    egl.surface = eglCreateWindowSurface(egl.display, egl.config, gbm.surface, NULL);
    if (!egl.surface) {
        debug("failed to create window surface");
        return false;
    }
    
   if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
        debug("Could not make EGL context current to get OpenGL information");
        return false;
    }

    debug("setup opengl here");


    return true;
}

static bool swap_buffers()
{
  int ok;
//   eglSwapBuffers(egl.display, egl.surface);
//   struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm.surface);
//   uint32_t handle = gbm_bo_get_handle(bo).u32;
//   uint32_t pitch = gbm_bo_get_stride(bo);
//   uint32_t fb;
//   ok = drmModeAddFB(drm.fd, drm.mode->hdisplay, drm.mode->vdisplay, 24, 32, pitch, handle, &fb);
//   if (ok) {
//       debug("drmModeAddFB %d", ok);
//       return false;
//   }
//   ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb, 0, 0, &drm.connector_id, 1, drm.mode);
//   if (ok) {
//       debug("drmModeSetCrtc %d", ok);
//       return false;
//   }
    debug("Swapping buffers...");
    eglSwapBuffers(egl.display, egl.surface);

    debug("Locking front buffer...");
    drm.previous_bo = gbm_surface_lock_front_buffer(gbm.surface);
    uint32_t handle = gbm_bo_get_handle(drm.previous_bo).u32;
    uint32_t pitch = gbm_bo_get_stride(drm.previous_bo);
    uint32_t fb;
    ok = drmModeAddFB(drm.fd, drm.mode->hdisplay, drm.mode->vdisplay, 24, 32, pitch, handle, &fb);
    if (ok) {
        debug("drmModeAddFB failed %d", ok);
        return false;
    }
    ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb, 0, 0, &drm.connector_id, 1, drm.mode);
    if (ok) {
        debug("drmModeSetCrtc failed %d", ok);
        return false;
    }

    // debug("getting new framebuffer for BO...");
    // struct drm_fb *fb = drm_fb_get_from_bo(drm.previous_bo);
    // if (!fb) {
    //     debug("failed to get a new framebuffer BO");
    //     return false;
    // }

    // debug("Setting CRTC...");
    // ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    // if (ok) {
    //     debug("failed to set mode: %s %d\n", strerror(errno), glGetError());
    //     return false;
    // }

    debug("Clearing current context...");
    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        debug("Could not clear EGL context");
        return false;
    }

    return true;
}

static bool init_display()
{
    drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    debug("opened card\r\n");
    find_display_configuration();
    setup_opengl();
    swap_buffers();
    return true;
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
        if (task->type == kFlutterTask) {
            if (FlutterEngineRunTask(engine, &task->task) != kSuccess) {
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
    void      *address;

    // if we do, and the symbol to resolve is glGetString, we return our hacked_glGetString.
    if (strcmp(name, "glGetString") == 0)
        return hacked_glGetString;

    if ((address = dlsym(RTLD_DEFAULT, name)) || (address = eglGetProcAddress(name)))
        return address;

    debug("proc_resolver: could not resolve symbol \"%s\"\n", name);

    return NULL;
}

static bool init_application(void)
{
    // configure flutter rendering
    flutter.renderer_config.type = kOpenGL;
    flutter.renderer_config.open_gl.struct_size     = sizeof(flutter.renderer_config.open_gl);
    flutter.renderer_config.open_gl.make_current    = make_current;
    flutter.renderer_config.open_gl.clear_current   = clear_current;
    flutter.renderer_config.open_gl.present         = present;
    flutter.renderer_config.open_gl.fbo_callback    = fbo_callback;
    flutter.renderer_config.open_gl.gl_proc_resolver = proc_resolver;


    // configure flutter
    flutter.args.struct_size                = sizeof(FlutterProjectArgs);
    flutter.args.assets_path                = flutter.asset_bundle_path;
    flutter.args.icu_data_path              = flutter.icu_data_path;
    flutter.args.isolate_snapshot_data_size = 0;
    flutter.args.isolate_snapshot_instructions_size = 0;
    flutter.args.vm_snapshot_data_size      = 0;
    flutter.args.vm_snapshot_instructions_size = 0;
    flutter.args.command_line_argc          = flutter.engine_argc;
    flutter.args.command_line_argv          = flutter.engine_argv;
    flutter.args.custom_task_runners        = &(FlutterCustomTaskRunners) {
        .struct_size = sizeof(FlutterCustomTaskRunners),
        .platform_task_runner = &(FlutterTaskRunnerDescription) {
            .struct_size = sizeof(FlutterTaskRunnerDescription),
            .user_data = NULL,
            .runs_task_on_current_thread_callback = &runs_platform_tasks_on_current_thread,
            .post_task_callback = &flutter_post_platform_task
        }
    };

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
    int ok = FlutterEngineSendWindowMetricsEvent(
                 engine,
    &(FlutterWindowMetricsEvent) {
        .struct_size = sizeof(FlutterWindowMetricsEvent), 
        .width = drm.mode->hdisplay, 
        .height = drm.mode->vdisplay, 
        .pixel_ratio = pixel_ratio
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
    return FlutterEngineSendPointerEvent(engine, flutterevents, i_flutterevent) == kSuccess;
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

// cmd("/root/flutter_embedder /srv/erlang/lib/nerves_example-0.1.0/priv/flutter_assets /srv/erlang/lib/flutter_embedder-0.1.0/priv/icudtl.dat")
// cmd("/srv/erlang/lib/flutter_embedder-0.1.0/priv/flutter_embedder /srv/erlang/lib/nerves_example-0.1.0/priv/flutter_assets /srv/erlang/lib/flutter_embedder-0.1.0/priv/icudtl.dat")
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

    if (argc < 3) {
        error("flutter_embedder <asset bundle path> <icu path> [other args]");
        exit(EXIT_FAILURE);
    }

    snprintf(flutter.asset_bundle_path, sizeof(flutter.asset_bundle_path), "%s", argv[1]);
    snprintf(flutter.icu_data_path, sizeof(flutter.icu_data_path), "%s", argv[2]);

    argv[2] = argv[0];
    flutter.engine_argc = argc - 2;
    flutter.engine_argv = (const char *const *) & (argv[2]);

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
        debug("failed to initialize display");
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

    return EXIT_SUCCESS;
}
