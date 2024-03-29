/*
 *  Copyright 2014 Frank Hunleth
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Common Erlang->C port communications code
 */

#include "erlcmd.h"

#include <errno.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

/**
 * Initialize an Erlang command handler.
 *
 * @param handler the structure to initialize
 * @param read_fd the file descriptor to read()
 * @param write_fd the file descriptor to write()
 * @param request_handler callback for each message received
 * @param cookie optional data to pass back to the handler
 */
void erlcmd_init(struct erlcmd *handler, int read_fd, int write_fd,
                 void (*request_handler)(const uint8_t *req, size_t length, void *cookie),
                 void *cookie)
{
    memset(handler, 0, sizeof(*handler));
    handler->read_fd = read_fd;
    handler->write_fd = write_fd;
    handler->request_handler = request_handler;
    handler->cookie = cookie;
}

/**
 * @brief Synchronously send a response back to Erlang
 *
 * The message to be sent back to Erlang should start at &response[2]. A
 * two-byte big endian length will be filled in by this function.
 *
 * @param response what to send back
 * @param len the total length of the message including the two-byte header
 */
void erlcmd_send(struct erlcmd *handler, uint8_t *response, size_t len)
{
    uint32_t be_len = htonl(len - sizeof(uint32_t));
    memcpy(response, &be_len, sizeof(be_len));

    size_t wrote = 0;
    do {
        ssize_t amount_written = write(handler->write_fd, response + wrote, len - wrote);
        if (amount_written < 0) {
            if (errno == EINTR)
                continue;

            err(EXIT_FAILURE, "write");
        }

        wrote += amount_written;
    } while (wrote < len);
}

/**
 * @brief Dispatch commands in the buffer
 * @return the number of bytes processed
 */
static size_t erlcmd_try_dispatch(struct erlcmd *handler)
{
    /* Check for length field */
    if (handler->index < sizeof(uint32_t))
        return 0;

    uint32_t be_len;
    memcpy(&be_len, handler->buffer, sizeof(uint32_t));
    size_t msglen = ntohl(be_len);
    if (msglen + sizeof(uint32_t) > sizeof(handler->buffer))
        errx(EXIT_FAILURE, "Message too long");

    /* Check whether we've received the entire message */
    if (msglen + sizeof(uint32_t) > handler->index)
        return 0;

    handler->request_handler(handler->buffer, msglen, handler->cookie);

    return msglen + sizeof(uint32_t);
}

/**
 * @brief call to process any new requests from Erlang
 */
void erlcmd_process(struct erlcmd *handler)
{
    ssize_t amount_read = read(handler->read_fd, handler->buffer + handler->index,
                               sizeof(handler->buffer) - handler->index);
    if (amount_read < 0) {
        /* EINTR is ok to get, since we were interrupted by a signal. */
        if (errno == EINTR)
            return;

        /* Everything else is unexpected. */
        err(EXIT_FAILURE, "read");
    } else if (amount_read == 0) {
        /* EOF. Erlang process was terminated. This happens after a release or if there was an error. */
        exit(EXIT_SUCCESS);
    }

    handler->index += amount_read;
    for (;;) {
        size_t bytes_processed = erlcmd_try_dispatch(handler);

        if (bytes_processed == 0) {
            /* Only have part of the command to process. */
            break;
        } else if (handler->index > bytes_processed) {
            /* Processed the command and there's more data. */
            memmove(handler->buffer, &handler->buffer[bytes_processed], handler->index - bytes_processed);
            handler->index -= bytes_processed;
        } else {
            /* Processed the whole buffer. */
            handler->index = 0;
            break;
        }
    }
}
