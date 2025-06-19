#ifndef NET_H
#define NET_H

#include "config.h"
#include <stdint.h>
#include <openssl/ssl.h>
#include "wrk.h"
#include "machnet.h"  

typedef enum {
    OK,
    ERROR,
    RETRY
} status;

struct sock {
    status ( *connect)(connection *, char *, char *, uint16_t);
    status (   *close)(connection *);
    status (    *read)(connection *, size_t *);
    status (   *write)(connection *, char *, size_t, size_t *);
    size_t (*readable)(connection *); 
};

status sock_connect(connection *c, char *local_ip, char *remote_ip, uint16_t remote_port);
status sock_close(connection *);
status sock_read(connection *, size_t *);
status sock_write(connection *, char *, size_t, size_t *);
size_t sock_readable(connection *);

#endif /* NET_H */
