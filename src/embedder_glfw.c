#include <assert.h>
#include <errno.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <dlfcn.h>

#include "embedder_gfx.h"
#include "flutter_embedder.h"
#include "debug.h"

// This value is calculated after the window is created.
static double g_pixelRatio = 1.0;
static const size_t kInitialWindowWidth = 800;
static const size_t kInitialWindowHeight = 600;
GLFWwindow* window;

void GLFWcursorPositionCallbackAtPhase(GLFWwindow *window, FlutterPointerPhase phase, double x, double y)
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

void GLFWmouseButtonCallback(GLFWwindow *window, int key, int action, int mods)
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

void GLFWKeyCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
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

bool runs_platform_tasks_on_current_thread(void *userdata)
{
    return true;
}

void on_post_flutter_task(
    FlutterTask task,
    uint64_t target_time,
    void* userdata
)
{
    FlutterEngineRunTask(glfwGetWindowUserPointer(window), &task);
    return;
}

bool gfx_make_current(void *userdata)
{
    glfwMakeContextCurrent(window);
    glewInit();
    return true;
}

bool gfx_clear_current(void *userdata)
{
    glfwMakeContextCurrent(window);
    return true;
}

bool gfx_present(void *userdata)
{
    glfwSwapBuffers(window);
    return true;
}

uint32_t gfx_fbo_callback(void *userdata)
{
    return 0;
}

/**
 * elixir embedder "callbacks"
 */

size_t gfx_init(FlutterEngine engine)
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


    glfwSetWindowUserPointer(window, engine);
    GLFWwindowSizeCallback(window, kInitialWindowWidth, kInitialWindowHeight);
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