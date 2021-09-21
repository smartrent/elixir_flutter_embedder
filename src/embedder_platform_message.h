#ifndef EMBEDDER_PLATFORM_MESSAGE_H
#define EMBEDDER_PLATFORM_MESSAGE_H

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include "erlcmd.h"
#include "flutter_embedder.h"

typedef struct platform_message
{
    uint8_t cookie;
    bool dispatched;
    struct platform_message *next;

    // All of these came from FlutterPlatformMessageResponseHandle
    const char* channel;
    const uint8_t* message;
    size_t channel_size;
    size_t message_size;
    const FlutterPlatformMessageResponseHandle* response_handle;
} plat_msg_container_t;

typedef struct platform_message_queue
{
  plat_msg_container_t* messages;
  pthread_mutex_t lock;
  uint32_t index;
} plat_msg_queue_t;

size_t plat_msg_queue_init(plat_msg_queue_t*);
plat_msg_container_t* plat_msg_push(plat_msg_queue_t*, const FlutterPlatformMessage*);
void plat_msg_process(plat_msg_queue_t*, FlutterEngine, const uint8_t*, size_t);
size_t plat_msg_dispatch_all(plat_msg_queue_t*, struct erlcmd* handler);
size_t plat_msg_dispatch(plat_msg_container_t*, struct erlcmd*);
void plat_msg_queue_destroy(plat_msg_queue_t*);

#endif