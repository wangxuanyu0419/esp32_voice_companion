/*
 * openclaw_ws_client.c — plain TCP socket client (Phase 2 fallback)
 *
 * TLS requires esp_tls component which is not available in MINIMAL_BUILD.
 * Phase 2 uses plain TCP; Phase 3 will re-add TLS via mbedtls directly.
 */
#include "openclaw_ws_client.h"
#include <esp_log.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <lwip/netdb.h>

static const char* TAG = "OC_WSClient";

int openclaw_ws_connect(const char* host, int port, bool tls) {
    (void)tls;
    struct hostent* he = gethostbyname(host);
    if (he == NULL) { ESP_LOGE(TAG, "DNS fail: %s", host); return -1; }

    char ip_str[INET_ADDRSTRLEN];
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)port);
    memcpy(&dest.sin_addr, he->h_addr_list[0], he->h_length);
    inet_ntop(AF_INET, &dest.sin_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "DNS %s → %s", host, ip_str);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { ESP_LOGE(TAG, "socket fail"); return -2; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ret = connect(fd, (struct sockaddr*)&dest, sizeof(dest));
    if (ret < 0 && errno != EINPROGRESS) { close(fd); return -3; }

    fd_set wfds;
    FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ret = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (ret <= 0) { close(fd); return -4; }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr != 0) { close(fd); return -5; }

    fcntl(fd, F_SETFL, flags);
    ESP_LOGI(TAG, "TCP fd=%d to %s:%d", fd, ip_str, port);
    return fd;
}

void openclaw_ws_close(int fd) {
    if (fd >= 0) close(fd);
}

int openclaw_ws_write(int fd, const void* buf, size_t len) {
    return (int)send(fd, buf, len, 0);
}

int openclaw_ws_read(int fd, void* buf, size_t len) {
    return (int)recv(fd, buf, len, 0);
}
