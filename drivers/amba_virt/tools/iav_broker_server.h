/*
 * iav_broker_server.h
 *
 * The IAV broker's place in amba-virt-server: the real driver as its backend, a small pool of
 * worker threads so a blocking call never stalls the server's receive loop, and a liveness thread
 * that releases the sessions of guests that have disconnected.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_BROKER_SERVER_H
#define IAV_BROKER_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "iav_broker.h"

#define IAV_BROKER_DEVICE_PATH	"/dev/iav"

/*
 * Start the broker. dev_fd is the open /dev/amba_virt descriptor the responses are sent on and
 * the connected guests are listed from. Returns 0 or a negative errno. Without a /dev/iav the
 * broker does not start, and requests are refused.
 */
int iav_broker_server_init(int dev_fd, enum iav_profile profile);
void iav_broker_server_shutdown(void);

/* True once the broker is running. */
int iav_broker_server_active(void);

/*
 * Hand a request to the broker from the receive loop. payload is the message after the
 * struct amba_virt_msg header. Never blocks: if the queue is full, a refusal is sent and the
 * request is dropped. Returns 0 when handled or queued, or a negative errno if the broker is not
 * running, in which case the caller should refuse.
 */
int iav_broker_server_submit(uint32_t cid, uint32_t seq, const void *payload, size_t len);

#endif /* IAV_BROKER_SERVER_H */
