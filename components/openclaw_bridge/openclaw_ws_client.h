/*
 * openclaw_ws_client.h — plain TCP socket client
 */
#ifndef _OPENCLAW_WS_CLIENT_H_
#define _OPENCLAW_WS_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * Connect to host:port (tls flag ignored in Phase 2, always plain TCP).
 * Returns socket fd (>=0) or negative error code.
 * Caller owns fd and must close() it.
 */
int openclaw_ws_connect(const char* host, int port, bool tls);

/* Close socket */
void openclaw_ws_close(int fd);

/* Send data, returns bytes sent or negative on error */
int openclaw_ws_write(int fd, const void* buf, size_t len);

/* Receive data, returns bytes read or 0 on close or negative on error */
int openclaw_ws_read(int fd, void* buf, size_t len);

#endif // _OPENCLAW_WS_CLIENT_H_
