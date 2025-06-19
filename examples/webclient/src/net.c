// Copyright (C) 2013 - Will Glozer.  All rights reserved.

#include "net.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "machnet.h"

status sock_connect(connection *c, char *local_ip, char *remote_ip,
                    uint16_t remote_port) {
  if (!remote_ip || strlen(remote_ip) == 0) {
    fprintf(stderr, "[ERROR] Invalid remote IP in sock_connect.\n");
    return ERROR;
  }

  assert(c->channel_ctx == NULL);

  c->channel_ctx = machnet_attach();

  if (c->channel_ctx == NULL) {
    fprintf(stderr, "[ERROR] sock_connect: Failed to attach to Machnet.\n");
    return ERROR;
  }

  int connect_status = machnet_connect(c->channel_ctx, local_ip, remote_ip,
                                       remote_port, &c->machnet_flow);
  if (connect_status == 0) {
    printf("[INFO] sock_connect: Connected successfully to %s:%u\n", remote_ip,
           remote_port);
    return OK;
  } else {
    fprintf(stderr, "[ERROR] sock_connect: Failed to connect to %s:%u: %s\n",
            remote_ip, remote_port, strerror(errno));
    return ERROR;
  }
}

status sock_close(connection *c) {
  assert(c->channel_ctx != NULL);
  fprintf(stderr, "[INFO] sock_close: Closing connection\n");
  return OK;
}

status sock_read(connection *c, size_t *n) {
  MachnetFlow_t flow_info;
  ssize_t bytes_received =
      machnet_recv(c->channel_ctx, c->buf, sizeof(c->buf), &flow_info);

  if (bytes_received > 0) {
    *n = (size_t)bytes_received;

#ifdef MACHNET_DEBUG
    printf("[DEBUG] sock_recv: Received %ld bytes.\n", bytes_received);
#endif

    return OK;
  } else if (bytes_received == 0) {
    // poll again
    return RETRY;
  } else {
    fprintf(stderr, "[ERROR] sock_read: Failed to read from Machnet: %s\n",
            strerror(errno));
    return ERROR;
  }
}

status sock_write(connection *c, char *buf, size_t len, size_t *n) {
  int result = machnet_send(c->channel_ctx, c->machnet_flow, buf, len);

  if (result >= 0) {
    *n = len;

#ifdef MACHNET_DEBUG
    printf("[DEBUG] sock_write: Sent %zu bytes\n", len);
#endif

    return OK;
  } else {
    fprintf(stderr, "[ERROR] sock_write: Failed to send: %s\n",
            strerror(errno));
    return ERROR;
  }
}

size_t sock_readable(connection *c) {
  // Machnet is polling
  return 1;
}