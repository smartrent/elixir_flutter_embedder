
#include <assert.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "erlcmd.h"

#include "flutter_embedder.h"
#include "platformchannel.h"

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
FlutterEngine engine;

#define NUM_POLLFD_ENTRIES 32
static struct erlcmd handler;
static struct pollfd fdset[NUM_POLLFD_ENTRIES];
static int num_pollfds = 0;

static_assert(FLUTTER_ENGINE_VERSION == 1,
              "This Flutter Embedder was authored against the stable Flutter "
              "API at version 1. There has been a serious breakage in the "
              "API. Please read the ChangeLog and take appropriate action "
              "before updating this assertion");

void GLFWcursorPositionCallbackAtPhase(GLFWwindow* window,
                                       FlutterPointerPhase phase,
                                       double x,
                                       double y) {
  FlutterPointerEvent event = {};
  event.struct_size = sizeof(event);
  event.phase = phase;
  event.x = x * g_pixelRatio;
  event.y = y * g_pixelRatio;
  event.timestamp = FlutterEngineGetCurrentTime();
  FlutterEngineSendPointerEvent(glfwGetWindowUserPointer(window), &event, 1);
}

void GLFWcursorPositionCallback(GLFWwindow* window, double x, double y) {
  GLFWcursorPositionCallbackAtPhase(window, kMove, x, y);
}

void GLFWmouseButtonCallback(GLFWwindow* window,
                             int key,
                             int action,
                             int mods) {
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

static void GLFWKeyCallback(GLFWwindow* window,
                            int key,
                            int scancode,
                            int action,
                            int mods) {
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  }
}

void GLFWwindowSizeCallback(GLFWwindow* window, int width, int height) {
  FlutterWindowMetricsEvent event = {};
  event.struct_size = sizeof(event);
  event.width = width * g_pixelRatio;
  event.height = height * g_pixelRatio;
  event.pixel_ratio = g_pixelRatio;
  FlutterEngineSendWindowMetricsEvent(glfwGetWindowUserPointer(window), &event);
}

typedef struct erlcmd_platform_message {
  const FlutterPlatformMessageResponseHandle* response_handle;
  uint8_t erlcmd_handle;
  uint16_t channel_length;
  const char* channel;
  uint16_t message_length;
  const uint8_t* message;
  bool dispatched;
  struct erlcmd_platform_message* next;
} erlcmd_platform_message_t;

erlcmd_platform_message_t* head = NULL;
pthread_mutex_t lock;

static void on_platform_message(
	const FlutterPlatformMessage* message,
	void* userdata
) {

  pthread_mutex_lock(&lock);
  uint16_t channel_length = strlen(message->channel);
  if(head) {
    // debug("adding node");
    erlcmd_platform_message_t* current = head;
    while (current->next != NULL) {
      current = current->next;
    }

    current->next = malloc(sizeof(erlcmd_platform_message_t));
    if(!current->next)
      exit(EXIT_FAILURE);

    current->next->next = NULL;
    current->next->dispatched = false;
    current->next->erlcmd_handle = current->erlcmd_handle + 1;
    current->next->response_handle = message->response_handle;
    current->next->channel = message->channel;
    current->next->channel_length = channel_length;
    current->next->message = message->message;
    current->next->message_length = message->message_size;

  } else {
    // debug("Creating head node");
    head = malloc(sizeof(erlcmd_platform_message_t));
    if(!head)
      exit(EXIT_FAILURE);
    head->next = NULL;
    head->dispatched = false;
    head->erlcmd_handle = 1;
    head->response_handle = message->response_handle;
    head->channel = message->channel;
    head->channel_length = channel_length;
    head->message = message->message;
    head->message_length = message->message_size;
  }

  pthread_mutex_unlock(&lock);
  eventfd_write(fdset[1].fd, 1);

  if(strcmp(message->channel, "platform/idk") == 0) {
  //   uint16_t channel_length = strlen(message->channel);
  //   size_t buffer_size =2 + 2 + channel_length + message->message_size;
  //   debug("platform message channel(%lu): %s content(%lu): %s", channel_length, message->channel, message->message_size, message->message);
  //   debug("erlcmd buffer size: %lu", buffer_size);
  //   uint8_t* buffer = malloc(buffer_size);
  //   memcpy(&buffer[2], (void*)&channel_length, 2);
  //   memcpy(&buffer[4], message->channel, channel_length);
  //   memcpy(&buffer[4 + channel_length], message->message, message->message_size);
  //   // erlcmd_send(buffer, buffer_size);

    // platch_respond_success_std((const FlutterPlatformMessageResponseHandle*)message->response_handle, &STDFLOAT64(100.0));
    // platch_respond_error_std(
    //         (const FlutterPlatformMessageResponseHandle*)message->response_handle,
    //         "notsupported",
    //         "The vehicle doesn't support the PID used for this channel.",
    //         NULL
    //     );

    // 256 char string
    // platch_respond_success_std((const FlutterPlatformMessageResponseHandle*)message->response_handle, &STDSTRING("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  }
}

static bool runs_platform_tasks_on_current_thread(void* userdata) {
  return true;
}

static void on_post_flutter_task(
  FlutterTask task,
	uint64_t target_time,
	void *userdata
) {
  FlutterEngineRunTask(engine, &task);
  return;
}

static bool make_current(void* userdata) {
  glfwMakeContextCurrent((GLFWwindow*)userdata);
  glewInit();
  return true;
}

static bool clear_current(void* userdata) {
  glfwMakeContextCurrent((GLFWwindow*)userdata);
  return true;
}

static bool present(void* userdata) {
  glfwSwapBuffers((GLFWwindow*)userdata);
  return true;
}

static uint32_t fbo_callback(void* userdata) {
  return 0;
}

bool RunFlutter(GLFWwindow* window,
                const char* project_path,
                const char* icudtl_path) {
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

bool isCallerDown()
{
  struct pollfd ufd;
  memset(&ufd, 0, sizeof ufd);
  ufd.fd     = STDIN_FILENO;
  ufd.events = POLLIN;
  if (poll(&ufd, 1, 0) < 0)
    return true;
  return ufd.revents & POLLHUP;
}

static void handle_from_elixir(const uint8_t *buffer, size_t length, void *cookie) {
  debug("handle_from_elixir: len=%lu, event=%u", length, buffer[2]);
  erlcmd_platform_message_t* previous = head;
  erlcmd_platform_message_t* current = head;
  while(current) {
    debug("checking");
    if(current->erlcmd_handle == buffer[2]) {
      debug("responding to %lu", current->erlcmd_handle);
      FlutterEngineSendPlatformMessageResponse(engine, (const FlutterPlatformMessageResponseHandle*)current->response_handle, &buffer[3], length - sizeof(uint8_t));
      previous->next = current->next;
      free(current);
      break;
    } else {
      previous = current;
    }
    current = current->next;
  }
}

void *myThreadFun(void *vargp)
{
  for(;;) {
      for (int i = 0; i < num_pollfds; i++)
          fdset[i].revents = 0;
      int rc = poll(fdset, num_pollfds, 0);
      if (rc < 0) {
          // Retry if EINTR
          if (errno == EINTR)
              continue;

          error("poll failed with %d", errno);

      }

      if (fdset[0].revents & (POLLIN | POLLHUP))
          erlcmd_process(&handler);

      if (fdset[1].revents & (POLLIN | POLLHUP)) {
          pthread_mutex_lock(&lock);
          eventfd_t event;
          eventfd_read(fdset[1].fd, &event);

          erlcmd_platform_message_t* current = head;
          while(current != NULL) {
            if(!current->dispatched) {
              // debug("checking %lu", current->erlcmd_handle);
              size_t buffer_length = sizeof(uint16_t) + // erlcmd length packet
                              sizeof(uint8_t) + // handle
                              sizeof(uint16_t) + // channel_length
                              current->channel_length + // channel
                              sizeof(uint16_t) + // message_length
                              current->message_length;
              uint8_t buffer[buffer_length];
              buffer[0] = 0; buffer[1] = 0; // erlcmd popultaes this.
              buffer[2] = current->erlcmd_handle;
              memcpy(&buffer[3], &current->channel_length, sizeof(uint16_t)); // 3&4 = channel_length
              memcpy(&buffer[5], current->channel, current->channel_length); // channel
              memcpy(&buffer[5 + current->channel_length], &current->message_length, sizeof(uint16_t));
              memcpy(&buffer[5 + current->channel_length + sizeof(uint16_t)], current->message, current->message_length);
              erlcmd_send(buffer, buffer_length);
              current->dispatched = true;
            }
            current = current->next;
          }

          // debug("why won't this packet go thru??????");
          // uint8_t buffer[4] = {0, 0, 'a', 'b'};
          // erlcmd_send(buffer, 4);
          pthread_mutex_unlock(&lock);
      }
  }
}

int main(int argc, const char* argv[]) {
  if (argc != 3) {
    exit(EXIT_FAILURE);
  }

  const char* project_path = argv[1];
  const char* icudtl_path = argv[2];

  erlcmd_init(&handler, handle_from_elixir, NULL);

  // Initialize the file descriptor set for polling
  memset(fdset, -1, sizeof(fdset));
  fdset[0].fd = STDIN_FILENO;
  fdset[0].events = POLLIN;
  fdset[0].revents = 0;

  fdset[1].fd = eventfd(0, 0);
  fdset[1].events = POLLIN;
  fdset[1].revents = 0;

  num_pollfds = 2;

  int result = glfwInit();
  assert(result == GLFW_TRUE);

  GLFWwindow* window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight, "Flutter", NULL, NULL);
  assert(window != NULL);

  int framebuffer_width, framebuffer_height;
  glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
  g_pixelRatio = framebuffer_width / kInitialWindowWidth;

  bool runResult = RunFlutter(window, project_path, icudtl_path);
  assert(runResult);

  glfwSetKeyCallback(window, GLFWKeyCallback);
  glfwSetWindowSizeCallback(window, GLFWwindowSizeCallback);
  glfwSetMouseButtonCallback(window, GLFWmouseButtonCallback);

  if (pthread_mutex_init(&lock, NULL) != 0) {
    error("\n mutex init has failed\n");
    return 1;
  }

  pthread_t thread_id;
  pthread_create(&thread_id, NULL, myThreadFun, NULL);

  while (!glfwWindowShouldClose(window) && !isCallerDown()) {
    glfwWaitEventsTimeout(0.1);
    // glfwWaitEvents();
  }

  pthread_join(thread_id, NULL);
  pthread_mutex_destroy(&lock);
  glfwDestroyWindow(window);
  glfwTerminate();

  exit(EXIT_SUCCESS);
}
