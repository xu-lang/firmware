#define _GNU_SOURCE

#include "ieee80211_radiotap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#define RX_ANT_MAX 4

/* radiotap header, same layout as wfb-ng radiotap_header_ht
   version(2) len(2) present(4) TX_FLAGS(2) MCS{known,flags,idx}(3) = 13 bytes
   present bits: bit15=RADIOTAP_NAMESPACE, bit19=MCS
   TX_FLAGS = 0x08 => IEEE80211_RADIOTAP_F_TX_NOACK (no hw retry) */
static uint8_t radiotap_ht[] = {
    0x00, 0x00,             /* version */
    0x0d, 0x00,             /* header length = 13 */
    0x00, 0x80, 0x08, 0x00, /* present: TX_FLAGS + MCS */
    0x08, 0x00,             /* TX_FLAGS: NOACK */
    0x3d, 0x00,             /* MCS known(bitmap), flags */
    0x07,                   /* MCS index (default MCS7) */
};

/* optional fake 802.11 header, wfb-style, only if -d given */
static const uint8_t dot11_hdr[] = {
    0x08, 0x01, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x57, 0x42, 0xaa, 0xbb, 0xcc, 0xdd,
    0x57, 0x42, 0xaa, 0xbb, 0xcc, 0xdd,
    0x00, 0x00,
};

static volatile sig_atomic_t stop = 0;
static void on_sig(int s) { (void)s; stop = 1; }

static int open_packet_socket(const char *ifname, int bind_if)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket(PF_PACKET)"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); close(fd); return -1; }

    if (bind_if)
    {
        struct sockaddr_ll sll;
        memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = ifr.ifr_ifindex;
        sll.sll_protocol = htons(ETH_P_ALL);
        if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); close(fd); return -1; }
    }
    return fd;
}

static void raw_wifi_usage(const char *prog)
{
    fprintf(stderr,
        "usage:\n"
        "  %s tx <iface> <count> [mcs 0-7] [delay_us] [-d dot11_hdr]\n"
        "      sends \"12345\" <count> times (default: mcs7, no delay, no 802.11 hdr)\n"
        "  %s rx <iface> [max_packets]\n"
        "      listens and prints averaged stats every 1s, Ctrl-C to stop\n"
        "      stats: pkt/ok/bad counts + avg RSSI range (SNR) per antenna\n"
        "      (SNR shown only if driver emits noise, see RFMETRICS)\n"
        "prerequisites (both ends):\n"
        "  - 8812au driver loaded, interface in monitor mode and up\n"
        "  - SAME channel/bandwidth on TX and RX (e.g. 5745 HT40-)\n"
        "  - two DIFFERENT 8812 cards: a card cannot hear its own monitor TX\n"
        "  - iw/ip tools installed; set reg domain first if 5G channel is blocked\n"
        "    (e.g. \"iw reg set BO\")\n"
        "  - deploy this binary to the target (e.g. scp to /mnt/mmcblk0p1)\n"
        "  - test close-range first; use low mcs (e.g. mcs 0) for distance tests\n"
        "prepare:\n"
        "  ip link set <iface> up\n"
        "  iw dev <iface> set type monitor\n"
        "  iw dev <iface> set channel 5745 HT40-\n",
        prog, prog);
}

static int raw_wifi_tx(int argc, char **argv)
{
    const char *iface = argv[2];
    int count = atoi(argv[3]);
    int mcs = (argc > 4) ? atoi(argv[4]) : 7;
    int delay_us = (argc > 5) ? atoi(argv[5]) : 0;
    int use_dot11 = (argc > 6 && strcmp(argv[6], "-d") == 0);

    if (count <= 0 || mcs < 0 || mcs > 7)
    {
        fprintf(stderr, "bad args\n");
        return 1;
    }

    radiotap_ht[12] = (uint8_t)mcs;

    int fd = open_packet_socket(iface, 1);
    if (fd < 0) return 1;

    /* skip qdisc to lower latency (optional, works on most kernels) */
    int bypass = 1;
    setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass));

    uint8_t buf[256];
    size_t n = 0;
    memcpy(buf + n, radiotap_ht, sizeof(radiotap_ht)); n += sizeof(radiotap_ht);
    if (use_dot11)
    {
        memcpy(buf + n, dot11_hdr, sizeof(dot11_hdr)); n += sizeof(dot11_hdr);
    }
    const char payload[] = "12345";
    memcpy(buf + n, payload, sizeof(payload) - 1); n += sizeof(payload) - 1;

    printf("tx %s: count=%d mcs=%d len=%zuB payload=\"%s\"%s\n",
           iface, count, mcs, n, payload, use_dot11 ? " +dot11" : "");

    int sent = 0, failed = 0;
    for (int i = 0; i < count; i++)
    {
        if (use_dot11) buf[sizeof(radiotap_ht) + 22] = (uint8_t)i; /* seq */
        ssize_t rc = send(fd, buf, n, 0);
        if (rc < 0)
        {
            failed++;
            fprintf(stderr, "send #%d failed: %s\n", i, strerror(errno));
        }
        else
            sent++;
        if (delay_us > 0)
        {
            struct timespec ts = { 0, delay_us * 1000L };
            nanosleep(&ts, NULL);
        }
    }
    printf("tx done: sent=%d failed=%d\n", sent, failed);
    close(fd);
    return 0;
}

static int parse_rx_radiotap(const uint8_t *buf, size_t len,
                             int8_t *rssi, int8_t *noise,
                             int *ant_cnt, uint32_t *freq, uint8_t *mcs)
{
    struct ieee80211_radiotap_iterator it;
    int ant_idx = 0;
    uint8_t mcs_have = 0;

    if (len < sizeof(struct ieee80211_radiotap_header))
        return -1;
    if (ieee80211_radiotap_iterator_init(&it, (struct ieee80211_radiotap_header *)buf,
                                         (int)len, NULL))
        return -1;

    for (int i = 0; i < RX_ANT_MAX; i++)
    {
        rssi[i] = SCHAR_MIN;
        noise[i] = SCHAR_MAX;
    }
    *ant_cnt = 0;
    *freq = 0;
    *mcs = 0;

    while (ieee80211_radiotap_iterator_next(&it) == 0)
    {
        switch (it.this_arg_index)
        {
        case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
            if (ant_idx < RX_ANT_MAX) rssi[ant_idx] = *(int8_t *)it.this_arg;
            break;
        case IEEE80211_RADIOTAP_DBM_ANTNOISE:
            if (ant_idx < RX_ANT_MAX) noise[ant_idx] = *(int8_t *)it.this_arg;
            break;
        case IEEE80211_RADIOTAP_ANTENNA:
            if (ant_idx < RX_ANT_MAX) ant_idx++;
            if (ant_idx > *ant_cnt) *ant_cnt = ant_idx;
            break;
        case IEEE80211_RADIOTAP_CHANNEL:
            *freq = get_unaligned_le32(it.this_arg) & 0xffff;
            break;
        case IEEE80211_RADIOTAP_MCS:
            mcs_have = it.this_arg[0];
            if (mcs_have & IEEE80211_RADIOTAP_MCS_HAVE_MCS)
                *mcs = it.this_arg[2] & 0x7f;
            break;
        default:
            break;
        }
    }
    if (*ant_cnt == 0) *ant_cnt = 1;
    return 0;
}

static int raw_wifi_rx(int argc, char **argv)
{
    const char *iface = argv[2];
    int max_pkts = (argc > 3) ? atoi(argv[3]) : 0;

    int fd = open_packet_socket(iface, 1);
    if (fd < 0) return 1;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    printf("rx %s: listening, stats every 1s (Ctrl-C to stop)\n", iface);

    uint8_t buf[2048];
    long total = 0, matched = 0, other = 0;
    long win_total = 0, win_matched = 0, win_other = 0;
    long rssi_sum[RX_ANT_MAX] = {0}, rssi_cnt[RX_ANT_MAX] = {0};
    int8_t rssi_min[RX_ANT_MAX], rssi_max[RX_ANT_MAX];
    int8_t noise_sum[RX_ANT_MAX] = {0};
    long noise_cnt[RX_ANT_MAX] = {0};

    for (int a = 0; a < RX_ANT_MAX; a++)
    {
        rssi_min[a] = SCHAR_MAX;
        rssi_max[a] = SCHAR_MIN;
    }

    struct timespec now, last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (!stop)
    {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0)
        {
            if (errno == EINTR) continue;
            perror("recv"); break;
        }

        if (len < 8) continue;
        size_t rtap_len = (size_t)buf[2] | ((size_t)buf[3] << 8);
        if ((size_t)len < rtap_len) continue;

        int8_t rssi[RX_ANT_MAX], noise[RX_ANT_MAX];
        int ant_cnt = 1;
        uint32_t freq = 0;
        uint8_t mcs = 0;
        parse_rx_radiotap(buf, (size_t)len, rssi, noise, &ant_cnt, &freq, &mcs);

        size_t paylen = (size_t)len - rtap_len;
        const uint8_t *pay = buf + rtap_len;
        total++;
        win_total++;

        if (paylen == 5 && memcmp(pay, "12345", 5) == 0)
        {
            matched++;
            win_matched++;
        }
        else
        {
            other++;
            win_other++;
        }

        for (int a = 0; a < ant_cnt && a < RX_ANT_MAX; a++)
        {
            if (rssi[a] != SCHAR_MIN)
            {
                rssi_sum[a] += rssi[a];
                rssi_cnt[a]++;
                if (rssi[a] < rssi_min[a]) rssi_min[a] = rssi[a];
                if (rssi[a] > rssi_max[a]) rssi_max[a] = rssi[a];
            }
            if (noise[a] != SCHAR_MAX)
            {
                noise_sum[a] += noise[a];
                noise_cnt[a]++;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last.tv_sec >= 1)
        {
            printf("[+%lus] pkt=%ld ok=%ld bad=%ld  rssi:",
                   now.tv_sec - last.tv_sec, win_total, win_matched, win_other);
            for (int a = 0; a < ant_cnt && a < RX_ANT_MAX; a++)
            {
                if (rssi_cnt[a] > 0)
                {
                    long avg = rssi_sum[a] / rssi_cnt[a];
                    int snr = 0;
                    if (noise_cnt[a] > 0)
                        snr = (int)(avg - noise_sum[a] / noise_cnt[a]);
                    printf(" ant%d=%lddBm[%d..%d](%ddB)", a, avg,
                           rssi_min[a], rssi_max[a], snr);
                }
                else
                {
                    printf(" ant%d=n/a", a);
                }
            }
            printf("\n");
            fflush(stdout);

            win_total = win_matched = win_other = 0;
            for (int a = 0; a < RX_ANT_MAX; a++)
            {
                rssi_sum[a] = 0; rssi_cnt[a] = 0;
                noise_sum[a] = 0; noise_cnt[a] = 0;
                rssi_min[a] = SCHAR_MAX; rssi_max[a] = SCHAR_MIN;
            }
            last = now;
        }

        if (max_pkts > 0 && total >= max_pkts) break;
    }

    printf("rx done: total=%ld matched_12345=%ld other=%ld\n",
           total, matched, other);
    close(fd);
    return 0;
}

int raw_wifi_main(int argc, char **argv)
{
    if (argc < 3) { raw_wifi_usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "tx"))
        return raw_wifi_tx(argc, argv);
    else if (!strcmp(argv[1], "rx"))
        return raw_wifi_rx(argc, argv);

    raw_wifi_usage(argv[0]);
    return 1;
}
