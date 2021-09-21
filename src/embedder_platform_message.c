#include <pthread.h>
#include <string.h>
#include "erlcmd.h"
#include "embedder_platform_message.h"
#include "flutter_embedder.h"

/**
 * Initialize queue and pthread lock
 * @param queue the queue
 */
size_t plat_msg_queue_init(plat_msg_queue_t *queue)
{
    memset(queue, 0, sizeof(plat_msg_queue_t));
    queue->messages = NULL;
    queue->index = 1;
    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        return 1;
    }
    return 0;
}

/**
 * @param message message data that came from `on_platform_message` Flutter callback
 */
plat_msg_container_t *plat_msg_push(plat_msg_queue_t *queue, const FlutterPlatformMessage *message)
{
    pthread_mutex_lock(&queue->lock);
    plat_msg_container_t *new = malloc(sizeof(plat_msg_container_t));
    if (!new)
        return NULL;

    new->message_size = message->message_size;
    new->message = (const uint8_t *)malloc(message->message_size);
    if (!new->message_size) {
        free(new);
        return NULL;
    }
    memset((uint8_t *)new->message, 0, message->message_size);
    memcpy((uint8_t *)new->message, message->message, message->message_size);

    new->channel_size = strlen(message->channel);
    new->channel = (const char *)malloc(new->channel_size);
    if (!new->channel) {
        free((char *)new->message);
        free(new);
        return NULL;
    }
    memset((char *)new->channel, 0, new->channel_size);
    memcpy((char *)new->channel, message->channel, new->channel_size);
    new->response_handle = message->response_handle;
    new->dispatched = false;
    new->cookie = queue->index++;
    new->next = queue->messages;
    queue->messages = new;

    pthread_mutex_unlock(&queue->lock);
    return new;
}

void plat_msg_process(plat_msg_queue_t *queue,
                      FlutterEngine engine,
                      const uint8_t *buffer,
                      size_t length)
{

    pthread_mutex_lock(&queue->lock);
    // I didn't come up w/ this. Uses double pointer to slice out nodes without
    // having to track current, previous and head all at the same time
    // https://codereview.stackexchange.com/posts/539/revisions
    for (plat_msg_container_t **current = &queue->messages; *current; current = &(*current)->next) {
        if ((*current)->cookie == buffer[sizeof(uint32_t)]) {
            // I have no idea if this is correct. there are little docs for it.
            // What docs to exist say `FlutterEngineSendPlatformMessageResponse` must ALWAYS
            // be called, but i'm not really sure what to call it with if the
            // platform message has no response.
            // FlutterPi doesn't call anything if there's no listener.
            FlutterEngineSendPlatformMessageResponse(engine,
                                                     (*current)->response_handle,
                                                     buffer + sizeof(uint32_t) +1,
                                                     length - sizeof(uint8_t));
            plat_msg_container_t *next = (*current)->next;
            free((uint8_t *)(*current)->message);
            free((char *)(*current)->channel);
            (*current)->message = NULL;
            (*current)->channel = NULL;
            free(*current);
            *current = next;
            break;
        }
    }
    pthread_mutex_unlock(&queue->lock);
    return;
}

size_t plat_msg_dispatch_all(plat_msg_queue_t *queue, struct erlcmd *handler)
{
    pthread_mutex_lock(&queue->lock);
    plat_msg_container_t *current = queue->messages;
    size_t r = 0;
    while (current) {
        if (!current->dispatched) {
            r = plat_msg_dispatch(current, handler);
            if (r < 0) {
                r = -1;
                goto cleanup;
            }
        }
        current = current->next;
    }
cleanup:
    pthread_mutex_unlock(&queue->lock);
    return r;
}

size_t plat_msg_dispatch(plat_msg_container_t *container, struct erlcmd *handler)
{
    if (container->dispatched)
        return -3;

    if (container->message_size > 0xff)
        return -2;

    size_t buffer_length =  sizeof(uint32_t) + // erlcmd length packet
                            sizeof(uint8_t) + // opcode
                            sizeof(uint8_t) + // handle
                            sizeof(uint16_t) + // channel_length
                            container->channel_size + // channel
                            sizeof(uint16_t) + // message_length
                            container->message_size;
    uint8_t *buffer;
    buffer = malloc(buffer_length);
    if (!buffer)
        return -1;

    memset(buffer, 0, buffer_length);

    buffer[sizeof(uint32_t)] = 0x0;
    buffer[sizeof(uint32_t) +1] = container->cookie;
    memcpy(buffer + sizeof(uint32_t) +2, &container->channel_size, sizeof(uint16_t));
    memcpy(buffer + sizeof(uint32_t) +4, container->channel, container->channel_size);
    memcpy(buffer + sizeof(uint32_t) +4 + container->channel_size, &container->message_size, sizeof(uint16_t));
    memcpy(buffer + sizeof(uint32_t) +4 + container->channel_size + sizeof(uint16_t), container->message,
           container->message_size);

    erlcmd_send(handler, buffer, buffer_length);
    container->dispatched = true;

    free(buffer);
    return 0;
}

void plat_msg_queue_destroy(plat_msg_queue_t *queue)
{
    pthread_mutex_destroy(&queue->lock);
    return;
}