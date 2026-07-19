#define _DEFAULT_SOURCE

#include "time_sync.h"

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int64_t g_pc_offset_us;
static int g_local_port;

int64_t time_sync_offset_us(void)
{
    return g_pc_offset_us;
}

int time_sync_local_port(void)
{
    return g_local_port;
}

int time_sync_make_request(void *buf, int len, uint64_t *t1_us)
{
    struct timespec ts;

    if (len < 12) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *t1_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
    ((char *)buf)[0] = 'P';
    ((char *)buf)[1] = 'S';
    ((char *)buf)[2] = 'Y';
    ((char *)buf)[3] = 'N';
    memcpy((char *)buf + 4, t1_us, 8);
    return 12;
}

int time_sync_process_response(const void *buf, int len, uint64_t t1_us, uint64_t t4_us)
{
    const char *rsp = buf;
    uint64_t t2, t3;

    if (len < 28 || rsp[0] != 'P' || rsp[1] != 'S' || rsp[2] != 'Y' || rsp[3] != 'N')
        return -1;

    memcpy(&t2, rsp + 12, 8);
    memcpy(&t3, rsp + 20, 8);
    g_pc_offset_us = (int64_t)((t2 - t1_us) + (t3 - t4_us)) / 2;
    return 0;
}

static int do_time_sync(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int fd = -1;
    int best = -1;
    int64_t best_rtt = INT64_MAX;
    int64_t offset = 0;

    g_local_port = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res)) {
        fprintf(stderr, "sync: getaddrinfo(%s:%s) failed\n", host, port);
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        fprintf(stderr, "sync: socket: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    struct sockaddr_storage addr;
    socklen_t addrlen = res->ai_addrlen;
    memcpy(&addr, res->ai_addr, addrlen);
    freeaddrinfo(res);

    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        fprintf(stderr, "sync: connect(%s:%s): %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_storage local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (!getsockname(fd, (struct sockaddr *)&local_addr, &local_len)) {
        char local_port[16];
        if (!getnameinfo((struct sockaddr *)&local_addr, local_len, NULL, 0,
                local_port, sizeof(local_port), NI_NUMERICSERV))
            g_local_port = atoi(local_port);
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (int round = 0; round < 12; round++) {
        struct timespec ts;
        uint64_t t1, t2, t3, t4;
        char req[12];
        char rsp[28];
        ssize_t n;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        t1 = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;

        time_sync_make_request(req, sizeof(req), &t1);

        if (send(fd, req, sizeof(req), 0) < 0)
            continue;

        n = recv(fd, rsp, sizeof(rsp), 0);
        if (n < (ssize_t)sizeof(rsp))
            continue;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        t4 = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;

        if (rsp[0] != 'P' || rsp[1] != 'S' || rsp[2] != 'Y' || rsp[3] != 'N')
            continue;

        memcpy(&t2, rsp + 12, 8);
        memcpy(&t3, rsp + 20, 8);

        int64_t rtt = (int64_t)((t4 - t1) - (t3 - t2));
        if (rtt < best_rtt) {
            best_rtt = rtt;
            best = round;
            offset = (int64_t)((t2 - t1) + (t3 - t4)) / 2;
        }
    }

    close(fd);

    if (best < 0) {
        fprintf(stderr, "sync: no valid response from %s:%s\n", host, port);
        return -1;
    }

    g_pc_offset_us = offset;
    printf("sync: offset=%lld us best_rtt=%lld us (round %d)\n",
           (long long)offset, (long long)best_rtt, best);
    return 0;
}

int time_sync_addr(const char *addr)
{
    char host[128];
    char port[16];
    const char *colon = strrchr(addr, ':');

    if (!colon || colon == addr || !colon[1]) {
        fprintf(stderr, "invalid --sync %s, expected host:port\n", addr);
        return -1;
    }

    size_t hlen = (size_t)(colon - addr);
    if (hlen >= sizeof(host))
        hlen = sizeof(host) - 1;
    memcpy(host, addr, hlen);
    host[hlen] = '\0';

    strncpy(port, colon + 1, sizeof(port) - 1);
    port[sizeof(port) - 1] = '\0';

    return do_time_sync(host, port);
}
