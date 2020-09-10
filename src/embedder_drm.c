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
#include <glob.h>
#include <poll.h>

// Graphics headers
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "erlcmd.h"
#include "flutter_embedder.h"

#define DEBUG

#ifdef DEBUG
#define log_location stderr
#define debug(...) do { fprintf(log_location, __VA_ARGS__); fprintf(log_location, "\r\n"); fflush(log_location); } while(0)
#define error(...) do { debug(__VA_ARGS__); } while (0)
#else
#define debug(...)
#define error(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

// This value is calculated after the window is created.
static double g_pixelRatio = 1.0;
static const size_t kInitialWindowWidth = 800;
static const size_t kInitialWindowHeight = 600;

#define NUM_POLLFD_ENTRIES 32
static struct erlcmd handler;
static struct pollfd fdset[NUM_POLLFD_ENTRIES];
static int num_pollfds = 0;

struct {
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

struct {
    struct gbm_device  *device;
    struct gbm_surface *surface;
    uint32_t            format;
    uint64_t            modifier;
} gbm = {0};

struct {
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

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

/// width & height of the display in pixels
uint32_t width, height;

/// physical width & height of the display in millimeters
/// the physical size can only be queried for HDMI displays (and even then, most displays will
///   probably return bogus values like 160mm x 90mm).
/// for DSI displays, the physical size of the official 7-inch display will be set in init_display.
/// init_display will only update width_mm and height_mm if they are set to zero, allowing you
///   to hardcode values for you individual display.
uint32_t width_mm = 0, height_mm = 0;
uint32_t refresh_rate;

/// The pixel ratio used by flutter.
/// This is computed inside init_display using width_mm and height_mm.
/// flutter only accepts pixel ratios >= 1.0
/// init_display will only update this value if it is equal to zero,
///   allowing you to hardcode values.
double pixel_ratio = 0.0;

enum device_orientation {
    kPortraitUp, kLandscapeLeft, kPortraitDown, kLandscapeRight
};

/// The current device orientation.
/// The initial device orientation is based on the width & height data from drm.
enum device_orientation orientation;

/// The angle between the initial device orientation and the current device orientation in degrees.
/// (applied as a rotation to the flutter window in transformation_callback, and also
/// is used to determine if width/height should be swapped when sending a WindowMetrics event to flutter)
int rotation = 0;

void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = data;

    if (fb->fb_id)
        drmModeRmFB(drm.fd, fb->fb_id);

    free(fb);
}

struct drm_fb *drm_fb_get_from_bo(struct gbm_bo *bo)
{
    uint32_t width, height, format, strides[4] = {0}, handles[4] = {0}, offsets[4] = {0}, flags = 0;
    int ok = -1;

    // if the buffer object already has some userdata associated with it,
    //   it's the framebuffer we allocated.
    struct drm_fb *fb = gbm_bo_get_user_data(bo);
    if (fb) return fb;

    // if there's no framebuffer for the bo, we need to create one.
    fb = calloc(1, sizeof(struct drm_fb));
    fb->bo = bo;

    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    format = gbm_bo_get_format(bo);

    uint64_t modifiers[4] = {0};
    modifiers[0] = gbm_bo_get_modifier(bo);
    const int num_planes = gbm_bo_get_plane_count(bo);

    for (int i = 0; i < num_planes; i++) {
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        handles[i] = gbm_bo_get_handle(bo).u32;
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifiers[0];
    }

    if (modifiers[0]) {
        flags = DRM_MODE_FB_MODIFIERS;
    }

    ok = drmModeAddFB2WithModifiers(drm.fd, width, height, format, handles, strides, offsets, modifiers,
                                    &fb->fb_id, flags);

    if (ok) {
        if (flags)
            debug("drm_fb_get_from_bo: modifiers failed!\n");

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

bool init_display(void)
{
    /**********************
     * DRM INITIALIZATION *
     **********************/

    drmModeRes *resources = NULL;
    drmModeConnector *connector;
    drmModeEncoder *encoder = NULL;
    int i, ok, area;

    if (!drm.has_device) {
        debug("Finding a suitable DRM device, since none is given...\n");
        drmDevicePtr devices[64] = { NULL };
        int num_devices, fd = -1;

        num_devices = drmGetDevices2(0, devices, sizeof(devices) / sizeof(drmDevicePtr));
        if (num_devices < 0) {
            debug("could not query drm device list: %s\n", strerror(-num_devices));
            return false;
        }

        debug("looking for a suitable DRM device from %d available DRM devices...\n", num_devices);
        for (i = 0; i < num_devices; i++) {
            drmDevicePtr device = devices[i];

            debug("  devices[%d]: \n", i);

            debug("    available nodes: ");
            if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) debug("DRM_NODE_PRIMARY, ");
            if (device->available_nodes & (1 << DRM_NODE_CONTROL)) debug("DRM_NODE_CONTROL, ");
            if (device->available_nodes & (1 << DRM_NODE_RENDER))  debug("DRM_NODE_RENDER");
            debug("\n");

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
                if ((errno = EOPNOTSUPP) || (errno = EINVAL)) debug("doesn't look like a modeset device.\n");
                else                                          debug("%s\n", strerror(errno));
                close(fd);
                continue;
            }

            // we found our DRM device.
            debug("    flutter-pi chose \"%s\" as its DRM device.\n", device->nodes[DRM_NODE_PRIMARY]);
            drm.fd = fd;
            drm.has_device = true;
            snprintf(drm.device, sizeof(drm.device) - 1, "%s", device->nodes[DRM_NODE_PRIMARY]);
        }

        if (!drm.has_device) {
            debug("flutter-pi couldn't find a usable DRM device.\n"
                  "Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
                  "If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
            return false;
        }
    }

    if (drm.fd <= 0) {
        debug("Opening DRM device...\n");
        drm.fd = open(drm.device, O_RDWR);
        if (drm.fd < 0) {
            debug("Could not open DRM device\n");
            return false;
        }
    }

    if (!resources) {
        debug("Getting DRM resources...\n");
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
    for (i = 0; i < resources->count_connectors; i++) {
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
        debug("could not find a connected connector!\n");
        return false;
    }

    debug("Choosing DRM mode from %d available modes...\n", connector->count_modes);
    bool found_preferred = false;
    for (i = 0, area = 0; i < connector->count_modes; i++) {
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
                debug("    this mode is preferred by DRM. (DRM_MODE_TYPE_PREFERRED)\n");
                found_preferred = true;
            }
        }
    }

    if (!drm.mode) {
        debug("could not find a suitable DRM mode!\n");
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

    debug("Finding DRM encoder...\n");
    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm.crtc_id = encoder->crtc_id;
    } else {
        debug("could not find a suitable crtc!\n");
        return false;
    }

    for (i = 0; i < resources->count_crtcs; i++) {
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
    debug("Creating GBM device\n");
    gbm.device = gbm_create_device(drm.fd);
    gbm.format = DRM_FORMAT_RGB565;
    gbm.surface = NULL;
    gbm.modifier = DRM_FORMAT_MOD_LINEAR;

    gbm.surface = gbm_surface_create_with_modifiers(gbm.device, width, height, gbm.format, &gbm.modifier, 1);

    if (!gbm.surface) {
        if (gbm.modifier != DRM_FORMAT_MOD_LINEAR) {
            debug("GBM Surface creation modifiers requested but not supported by GBM\n");
            return false;
        }
        gbm.surface = gbm_surface_create(gbm.device, width, height, gbm.format,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }

    if (!gbm.surface) {
        debug("failed to create GBM surface\n");
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

    debug("Querying EGL client extensions...\n");
    egl_exts_client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    egl.eglGetPlatformDisplayEXT = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
    debug("Getting EGL display for GBM device...\n");
    if (egl.eglGetPlatformDisplayEXT) egl.display = egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm.device,
                                                                                     NULL);
    else                              egl.display = eglGetDisplay((void *) gbm.device);

    if (!egl.display) {
        debug("Couldn't get EGL display\n");
        return false;
    }


    debug("Initializing EGL...\n");
    if (!eglInitialize(egl.display, &major, &minor)) {
        debug("failed to initialize EGL\n");
        return false;
    }

    debug("Querying EGL display extensions...\n");
    egl_exts_dpy = eglQueryString(egl.display, EGL_EXTENSIONS);
    egl.modifiers_supported = strstr(egl_exts_dpy, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;


    debug("Using display %p with EGL version %d.%d\n", egl.display, major, minor);
    debug("===================================\n");
    debug("EGL information:\n");
    debug("  version: %s\n", eglQueryString(egl.display, EGL_VERSION));
    debug("  vendor: \"%s\"\n", eglQueryString(egl.display, EGL_VENDOR));
    debug("  client extensions: \"%s\"\n", egl_exts_client);
    debug("  display extensions: \"%s\"\n", egl_exts_dpy);
    debug("===================================\n");


    debug("Binding OpenGL ES API...\n");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        debug("failed to bind OpenGL ES API\n");
        return false;
    }


    debug("Choosing EGL config...\n");
    EGLint count = 0, matched = 0;
    EGLConfig *configs;
    bool _found_matching_config = false;

    if (!eglGetConfigs(egl.display, NULL, 0, &count) || count < 1) {
        debug("No EGL configs to choose from.\n");
        return false;
    }

    configs = malloc(count * sizeof(EGLConfig));
    if (!configs) return false;

    debug("Finding EGL configs with appropriate attributes...\n");
    if (!eglChooseConfig(egl.display, config_attribs, configs, count, &matched) || !matched) {
        debug("No EGL configs with appropriate attributes.\n");
        free(configs);
        return false;
    }
    debug("eglChooseConfig done\n");

    if (!gbm.format) {
        debug("!gbm.format\n");
        _found_matching_config = true;
    } else {
        debug("gbm.format\n");
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
        debug("Could not find context with appropriate attributes and matching native visual ID.\n");
        return false;
    }


    debug("Creating EGL context...\n");
    egl.context = eglCreateContext(egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);
    if (egl.context == NULL) {
        debug("failed to create EGL context\n");
        return false;
    }


    debug("Creating EGL window surface...\n");
    egl.surface = eglCreateWindowSurface(egl.display, egl.config, (EGLNativeWindowType) gbm.surface, NULL);
    if (egl.surface == EGL_NO_SURFACE) {
        debug("failed to create EGL window surface\n");
        return false;
    }

    if (!eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) {
        debug("Could not make EGL context current to get OpenGL information\n");
        return false;
    }

    egl.renderer = (char *) glGetString(GL_RENDERER);

    gl_exts = (char *) glGetString(GL_EXTENSIONS);
    debug("===================================\n");
    debug("OpenGL ES information:\n");
    debug("  version: \"%s\"\n", glGetString(GL_VERSION));
    debug("  shading language version: \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    debug("  vendor: \"%s\"\n", glGetString(GL_VENDOR));
    debug("  renderer: \"%s\"\n", egl.renderer);
    debug("  extensions: \"%s\"\n", gl_exts);
    debug("===================================\n");

    // it seems that after some Raspbian update, regular users are sometimes no longer allowed
    //   to use the direct-rendering infrastructure; i.e. the open the devices inside /dev/dri/
    //   as read-write. flutter-pi must be run as root then.
    // sometimes it works fine without root, sometimes it doesn't.
    if (strncmp(egl.renderer, "llvmpipe", sizeof("llvmpipe") - 1) == 0)
        debug("WARNING: Detected llvmpipe (ie. software rendering) as the OpenGL ES renderer.\n"
              "         Check that flutter-pi has permission to use the 3D graphics hardware,\n"
              "         or try running it as root.\n"
              "         This warning will probably result in a \"failed to set mode\" error\n"
              "         later on in the initialization.\n");

    drm.evctx = (drmEventContext) {
        .version = 4,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = NULL,
        .sequence_handler = NULL
    };

    debug("Swapping buffers...\n");
    eglSwapBuffers(egl.display, egl.surface);

    debug("Locking front buffer...\n");
    drm.previous_bo = gbm_surface_lock_front_buffer(gbm.surface);

    debug("getting new framebuffer for BO...\n");
    struct drm_fb *fb = drm_fb_get_from_bo(drm.previous_bo);
    if (!fb) {
        debug("failed to get a new framebuffer BO\n");
        return false;
    }

    debug("Setting CRTC...\n");
    ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    if (ok) {
        debug("failed to set mode: %s\n", strerror(errno));
        return false;
    }

    debug("Clearing current context...\n");
    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        debug("Could not clear EGL context\n");
        return false;
    }

    drm.disable_vsync = true;
    debug("finished display setup!\n");

    return true;
}

void destroy_display(void)
{
    debug("Deinitializing display not yet implemented\n");
}

FlutterEngine engine;

bool make_current(void *userdata)
{
    debug("make_current");
    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) != EGL_TRUE) {
        error("make_current: could not make the context current.\n");
        return false;
    }

    debug("end make_current");
    return true;
}

bool clear_current(void *userdata)
{
    debug("clear_current");
    if (eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        error("clear_current: could not clear the current context.\n");
        return false;
    }
    debug("end clear_current");
    return true;
}

bool present(void *userdata)
{
    debug("present");
    fd_set fds;
    struct gbm_bo *next_bo;
    struct drm_fb *fb;
    int ok;

    FlutterEngineTraceEventDurationBegin("present");

    eglSwapBuffers(egl.display, egl.surface);
    next_bo = gbm_surface_lock_front_buffer(gbm.surface);
    fb = drm_fb_get_from_bo(next_bo);

    // workaround for #38
    if (!drm.disable_vsync) {
        ok = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, drm.previous_bo);
        if (ok) {
            error("failed to queue page flip");
            return false;
        }
    } else {
        ok = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
        if (ok == -1) {
            error("failed swap buffers\n");
            return false;
        }
    }

    gbm_surface_release_buffer(gbm.surface, drm.previous_bo);
    drm.previous_bo = (struct gbm_bo *) next_bo;

    FlutterEngineTraceEventDurationEnd("present");
    debug("end present");
    return true;
}

uint32_t fbo_callback(void *userdata)
{
    debug("fbo");
    return 0;
}

static bool runs_platform_tasks_on_current_thread(void *userdata)
{
    return true;
}

static void on_post_flutter_task(
    FlutterTask task,
    uint64_t target_time,
    void *userdata
)
{
    FlutterEngineRunTask(engine, &task);
    return;
}

bool RunFlutter(const char *project_path,
                const char *icudtl_path)
{
    FlutterRendererConfig config = {};
    config.type = kOpenGL;
    config.open_gl.struct_size      = sizeof(config.open_gl);
    config.open_gl.make_current = make_current;
    config.open_gl.clear_current = clear_current;
    config.open_gl.present = present;
    config.open_gl.fbo_callback = fbo_callback;

    FlutterTaskRunnerDescription custom_task_runner_description = {
        .struct_size = sizeof(FlutterTaskRunnerDescription),
        .user_data = NULL,
        .runs_task_on_current_thread_callback = runs_platform_tasks_on_current_thread,
        .post_task_callback = on_post_flutter_task
    };

    FlutterCustomTaskRunners custom_task_runners = {
        .struct_size = sizeof(FlutterCustomTaskRunners),
        .platform_task_runner = &custom_task_runner_description
    };

    FlutterProjectArgs args = {
        .struct_size = sizeof(FlutterProjectArgs),
        .assets_path = project_path,
        .icu_data_path = icudtl_path,
        // .platform_message_callback = on_platform_message,
        .custom_task_runners = &custom_task_runners
    };

    FlutterEngineResult result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &config, &args, NULL, &engine);
    assert(result == kSuccess && engine != NULL);
    debug("width=%lu height=%lu pixel_ratio=%lf", width, height, pixel_ratio);
    result = FlutterEngineSendWindowMetricsEvent(
                 engine,
    &(FlutterWindowMetricsEvent) {
        .struct_size = sizeof(FlutterWindowMetricsEvent), .width = width, .height = height, .pixel_ratio = pixel_ratio
    }
             );
    assert(result == kSuccess);
    return true;
}

bool isCallerDown()
{
    struct pollfd ufd;
    memset(&ufd, 0, sizeof ufd);
    ufd.fd     = ERLCMD_READ_FD;
    ufd.events = POLLIN;
    if (poll(&ufd, 1, 0) < 0)
        return true;
    return ufd.revents & POLLHUP;
}

int main(int argc, const char *argv[])
{
    if (argc != 3) {
        exit(EXIT_FAILURE);
    }

    const char *project_path = argv[1];
    const char *icudtl_path = argv[2];

    init_display();

    bool runResult = RunFlutter(project_path, icudtl_path);
    assert(runResult);

    //main loop
    while (!isCallerDown()) {

    }


    exit(EXIT_SUCCESS);
}
