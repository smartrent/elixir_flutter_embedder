#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "embedder_platform_message.h"
#include "embedder_gfx.h"
#include "erlcmd.h"
#include "flutter_embedder.h"
#include "debug.h"

static_assert(FLUTTER_ENGINE_VERSION == 1,
              "This Flutter Embedder was authored against the stable Flutter "
              "API at version 1. There has been a serious breakage in the "
              "API. Please read the ChangeLog and take appropriate action "
              "before updating this assertion");

FlutterEngine engine;

static plat_msg_queue_t queue;
static struct erlcmd handler;
static struct pollfd fdset[3];
static int num_pollfds = 3;
static int capstdout[2];
static unsigned char capstdoutbuffer[ERLCMD_BUF_SIZE];
static pthread_t flutter_embedder_pollfd_thread;

/** Called by erlcmd. */
static void handle_from_elixir(const uint8_t *buffer, size_t length, void *cookie)
{
    plat_msg_process(&queue, engine, buffer, length);
}

/** gfx implementations must configure this function */
void on_platform_message(
    const FlutterPlatformMessage *message,
    void *userdata
)
{
    debug("on_platform_message");
    plat_msg_push(&queue, message);
    eventfd_write(fdset[2].fd, 1);
}

/** Initializes erlcmd to be be ready for polling */
static int init_erlcmd()
{
    int writefd = dup(STDOUT_FILENO);
    int readfd = dup(STDIN_FILENO);
    erlcmd_init(&handler, readfd, writefd, handle_from_elixir, NULL);
    // Initialize the file descriptor set for polling
    memset(fdset, -1, sizeof(fdset));
    fdset[0].fd = readfd;
    fdset[0].events = POLLIN;
    fdset[0].revents = 0;
    return 0;
}

/** Initializes the engine stdout and eventfd pollfds */
static int init_pollfds()
{
    if (pipe2(capstdout, O_NONBLOCK) < 0) {
        error("pipe2");
        return -1;
    }

    // replace STDOUT with something we can poll
    // this is because the Flutter engine doesn't allow
    // for configuration of it's output.
    dup2(capstdout[1], STDOUT_FILENO);
    fdset[1].fd = capstdout[0];
    fdset[1].events = POLLIN;
    fdset[1].revents = 0;

    // EventFD for signaling that
    // Tasks have been resolved
    fdset[2].fd = eventfd(0, 0);
    fdset[2].events = POLLIN;
    fdset[2].revents = 0;
    return 0;
}

// Eventloop for for I/O with the port and the engine
void *pollfd_thread_function(void *vargp)
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
                error("Failed to dispatch platform messages: %ld", r);
        }
    }
}

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

    if (init_erlcmd() < 0) {
        error("erlcmd");
        exit(EXIT_FAILURE);
    }

    if (plat_msg_queue_init(&queue) < 0) {
        error("plat_msg_init");
        exit(EXIT_FAILURE);
    }

    if (init_pollfds() < 0) {
        error("poll");
        exit(EXIT_FAILURE);
    }

    FlutterProjectArgs args = {
        .struct_size               = sizeof(FlutterProjectArgs),
        .assets_path               = project_path,
        .icu_data_path             = icudtl_path,
        .platform_message_callback = on_platform_message,
        .vsync_callback            = gfx_vsync
    };
    FlutterRendererConfig config = {
        .type                     = kOpenGL,
        .open_gl.struct_size      = sizeof(FlutterOpenGLRendererConfig),
        .open_gl.make_current     = gfx_make_current,
        .open_gl.clear_current    = gfx_clear_current,
        .open_gl.present          = gfx_present,
        .open_gl.fbo_callback     = gfx_fbo_callback,
        .open_gl.gl_proc_resolver = proc_resolver
    };

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

    args.custom_task_runners = &custom_task_runners;

    FlutterEngineResult result = FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &config, &args, NULL, &engine);
    assert(result == kSuccess && engine != NULL);

    debug("initializing gfx");
    if (gfx_init(engine) < 0) {
        error("gfx");
        exit(EXIT_FAILURE);
    }
    result = FlutterEngineRunInitialized(engine);
    assert(result == kSuccess && engine != NULL);

    // FlutterEngineResult result = FlutterEngineRun(FLUTTER_ENGINE_VERSION, &config, &args, NULL, engine);
    debug("and here");

    if (pthread_create(&flutter_embedder_pollfd_thread,
                       NULL,
                       pollfd_thread_function,
                       NULL) < 0) {
        error("pthread");
        exit(EXIT_FAILURE);
    }

    // Enter the main loop
    gfx_loop();

    if (pthread_join(flutter_embedder_pollfd_thread, NULL) < 0) {
        error("pthread_join");
        exit(EXIT_FAILURE);
    }

    if (plat_msg_queue_destroy(&queue) < 0) {
        error("plat_msg_queue_destroy");
        exit(EXIT_FAILURE);
    }

    if (gfx_terminate() < 0) {
        error("gfx_terminate");
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}