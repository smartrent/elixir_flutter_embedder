#include <assert.h>
#include <errno.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "embedder_gfx.h"
#include "flutter_embedder.h"

// This value is calculated after the window is created.
static double g_pixelRatio = 1.0;
static const size_t kInitialWindowWidth = 800;
static const size_t kInitialWindowHeight = 600;
GLFWwindow *window;

// There is probably a better way to do this.
extern FlutterEngine engine;

void GLFWcursorPositionCallbackAtPhase(GLFWwindow *window,
                                       FlutterPointerPhase phase,
                                       double x,
                                       double y)
{
    FlutterPointerEvent event = {};
    event.struct_size = sizeof(event);
    event.phase = phase;
    event.x = x * g_pixelRatio;
    event.y = y * g_pixelRatio;
    event.timestamp = FlutterEngineGetCurrentTime();
    FlutterEngineSendPointerEvent(glfwGetWindowUserPointer(window), &event, 1);
}

void GLFWcursorPositionCallback(GLFWwindow *window, double x, double y)
{
    GLFWcursorPositionCallbackAtPhase(window, kMove, x, y);
}

void GLFWmouseButtonCallback(GLFWwindow *window,
                             int key,
                             int action,
                             int mods)
{
    if (key == GLFW_MOUSE_BUTTON_1 && action == GLFW_PRESS) {
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        GLFWcursorPositionCallbackAtPhase(window, kDown, x, y);
        glfwSetCursorPosCallback(window, GLFWcursorPositionCallback);
    }

    if (key == GLFW_MOUSE_BUTTON_1 && action == GLFW_RELEASE) {
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        GLFWcursorPositionCallbackAtPhase(window, kUp, x, y);
        glfwSetCursorPosCallback(window, NULL);
    }
}

static void GLFWKeyCallback(GLFWwindow *window,
                            int key,
                            int scancode,
                            int action,
                            int mods)
{
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void GLFWwindowSizeCallback(GLFWwindow *window, int width, int height)
{
    FlutterWindowMetricsEvent event = {};
    event.struct_size = sizeof(event);
    event.width = width * g_pixelRatio;
    event.height = height * g_pixelRatio;
    event.pixel_ratio = g_pixelRatio;
    FlutterEngineSendWindowMetricsEvent(glfwGetWindowUserPointer(window), &event);
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

static bool make_current(void *userdata)
{
    glfwMakeContextCurrent((GLFWwindow *)userdata);
    glewInit();
    return true;
}

static bool clear_current(void *userdata)
{
    glfwMakeContextCurrent((GLFWwindow *)userdata);
    return true;
}

static bool present(void *userdata)
{
    glfwSwapBuffers((GLFWwindow *)userdata);
    return true;
}

static uint32_t fbo_callback(void *userdata)
{
    return 0;
}

static size_t run_flutter(GLFWwindow *window, FlutterProjectArgs *args)
{
    FlutterRendererConfig config = {};
    config.type = kOpenGL;
    config.open_gl.struct_size = sizeof(config.open_gl);
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

    args->custom_task_runners = &custom_task_runners;
    FlutterEngineResult result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &config, args, window, &engine);
    assert(result == kSuccess && engine != NULL);

    glfwSetWindowUserPointer(window, engine);
    GLFWwindowSizeCallback(window, kInitialWindowWidth, kInitialWindowHeight);
    return 0;
}

/**
 * elixir embedder "callbacks"
 */

size_t gfx_init(FlutterProjectArgs *args)
{
    int result;
    result = glfwInit();
    if (result != GLFW_TRUE)
        return result;

    window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight, "Flutter", NULL, NULL);
    if (!window)
        return -1;

    int framebuffer_width, framebuffer_height;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    g_pixelRatio = framebuffer_width / kInitialWindowWidth;

    result = run_flutter(window, args);
    if (result < 0)
        return result;

    glfwSetKeyCallback(window, GLFWKeyCallback);
    glfwSetWindowSizeCallback(window, GLFWwindowSizeCallback);
    glfwSetMouseButtonCallback(window, GLFWmouseButtonCallback);
    return 0;
}

size_t gfx_terminate()
{
    glfwDestroyWindow(window);
    glfwTerminate();
}

void gfx_loop()
{
    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
    }
}