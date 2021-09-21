#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "erlcmd.h"
#include "flutter_embedder.h"
#include "embedder_platform_message.h"

#define DEBUG

#ifdef DEBUG
#define LOG_PATH "log.txt"
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
FlutterEngine engine;

static plat_msg_queue_t queue;
static struct erlcmd handler;
static struct pollfd fdset[3];
static int num_pollfds = 3;
static int capstdout[2];
static char stdmethodcallbuffer[ERLCMD_BUF_SIZE];
static char capstdoutbuffer[ERLCMD_BUF_SIZE];

static_assert(FLUTTER_ENGINE_VERSION == 1,
              "This Flutter Embedder was authored against the stable Flutter "
              "API at version 1. There has been a serious breakage in the "
              "API. Please read the ChangeLog and take appropriate action "
              "before updating this assertion");

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

size_t erlcmd_uid = 1;

static void on_platform_message(
    const FlutterPlatformMessage *message,
    void *userdata
)
{
    plat_msg_push(&queue, message);
    eventfd_write(fdset[2].fd, 1);
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

bool RunFlutter(GLFWwindow *window,
                const char *project_path,
                const char *icudtl_path)
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

    FlutterProjectArgs args = {
        .struct_size = sizeof(FlutterProjectArgs),
        .assets_path = project_path,
        .icu_data_path = icudtl_path,
        .platform_message_callback = on_platform_message,
        .custom_task_runners = &custom_task_runners
    };
    FlutterEngineResult result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &config, &args, window, &engine);
    assert(result == kSuccess && engine != NULL);

    glfwSetWindowUserPointer(window, engine);
    GLFWwindowSizeCallback(window, kInitialWindowWidth, kInitialWindowHeight);

    return true;
}

static void handle_from_elixir(const uint8_t *buffer, size_t length, void *cookie)
{
    plat_msg_process(&queue, engine, buffer, length);
}

void *myThreadFun(void *vargp)
{
    for (;;) {
        for (int i = 0; i < num_pollfds; i++)
            fdset[i].revents = 0;
        int rc = poll(fdset, num_pollfds, 0);
        if (rc < 0) {
            // Retry if EINTR
            if (errno == EINTR)
                continue;
            error("poll failed with %d", errno);
        }

        // Erlang closed the port
        if (fdset[0].revents & POLLHUP)
            exit(2);

        // from elixir
        if (fdset[0].revents & POLLIN)
            erlcmd_process(&handler);

        // Engine STDOUT
        if (fdset[1].revents & POLLIN) {
            memset(capstdoutbuffer, 0, ERLCMD_BUF_SIZE);
            capstdoutbuffer[sizeof(uint32_t)] = 1;
            size_t nbytes = read(fdset[1].fd, capstdoutbuffer + sizeof(uint32_t) + sizeof(uint32_t),
                                 ERLCMD_BUF_SIZE - sizeof(uint32_t) - sizeof(uint32_t));
            if (nbytes < 0)
                error("Failed to read engine log buffer");
            erlcmd_send(&handler, capstdoutbuffer, nbytes);
        }

        if (fdset[2].revents & (POLLIN | POLLHUP)) {
            eventfd_t event;
            eventfd_read(fdset[1].fd, &event);
            size_t r;
            r = plat_msg_dispatch_all(&queue, &handler);
            if (r < 0)
                error("Failed to dispatch platform messages: %d", r);
        }
    }
}

#include <unistd.h>

int main(int argc, const char *argv[])
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

    const char *project_path = argv[1];
    const char *icudtl_path = argv[2];
#ifdef DEBUG
    sleep(5);
#endif

    int writefd = dup(STDOUT_FILENO);
    int readfd = dup(STDIN_FILENO);
    debug("using %d for erlcmd", writefd);
    erlcmd_init(&handler, readfd, writefd, handle_from_elixir, NULL);

    if (plat_msg_queue_init(&queue) < 0) {
        error("plat_msg_init");
        exit(EXIT_FAILURE);
    }

    // Initialize the file descriptor set for polling
    memset(fdset, -1, sizeof(fdset));
    fdset[0].fd = readfd;
    fdset[0].events = POLLIN;
    fdset[0].revents = 0;

    if (pipe2(capstdout, O_NONBLOCK) < 0) {
        error("pipe2");
        error("plat_msg_init");
    }

    dup2(capstdout[1], STDOUT_FILENO);
    fdset[1].fd = capstdout[0];
    fdset[1].events = POLLIN;
    fdset[1].revents = 0;

    fdset[2].fd = eventfd(0, 0);
    fdset[2].events = POLLIN;
    fdset[2].revents = 0;

    int result = glfwInit();
    assert(result == GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight, "Flutter", NULL, NULL);
    assert(window != NULL);

    int framebuffer_width, framebuffer_height;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    g_pixelRatio = framebuffer_width / kInitialWindowWidth;

    bool runResult = RunFlutter(window, project_path, icudtl_path);
    assert(runResult);

    glfwSetKeyCallback(window, GLFWKeyCallback);
    glfwSetWindowSizeCallback(window, GLFWwindowSizeCallback);
    glfwSetMouseButtonCallback(window, GLFWmouseButtonCallback);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, myThreadFun, NULL);

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.1);
    }

    pthread_join(thread_id, NULL);
    plat_msg_queue_destroy(&queue);
    glfwDestroyWindow(window);
    glfwTerminate();

    exit(EXIT_SUCCESS);
}
