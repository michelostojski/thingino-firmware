/*
 * hiwifi - netlink-28 probe for the Hi3861L hichannel driver (addx firmware).
 *
 * Your device runs the addx/SPC021 firmware, whose host command protocol is a
 * BINARY framing (recovered from addx_mcu_daemon's framer at 0x4096cc):
 *
 *     USER_MSG payload = [cmd_id:1][len:4 LE][payload:len]
 *
 * This tool sends that framing over NETLINK family 28 (the socket the driver
 * creates) and hexdumps whatever the device replies, so we can (a) confirm the
 * framing works on real hardware and (b) map which command ids are live.
 *
 * Command ids seen in the daemon (0x80..0xb7). The three "safe get" ids below
 * (0xb0/0xb1/0xb3) copy a response back out, so they should reply with data and
 * are the safest to confirm the framing. A full-range probe may hit commands
 * with side effects (reboot/poweroff of the MCU), so it is opt-in.
 *
 * Build:  mipsel-linux-gcc -static -O2 -o hiwifi hiwifi.c
 *
 * Usage:
 *   hiwifi get                 probe the 3 safe get-ids (confirm framing)
 *   hiwifi probe <hexA> <hexB> probe every id in [A..B]  (opt-in; side effects!)
 *   hiwifi raw <idhex> [payloadhex]   send one arbitrary command
 *
 * Examples:
 *   hiwifi get
 *   hiwifi raw b0
 *   hiwifi raw 90 0102030405
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <time.h>

#define NL_HI3861L   28
#define NL_PID       0x44c   /* the daemon binds/sends with this fixed pid; the
                                driver routes device replies back to it */

static int nl_fd = -1;

static int nl_open(void)
{
    struct sockaddr_nl src;
    int fd = socket(AF_NETLINK, SOCK_RAW, NL_HI3861L);
    if (fd < 0) { perror("socket(AF_NETLINK,28)"); return -1; }
    memset(&src, 0, sizeof(src));
    src.nl_family = AF_NETLINK;
    src.nl_pid = NL_PID;
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) { perror("bind(pid=0x44c)"); close(fd); return -1; }
    return fd;
}

/* Send one addx command: [id][len:4 LE][payload]. */
static int send_cmd(unsigned char id, const unsigned char *payload, int plen)
{
    unsigned char msg[5 + 1024];
    int mlen = 5 + plen;
    struct sockaddr_nl dst;
    struct nlmsghdr *nlh;
    struct iovec iov;
    struct msghdr mh;
    int total, ret;

    if (plen < 0 || plen > 1024) return -1;
    msg[0] = id;
    msg[1] = (unsigned char)(plen & 0xff);
    msg[2] = (unsigned char)((plen >> 8) & 0xff);
    msg[3] = (unsigned char)((plen >> 16) & 0xff);
    msg[4] = (unsigned char)((plen >> 24) & 0xff);
    if (plen) memcpy(msg + 5, payload, plen);

    total = NLMSG_HDRLEN + mlen;
    nlh = (struct nlmsghdr *)calloc(1, total);
    if (!nlh) return -1;
    nlh->nlmsg_len = total;
    nlh->nlmsg_pid = NL_PID;
    memcpy(NLMSG_DATA(nlh), msg, mlen);

    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;
    iov.iov_base = nlh; iov.iov_len = total;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &dst; mh.msg_namelen = sizeof(dst);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;

    ret = sendmsg(nl_fd, &mh, 0);
    free(nlh);
    if (ret < 0) { perror("sendmsg"); return -1; }
    return 0;
}

static void hexdump(const unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        printf("%02x ", p[i]);
        if ((i & 15) == 15) printf("\n                 ");
    }
    printf("\n");
}

/* Wait up to timeout_ms for a reply; print it. Returns bytes received (0 = none). */
static int recv_reply(int timeout_ms)
{
    char buf[8192];
    struct iovec iov = { buf, sizeof(buf) };
    struct sockaddr_nl from;
    struct msghdr mh;
    struct timeval tv;
    fd_set rfds;
    int n, got = 0;

    FD_ZERO(&rfds); FD_SET(nl_fd, &rfds);
    tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(nl_fd + 1, &rfds, NULL, NULL, &tv) <= 0) return 0;

    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &from; mh.msg_namelen = sizeof(from);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    n = recvmsg(nl_fd, &mh, 0);
    if (n <= 0) return 0;

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    for (; NLMSG_OK(nlh, (unsigned)n); nlh = NLMSG_NEXT(nlh, n)) {
        int plen = nlh->nlmsg_len - NLMSG_HDRLEN;
        unsigned char *p = (unsigned char *)NLMSG_DATA(nlh);
        if (plen <= 0) continue;
        got += plen;
        printf("    <- reply %d bytes: ", plen);
        hexdump(p, plen > 64 ? 64 : plen);
    }
    return got;
}

static int hexbyte(const char *s) { return (int)strtol(s, NULL, 16) & 0xff; }

static int parse_hex(const char *s, unsigned char *out, int max)
{
    int n = 0;
    while (s[0] && s[1] && n < max) {
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (unsigned char)strtol(b, NULL, 16);
        s += 2;
    }
    return n;
}

static void probe_one(unsigned char id)
{
    int got;
    printf(">> cmd 0x%02x (len=0)\n", id);
    if (send_cmd(id, NULL, 0) < 0) return;
    got = recv_reply(600);
    if (!got) printf("    (no reply)\n");
}

int main(int argc, char **argv)
{
    static const unsigned char safe_gets[] = { 0xb0, 0xb1, 0xb3 };
    int i;

    if (argc < 2) {
        fprintf(stderr,
          "usage:\n"
          "  %s get                  probe safe get-ids 0xb0/0xb1/0xb3\n"
          "  %s probe <A> <B>        probe every id in [A..B] hex (side effects!)\n"
          "  %s raw <id> [payload]   send one command, id+payload in hex\n", argv[0], argv[0], argv[0]);
        return 1;
    }

    nl_fd = nl_open();
    if (nl_fd < 0) return 1;

    if (!strcmp(argv[1], "get")) {
        for (i = 0; i < (int)sizeof(safe_gets); i++) probe_one(safe_gets[i]);
    } else if (!strcmp(argv[1], "probe") && argc >= 4) {
        int a = hexbyte(argv[2]), b = hexbyte(argv[3]);
        printf("probing ids 0x%02x..0x%02x (empty payload)\n", a, b);
        for (i = a; i <= b; i++) { probe_one((unsigned char)i); usleep(150000); }
    } else if (!strcmp(argv[1], "raw") && argc >= 3) {
        unsigned char id = hexbyte(argv[2]);
        unsigned char pl[1024]; int pn = 0;
        if (argc >= 4) pn = parse_hex(argv[3], pl, sizeof(pl));
        printf(">> cmd 0x%02x len=%d\n", id, pn);
        if (send_cmd(id, pl, pn) == 0) {
            if (!recv_reply(1500)) printf("    (no reply)\n");
        }
    } else {
        fprintf(stderr, "bad args\n");
        close(nl_fd); return 1;
    }

    close(nl_fd);
    return 0;
}
