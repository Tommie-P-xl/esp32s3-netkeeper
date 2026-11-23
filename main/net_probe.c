#include "net_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#define ICMP_ECHO_ID       0x5047u /* 'PG' */
#define ICMP_TYPE_ECHO_REQ 8u
#define ICMP_TYPE_ECHO_REP 0u

static uint16_t s_icmp_seq;

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint16_t icmp_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

bool net_probe_icmp(const char *ip, int timeout_ms)
{
    if (ip == NULL || ip[0] == '\0' || timeout_ms <= 0) {
        return false;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        return false;
    }

    const int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        return false;
    }

    const uint16_t seq = ++s_icmp_seq;
    uint8_t pkt[12] = {0};
    pkt[0] = ICMP_TYPE_ECHO_REQ;
    pkt[1] = 0;
    pkt[4] = (uint8_t)(ICMP_ECHO_ID >> 8);
    pkt[5] = (uint8_t)(ICMP_ECHO_ID & 0xFFu);
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)(seq & 0xFFu);
    pkt[8] = 'n';
    pkt[9] = 'k';
    pkt[10] = 'p';
    pkt[11] = 'g';

    const uint16_t csum = icmp_checksum(pkt, sizeof(pkt));
    pkt[2] = (uint8_t)(csum >> 8);
    pkt[3] = (uint8_t)(csum & 0xFFu);

    bool ok = false;
    if (sendto(sock, pkt, sizeof(pkt), 0, (const struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)sizeof(pkt)) {
        const int64_t deadline = mono_ms() + timeout_ms;
        uint8_t rx[96];

        for (;;) {
            const int64_t left = deadline - mono_ms();
            if (left <= 0) {
                break;
            }
            struct timeval tv = {
                .tv_sec = (long)(left / 1000),
                .tv_usec = (long)((left % 1000) * 1000),
            };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            struct sockaddr_in from = {0};
            socklen_t flen = sizeof(from);
            const ssize_t n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &flen);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (from.sin_addr.s_addr != dst.sin_addr.s_addr || n < 28) {
                continue;
            }

            /* 原始套接字接收含 IPv4 头，按 IHL 字段跳过。 */
            const size_t ihl = (size_t)(rx[0] & 0x0Fu) * 4u;
            if (ihl < 20u || (size_t)n < ihl + 8u) {
                continue;
            }
            const uint8_t *icmp = rx + ihl;
            const uint16_t rid = ((uint16_t)icmp[4] << 8) | icmp[5];
            const uint16_t rseq = ((uint16_t)icmp[6] << 8) | icmp[7];
            if (icmp[0] == ICMP_TYPE_ECHO_REP && icmp[1] == 0u &&
                rid == ICMP_ECHO_ID && rseq == seq) {
                ok = true;
                break;
            }
        }
    }

    close(sock);
    return ok;
}

static bool tcp_connect_addr(const struct sockaddr_in *dst, int timeout_ms)
{
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return false;
    }

    bool ok = false;
    const int rc = connect(sock, (const struct sockaddr *)dst, sizeof(*dst));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0 && FD_ISSET(sock, &wfds)) {
            int so_err = -1;
            socklen_t len = sizeof(so_err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err == 0) {
                ok = true;
            }
        }
    }

    close(sock);
    return ok;
}

bool net_probe_tcp(const char *ip, uint16_t port, int timeout_ms)
{
    if (ip == NULL || ip[0] == '\0' || port == 0 || timeout_ms <= 0) {
        return false;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        return false;
    }

    return tcp_connect_addr(&dst, timeout_ms);
}

bool net_probe_origin_tcp(const char *origin, int timeout_ms)
{
    if (origin == NULL || timeout_ms <= 0) {
        return false;
    }

    uint16_t port = 0;
    const char *p = NULL;
    if (strncmp(origin, "http://", 7) == 0) {
        port = 80;
        p = origin + 7;
    } else if (strncmp(origin, "https://", 8) == 0) {
        port = 443;
        p = origin + 8;
    } else {
        return false;
    }

    const size_t auth_len = strcspn(p, "/?#");
    if (auth_len == 0 || auth_len >= 64) {
        return false;
    }

    char host[64] = {0};
    const char *colon = memchr(p, ':', auth_len);
    if (colon != NULL) {
        const size_t hlen = (size_t)(colon - p);
        if (hlen == 0 || hlen >= sizeof(host)) {
            return false;
        }
        memcpy(host, p, hlen);
        host[hlen] = '\0';

        char port_str[8] = {0};
        const size_t plen = auth_len - hlen - 1;
        if (plen == 0 || plen >= sizeof(port_str)) {
            return false;
        }
        memcpy(port_str, colon + 1, plen);
        char *end = NULL;
        const long parsed = strtol(port_str, &end, 10);
        if (end == port_str || *end != '\0' || parsed <= 0 || parsed > 65535) {
            return false;
        }
        port = (uint16_t)parsed;
    } else {
        memcpy(host, p, auth_len);
        host[auth_len] = '\0';
    }

    /* IPv4 字面量直接建连，避免在断网状态下卡在 DNS 超时。 */
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) == 1) {
        return tcp_connect_addr(&dst, timeout_ms);
    }

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    bool ok = false;
    if (res->ai_addrlen >= sizeof(dst)) {
        memcpy(&dst, res->ai_addr, sizeof(dst));
        dst.sin_port = htons(port);
        ok = tcp_connect_addr(&dst, timeout_ms);
    }
    freeaddrinfo(res);
    return ok;
}
