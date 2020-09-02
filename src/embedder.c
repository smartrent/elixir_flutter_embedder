
#include <assert.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "flutter_embedder.h"
#include "platformchannel.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

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

static void on_platform_message(
	const FlutterPlatformMessage* message,
	void* userdata
) {
  
  if(strcmp(message->channel, "platform/idk") == 0) {
    debug("platform message %s", message->channel);
    debug("platform message content: %s", (char*)message->message);
    platch_respond_success_std((const FlutterPlatformMessageResponseHandle*)message->response_handle, &STDINT32(123));
  }
  // if(message->channel)
  // const char* resp = '{"method":"SystemChrome.setSystemUIOverlayStyle","args":{"systemNavigationBarColor":null,"systemNavigationBarDividerColor":null,"statusBarColor":null,"statusBarBrightness":"Brightness.dark","statusBarIconBrightness":"Brightness.light","systemNavigationBarIconBrightness":null}}';
  // FlutterEngineResult result = FlutterEngineSendPlatformMessageResponse(engine, message->response_handle, message->message, message->message_size);
  // assert(result == kSuccess);
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

int main(int argc, const char* argv[]) {
  if (argc != 3) {
    exit(EXIT_FAILURE);
  }

  const char* project_path = argv[1];
  const char* icudtl_path = argv[2];

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

  while (!glfwWindowShouldClose(window) && !isCallerDown()) {
    glfwWaitEvents();
  }

  glfwDestroyWindow(window);
  glfwTerminate();

  exit(EXIT_SUCCESS);
}
