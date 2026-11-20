/*
 * hinl.c - send one addx-format command to the Hi3861L over netlink 28 and
 *          print the chip's replies.
 *
 * Why not the wlan0 ioctl: the hichannel driver delivers chip replies to the
 * netlink pid of the LAST message it received over netlink 28. A command sent
 * by ioctl never registers a pid, so the reply is unicast to pid 0, which is
 * the kernel's own socket, and gets fed back to the chip as a new command.
 * Sending over netlink from the same socket we listen on avoids that.
 *
 * Wire format (from addx_mcu_daemon's framer at 0x4096cc):
 *     [cmd:1][len:4 little-endian][payload:len]
 *
 * Usage:
 *   hinl cmd <idhex> [payloadhex] [seconds]
 *
 * Examples:
 *   hinl cmd 80            get_version
 *   hinl cmd 9f            get_soc_min_version
 *   hinl cmd 8c            get_wifi_status
 *
 * Build (in the thingino container):
 *   O=output/cg721-pr/ostingo_l2_t31l_hi3881-3.10.14-uclibc
 *   "$O/host/bin/mipsel-linux-gcc" -O2 -static -o hinl hinl.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/netlink.h>

#define NL_CHANNEL 28
#define MAXPAY     1024

static void dump(const unsigned char *b, int n)
{
    int i, j;

    for (i = 0; i < n; i += 16) {
        printf("  %04x  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < n)
                printf("%02x ", b[i + j]);
            else
                printf("   ");
        }
        printf(" |");
        for (j = 0; j < 16 && i + j < n; j++)
            putchar(isprint(b[i + j]) ? b[i + j] : '.');
        printf("|\n");
    }
}

static int hex2bin(const char *h, unsigned char *out, int max)
{
    int n = 0;
    unsigned v;

    while (h[0] && h[1] && n < max) {
        if (sscanf(h, "%2x", &v) != 1)
            return -1;
        out[n++] = (unsigned char)v;
        h += 2;
    }
    return (h[0] == 0) ? n : -1;
}

/* ---- "up": watchdog off, connect, wait for the chip's DHCP lease, copy the
   chip's MAC/IP/mask/gateway to wlan0 (host and chip share one address) ---- */

static int nl_open(void)
{
    struct sockaddr_nl me;
    int s = socket(AF_NETLINK, SOCK_RAW, NL_CHANNEL);

    if (s < 0)
        return -1;
    memset(&me, 0, sizeof(me));
    me.nl_family = AF_NETLINK;
    me.nl_pid = getpid();
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int nl_cmd(int s, unsigned id, const unsigned char *pay, int plen)
{
    unsigned char msg[NLMSG_HDRLEN + 5 + MAXPAY];
    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    struct sockaddr_nl kernel;
    int total = NLMSG_HDRLEN + 5 + plen;

    memset(msg, 0, sizeof(msg));
    nlh->nlmsg_len = total;
    nlh->nlmsg_pid = getpid();
    msg[NLMSG_HDRLEN] = (unsigned char)id;
    msg[NLMSG_HDRLEN + 1] = plen & 0xff;
    msg[NLMSG_HDRLEN + 2] = (plen >> 8) & 0xff;
    if (plen)
        memcpy(msg + NLMSG_HDRLEN + 5, pay, plen);
    memset(&kernel, 0, sizeof(kernel));
    kernel.nl_family = AF_NETLINK;
    return sendto(s, msg, total, 0, (struct sockaddr *)&kernel, sizeof(kernel)) < 0 ? -1 : 0;
}

/* wait up to ms for one message; returns payload length or 0 */
static int nl_wait(int s, unsigned char *out, int max, int ms)
{
    unsigned char rx[4096];
    struct timeval tv;
    fd_set rf;
    int n;

    FD_ZERO(&rf);
    FD_SET(s, &rf);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(s + 1, &rf, NULL, NULL, &tv) <= 0)
        return 0;
    n = recv(s, rx, sizeof(rx), 0) - NLMSG_HDRLEN;
    if (n <= 0)
        return 0;
    if (n > max)
        n = max;
    memcpy(out, rx + NLMSG_HDRLEN, n);
    return n;
}

static void conf_get(const char *path, const char *key, char *out, int max)
{
    char line[256];
    FILE *f = fopen(path, "r");
    size_t kl = strlen(key);

    out[0] = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, key, kl) == 0 && line[kl] == '=')
            snprintf(out, max, "%s", line + kl + 1);
    }
    fclose(f);
}

static int run(const char *fmt, ...)
{
    char cmd[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    printf("+ %s\n", cmd);
    return system(cmd);
}

static int nl_query(int s, unsigned id, const unsigned char *pay, int plen,
                    unsigned char *r, int max);

/* is the byte string needle somewhere in hay? */
static int has_bytes(const unsigned char *hay, int hlen, const char *needle)
{
    int nlen = (int)strlen(needle), i;

    for (i = 0; nlen > 0 && i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

static int do_up(const char *conf)
{
    unsigned char pay[134], r[256], dns[4] = {0}, off = 0;
    char ssid[65], pass[65], wdg[16], dnsconf[32];
    const unsigned char *ip, *mk, *gw, *mac;
    unsigned char st[256];
    int s, n, t, have = 0;

    conf_get(conf, "ssid", ssid, sizeof(ssid));
    conf_get(conf, "pass", pass, sizeof(pass));
    conf_get(conf, "wdg", wdg, sizeof(wdg));
    conf_get(conf, "dns", dnsconf, sizeof(dnsconf));
    if (!ssid[0]) {
        fprintf(stderr, "no ssid= line in %s\n", conf);
        return 2;
    }
    s = nl_open();
    if (s < 0) {
        perror("netlink 28");
        return 1;
    }

    /* the chip's SoC watchdog is armed at power-up and resets the SoC unless
       fed (0x97); nothing on thingino feeds it yet, so switch it off unless
       the config says wdg=keep */
    if (strcmp(wdg, "keep") != 0) {
        nl_cmd(s, 0x94, &off, 1);
        usleep(200000);
        printf("watchdog: off\n");
    }

    /* The chip stays associated while the SoC sleeps or reboots. If it is
       already on the configured network and holds an address, use that:
       a second connect request on top of a live connection can leave the
       chip ignoring connects until it is told to disconnect. */
    n = nl_query(s, 0x8c, NULL, 0, st, sizeof(st));
    if (n > 0 && has_bytes(st, n, ssid)) {
        n = nl_query(s, 0x81, NULL, 0, r, sizeof(r));
        if (n >= 23 && r[5]) {
            have = 1;
            printf("already connected to \"%s\"\n", ssid);
        }
    }

    if (!have) {
        /* start from a clean state, then connect */
        nl_cmd(s, 0x8b, NULL, 0);
        sleep(2);
        memset(pay, 0, sizeof(pay));
        snprintf((char *)pay, 65, "%s", ssid);
        if (pass[0]) {
            snprintf((char *)pay + 65, 65, "%s", pass);
            pay[130] = 4;
        }
        nl_cmd(s, 0x8a, pay, sizeof(pay));
        printf("connect sent for \"%s\"\n", ssid);
    }

    /* poll get_ip_info (0x81) until the chip reports an address */
    for (t = 0; t < 60 && !have; t++) {
        int tries = 5;

        nl_cmd(s, 0x81, NULL, 0);
        while (tries-- > 0 && (n = nl_wait(s, r, sizeof(r), 200)) > 0) {
            if (n >= 23 && r[0] == 0x81 && r[5]) {
                have = 1;
                break;
            }
        }
        if (!have)
            sleep(1);
    }
    close(s);
    if (!have) {
        fprintf(stderr, "no address from the chip after 60 s\n");
        return 1;
    }
    ip = r + 5; mk = r + 9; gw = r + 13; mac = r + 17;
    /* the chip does not report a DNS server in a stable field; home routers
       serve DNS on the gateway address. Override with dns= in the config. */
    memcpy(dns, gw, 4);
    if (dnsconf[0])
        sscanf(dnsconf, "%hhu.%hhu.%hhu.%hhu", &dns[0], &dns[1], &dns[2], &dns[3]);

    run("ifconfig wlan0 down");
    run("ifconfig wlan0 hw ether %02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    run("ifconfig wlan0 %u.%u.%u.%u netmask %u.%u.%u.%u up",
        ip[0], ip[1], ip[2], ip[3], mk[0], mk[1], mk[2], mk[3]);
    run("route del default 2>/dev/null; route add default gw %u.%u.%u.%u",
        gw[0], gw[1], gw[2], gw[3]);
    run("echo nameserver %u.%u.%u.%u > /tmp/resolv.conf",
        dns[0], dns[1], dns[2], dns[3]);
    printf("wlan0 up: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

/* send one command and wait for the reply that echoes its id; unrelated
   messages from the chip are skipped. Returns the reply length or 0. */
static int nl_query(int s, unsigned id, const unsigned char *pay, int plen,
                    unsigned char *r, int max)
{
    int tries, n;

    if (nl_cmd(s, id, pay, plen) < 0)
        return 0;
    for (tries = 0; tries < 20; tries++) {
        n = nl_wait(s, r, max, 200);
        if (n > 0 && r[0] == (unsigned char)id)
            return n;
    }
    return 0;
}

/* ---- "status": wake reason, time powered, battery; shell-friendly ---- */
static int do_status(void)
{
    unsigned char r[256];
    int s = nl_open(), n, ok = 0;

    if (s < 0) {
        perror("netlink 28");
        return 1;
    }
    /* 0x82 get_wakeup_info: payload[0] = wake reason, u32 LE at +0x45 =
       milliseconds since the chip powered the SoC on */
    n = nl_query(s, 0x82, NULL, 0, r, sizeof(r));
    if (n >= 0x49) {
        printf("wake_reason=%u\n", r[5]);
        printf("soc_on_ms=%u\n", r[0x45] | (r[0x46] << 8) | (r[0x47] << 16) | ((unsigned)r[0x48] << 24));
        ok++;
    }
    /* 0x86 get_battery_status: charger flag, level %, voltage mV (LE) */
    n = nl_query(s, 0x86, NULL, 0, r, sizeof(r));
    if (n >= 10) {
        printf("charger=%u\n", r[5]);
        printf("battery_level=%u\n", r[6]);
        printf("battery_mv=%u\n", r[7] | (r[8] << 8));
        ok++;
    }
    close(s);
    return ok == 2 ? 0 : 1;
}

/* ---- "sleep": ask the chip to power the SoC off (0x8e, state 0). The chip
   stays associated and powers the SoC on again on a wake event. ---- */
static int do_sleep(void)
{
    unsigned char st = 0, on = 1;
    int s = nl_open();

    if (s < 0) {
        perror("netlink 28");
        return 1;
    }
    /* make sure the PIR can wake us: switch it on (0x87 01) before every
       sleep rather than relying on the chip having kept the setting */
    nl_cmd(s, 0x87, &on, 1);
    sleep(1);
    sync();
    nl_cmd(s, 0x8e, &st, 1);
    sleep(5);          /* power is normally gone before this returns */
    close(s);
    fprintf(stderr, "still running 5 s after the sleep command\n");
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char pay[MAXPAY], msg[NLMSG_HDRLEN + 5 + MAXPAY], rx[4096];
    struct sockaddr_nl me, kernel;
    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    unsigned id;
    int s, plen = 0, total, seconds = 3, got = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc >= 2 && strcmp(argv[1], "version") == 0) {
        printf("hinl 6 (reconnect, pir-on-sleep)\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "status") == 0)
        return do_status();
    if (argc >= 2 && strcmp(argv[1], "sleep") == 0)
        return do_sleep();
    if (argc >= 2 && strcmp(argv[1], "up") == 0)
        return do_up(argc >= 3 ? argv[2] : "/etc/hinl.conf");

    if (argc >= 3 && strcmp(argv[1], "wifi") == 0) {
        /* set_wifi_connect 0x8a: char ssid[65]; char pwd[65]; u32 auth (LE),
           auth = 4 with a password, 0 without (as addx_mcu_test does) */
        int haspwd = (argc >= 4 && strcmp(argv[3], "-") != 0 && argv[3][0]);
        id = 0x8a;
        plen = 134;
        memset(pay, 0, sizeof(pay));
        snprintf((char *)pay, 65, "%s", argv[2]);
        if (haspwd) {
            snprintf((char *)pay + 65, 65, "%s", argv[3]);
            pay[130] = 4;
        }
        if (argc >= 5)
            seconds = atoi(argv[4]);
    } else {
    if (argc < 3 || strcmp(argv[1], "cmd") != 0 || sscanf(argv[2], "%2x", &id) != 1) {
        fprintf(stderr, "usage: %s cmd <idhex> [payloadhex|-] [seconds]\n"
                        "       %s wifi <ssid> [password|-] [seconds]\n"
                        "       %s up [conffile]   (default /etc/hinl.conf: ssid=, pass=, wdg=keep)\n"
                        "       %s status          (wake reason, time powered, battery)\n"
                        "       %s sleep           (chip powers the SoC off until a wake event)\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    if (argc >= 4 && strcmp(argv[3], "-") != 0) {
        plen = hex2bin(argv[3], pay, sizeof(pay));
        if (plen < 0) {
            fprintf(stderr, "bad payload hex\n");
            return 2;
        }
    }
    if (argc >= 5)
        seconds = atoi(argv[4]);
    }

    s = socket(AF_NETLINK, SOCK_RAW, NL_CHANNEL);
    if (s < 0) {
        perror("socket(AF_NETLINK,28)");
        return 1;
    }
    memset(&me, 0, sizeof(me));
    me.nl_family = AF_NETLINK;
    me.nl_pid = getpid();
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        perror("bind");
        return 1;
    }

    /* netlink header + [cmd][len LE][payload]; no padding, the driver uses
       NLMSG_ALIGNTO 1 and takes payload length as nlmsg_len - 16 */
    memset(msg, 0, sizeof(msg));
    total = NLMSG_HDRLEN + 5 + plen;
    nlh->nlmsg_len = total;
    nlh->nlmsg_pid = getpid();
    msg[NLMSG_HDRLEN + 0] = (unsigned char)id;
    msg[NLMSG_HDRLEN + 1] = plen & 0xff;
    msg[NLMSG_HDRLEN + 2] = (plen >> 8) & 0xff;
    msg[NLMSG_HDRLEN + 3] = (plen >> 16) & 0xff;
    msg[NLMSG_HDRLEN + 4] = (plen >> 24) & 0xff;
    memcpy(msg + NLMSG_HDRLEN + 5, pay, plen);

    memset(&kernel, 0, sizeof(kernel));
    kernel.nl_family = AF_NETLINK;
    if (sendto(s, msg, total, 0, (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        perror("sendto");
        return 1;
    }
    printf(">> cmd 0x%02x, %d payload bytes, pid %d\n", id, plen, (int)getpid());

    for (;;) {
        struct timeval tv;
        fd_set rf;
        int n;

        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        if (select(s + 1, &rf, NULL, NULL, &tv) <= 0)
            break;
        n = recv(s, rx, sizeof(rx), 0);
        if (n <= NLMSG_HDRLEN)
            continue;
        got++;
        printf("<< reply %d: %d bytes\n", got, n - NLMSG_HDRLEN);
        dump(rx + NLMSG_HDRLEN, n - NLMSG_HDRLEN);
    }
    if (!got)
        printf("(no reply within %d s)\n", seconds);
    close(s);
    return got ? 0 : 1;
}
