/*
 * net.c - a small IPv4 stack: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP and DNS.
 *
 * One background task ("net") receives frames. Everything that waits (ARP, ping, DHCP, DNS)
 * sleeps and checks a flag that the receive task sets, so only one task ever touches the card's
 * receive ring. No TCP.
 */
#include "net.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "log.h"
#include "kstring.h"
#include "kprintf.h"
#include "kerror.h"

static struct netdev *dev;
static struct netdev *devs[4];
static int ndevs;
static bool probed, task_started, up, busy;
static struct mutex tx_lock = MUTEX_INIT(tx_lock);

static uint8_t rxbuf[2048], txbuf[2048];

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

int net_register(struct netdev *d)
{
    if (ndevs >= 4)
        return -1;
    ksnprintf(d->name, sizeof d->name, "eth%d", ndevs);
    devs[ndevs++] = d;
    return 0;
}

struct netdev *net_default(void) { return dev; }

int net_probe(void)
{
    if (!probed) {
        probed = true;
        e1000_probe();
        virtio_net_probe();
        if (ndevs)
            dev = devs[0];
    }
    return ndevs;
}

void net_format_ip(uint32_t ip, char *out, size_t n)
{
    ksnprintf(out, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool net_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9')
            n = n * 10 + (uint32_t)(*s++ - '0');
        if (n > 255)
            return false;
        v = v << 8 | n;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip = v;
    return true;
}

static uint16_t checksum(const uint8_t *p, size_t len, uint32_t sum)
{
    for (; len > 1; len -= 2, p += 2)
        sum += (uint32_t)(p[0] << 8 | p[1]);
    if (len)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---------------------------------------------------------------- */
/* Ethernet and ARP                                                  */
/* ---------------------------------------------------------------- */

static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };

static int eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    if (!dev || len > 1500)
        return -1;
    mutex_lock(&tx_lock);
    memcpy(txbuf, dst, 6);
    memcpy(txbuf + 6, dev->mac, 6);
    put16(txbuf + 12, type);
    memcpy(txbuf + 14, payload, len);
    size_t total = 14 + len;
    if (total < 60) {
        memset(txbuf + total, 0, 60 - total);
        total = 60;
    }
    int rc = dev->send(dev, txbuf, total);
    if (rc == 0)
        dev->tx_frames++;
    mutex_unlock(&tx_lock);
    return rc;
}

struct arp_ent { uint32_t ip; uint8_t mac[6]; bool valid; };
static struct arp_ent arp[8];
static int arp_next;

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    for (int i = 0; i < 8; i++)
        if (arp[i].valid && arp[i].ip == ip) {
            memcpy(arp[i].mac, mac, 6);
            return;
        }
    arp[arp_next].ip = ip;
    memcpy(arp[arp_next].mac, mac, 6);
    arp[arp_next].valid = true;
    arp_next = (arp_next + 1) % 8;
}

static bool arp_find(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < 8; i++)
        if (arp[i].valid && arp[i].ip == ip) {
            memcpy(mac, arp[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_send(uint16_t op, const uint8_t *dmac, uint32_t dip)
{
    uint8_t p[28];
    put16(p, 1);            /* Ethernet */
    put16(p + 2, 0x0800);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, op);
    memcpy(p + 8, dev->mac, 6);
    put32(p + 14, dev->ip);
    memcpy(p + 18, dmac, 6);
    put32(p + 24, dip);
    eth_send(op == 1 ? bcast : dmac, 0x0806, p, 28);
}

static bool arp_resolve(uint32_t ip, uint8_t *mac)
{
    for (int tries = 0; tries < 4; tries++) {
        if (arp_find(ip, mac))
            return true;
        static const uint8_t zero[6];
        arp_send(1, zero, ip);
        for (int i = 0; i < 30; i++) {
            timer_sleep_ms(10);
            if (arp_find(ip, mac))
                return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* IPv4, ICMP, UDP                                                   */
/* ---------------------------------------------------------------- */

static uint16_t ip_id;

static int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len, bool use_zero_src)
{
    uint8_t pkt[1500], mac[6];
    if (!dev || len > 1480)
        return -1;
    if (dst == 0xFFFFFFFFu || (dev->mask && dst == (dev->ip | ~dev->mask))) {
        memcpy(mac, bcast, 6);
    } else {
        uint32_t next = (dev->mask && ((dst ^ dev->ip) & dev->mask) == 0) ? dst : dev->gw;
        if (!next || !arp_resolve(next, mac))
            return -2;
    }
    pkt[0] = 0x45;
    pkt[1] = 0;
    put16(pkt + 2, (uint16_t)(20 + len));
    put16(pkt + 4, ++ip_id);
    put16(pkt + 6, 0);
    pkt[8] = 64;
    pkt[9] = proto;
    put16(pkt + 10, 0);
    put32(pkt + 12, use_zero_src ? 0 : dev->ip);
    put32(pkt + 16, dst);
    put16(pkt + 10, checksum(pkt, 20, 0));
    memcpy(pkt + 20, payload, len);
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

static volatile uint16_t ping_seq_wait, ping_got;

struct listener { uint16_t port; void (*cb)(uint32_t, uint16_t, const void *, size_t); };
static struct listener listeners[6];

int net_udp_listen(uint16_t port, void (*cb)(uint32_t, uint16_t, const void *, size_t))
{
    for (int i = 0; i < 6; i++)
        if (!listeners[i].cb || listeners[i].port == port) {
            listeners[i].port = port;
            listeners[i].cb = cb;
            return 0;
        }
    return -1;
}

static int udp_send_raw(uint32_t ip, uint16_t sport, uint16_t dport, const void *data, size_t len, bool zero_src)
{
    uint8_t d[1480];
    if (len > 1400)
        return -1;
    put16(d, sport);
    put16(d + 2, dport);
    put16(d + 4, (uint16_t)(8 + len));
    put16(d + 6, 0);                                  /* checksum optional in IPv4 */
    memcpy(d + 8, data, len);
    return ip_send(ip, 17, d, 8 + len, zero_src);
}

int net_udp_send(uint32_t ip, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    return udp_send_raw(ip, sport, dport, data, len, false);
}

static void icmp_rx(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < 8)
        return;
    if (p[0] == 8) {                                  /* echo request: answer it */
        uint8_t r[1480];
        if (len > sizeof r)
            return;
        memcpy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, checksum(r, len, 0));
        ip_send(src, 1, r, len, false);
    } else if (p[0] == 0 && get16(p + 4) == 0x4D59) { /* echo reply to our ping */
        ping_got = get16(p + 6);
    }
}

static void udp_rx(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < 8)
        return;
    uint16_t sport = get16(p), dport = get16(p + 2), ulen = get16(p + 4);
    if (ulen < 8 || ulen > len)
        return;
    for (int i = 0; i < 6; i++)
        if (listeners[i].cb && listeners[i].port == dport)
            listeners[i].cb(src, sport, p + 8, (size_t)ulen - 8);
}

static void ip_rx(const uint8_t *p, size_t len)
{
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(p[0] & 15) * 4, tot = get16(p + 2);
    if (ihl < 20 || tot > len || tot < ihl)
        return;
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    bool mine = dst == dev->ip || dst == 0xFFFFFFFFu || (dev->mask && dst == (dev->ip | ~dev->mask)) || dev->ip == 0;
    if (!mine || (get16(p + 6) & 0x3FFF))             /* not for us, or a fragment */
        return;
    if (p[9] == 1)
        icmp_rx(src, p + ihl, tot - ihl);
    else if (p[9] == 17)
        udp_rx(src, p + ihl, tot - ihl);
}

static void arp_rx(const uint8_t *p, size_t len)
{
    if (len < 28 || get16(p) != 1 || get16(p + 2) != 0x0800)
        return;
    uint16_t op = get16(p + 6);
    uint32_t sip = get32(p + 14), tip = get32(p + 24);
    arp_learn(sip, p + 8);
    if (op == 1 && dev->ip && tip == dev->ip)
        arp_send(2, p + 8, sip);
}

void net_poll(void)
{
    if (!dev)
        return;
    for (int n = 0; n < 16; n++) {
        int len = dev->poll(dev, rxbuf, sizeof rxbuf);
        if (len < 14)
            return;
        dev->rx_frames++;
        uint16_t type = get16(rxbuf + 12);
        if (type == 0x0806)
            arp_rx(rxbuf + 14, (size_t)len - 14);
        else if (type == 0x0800)
            ip_rx(rxbuf + 14, (size_t)len - 14);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_poll();
        timer_sleep_ms(10);
    }
}

/* ---------------------------------------------------------------- */
/* DHCP                                                              */
/* ---------------------------------------------------------------- */

static volatile int dhcp_state;                        /* 0 idle, 1 discover sent, 2 request sent, 3 done */
static uint32_t dhcp_xid = 0x4D594F53, dhcp_offer, dhcp_server;
static uint32_t lease_mask, lease_gw, lease_dns;

static void dhcp_send(uint8_t type)
{
    uint8_t m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;
    put32(m + 4, dhcp_xid);
    put16(m + 10, 0x8000);                            /* ask for a broadcast answer */
    memcpy(m + 28, dev->mac, 6);
    put32(m + 236, 0x63825363);
    int o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = type;
    if (type == 3) {
        m[o++] = 50; m[o++] = 4; put32(m + o, dhcp_offer); o += 4;
        m[o++] = 54; m[o++] = 4; put32(m + o, dhcp_server); o += 4;
    }
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;
    m[o++] = 255;
    udp_send_raw(0xFFFFFFFFu, 68, 67, m, (size_t)o, true);
}

static void dhcp_rx(uint32_t src, uint16_t sport, const void *data, size_t len)
{
    const uint8_t *m = data;
    (void)src; (void)sport;
    if (len < 241 || m[0] != 2 || get32(m + 4) != dhcp_xid || get32(m + 236) != 0x63825363)
        return;
    uint8_t type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, server = 0;
    for (size_t o = 240; o + 1 < len && m[o] != 255;) {
        if (m[o] == 0) { o++; continue; }
        uint8_t code = m[o], l = m[o + 1];
        if (o + 2 + l > len)
            break;
        const uint8_t *v = m + o + 2;
        if (code == 53) type = v[0];
        else if (code == 1 && l >= 4) mask = get32(v);
        else if (code == 3 && l >= 4) gw = get32(v);
        else if (code == 6 && l >= 4) dns = get32(v);
        else if (code == 54 && l >= 4) server = get32(v);
        o += 2 + l;
    }
    if (type == 2 && dhcp_state == 1) {
        dhcp_offer = get32(m + 16);
        dhcp_server = server;
        dhcp_state = 2;
        dhcp_send(3);
    } else if (type == 5 && dhcp_state == 2) {
        lease_mask = mask; lease_gw = gw; lease_dns = dns;
        dev->ip = get32(m + 16);
        dev->mask = mask ? mask : 0xFFFFFF00u;
        dev->gw = gw;
        dev->dns = dns;
        dhcp_state = 3;
    }
}

static int dhcp_run(void)
{
    net_udp_listen(68, dhcp_rx);
    dev->ip = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        dhcp_state = 1;
        dhcp_send(1);
        for (int i = 0; i < 100 && dhcp_state != 3; i++) {
            timer_sleep_ms(30);
            if (dhcp_state == 2 && i % 30 == 29)
                dhcp_send(3);
        }
        if (dhcp_state == 3)
            return 0;
    }
    dhcp_state = 0;
    return -1;
}

/* ---------------------------------------------------------------- */
/* Bring-up, ping, DNS                                               */
/* ---------------------------------------------------------------- */

int net_up(void)
{
    if (up)
        return 0;
    if (busy)
        return -EBUSY;
    busy = true;
    if (net_probe() == 0 || !dev) {
        busy = false;
        return -ENODEV;
    }
    if (!task_started) {
        task_started = true;
        task_create("net", net_task, NULL);
    }
    int rc = dhcp_run();
    if (rc == 0) {
        char a[20];
        net_format_ip(dev->ip, a, sizeof a);
        log_info("net", "%s (%s) got address %s", dev->name, dev->driver, a);
        up = true;
    } else {
        log_warn("net", "no DHCP answer on %s", dev->name);
    }
    busy = false;
    return rc;
}

static void netup_task(void *arg)
{
    (void)arg;
    net_up();
}

void net_start_async(void)
{
    if (!up && !busy)
        task_create("netup", netup_task, NULL);
}

bool net_is_up(void) { return up; }
bool net_busy(void) { return busy; }

const char *net_status_text(void)
{
    static char b[64];
    char a[20];
    if (busy)
        return "Network: connecting...";
    if (up && dev) {
        net_format_ip(dev->ip, a, sizeof a);
        ksnprintf(b, sizeof b, "Network: %s (%s)", a, dev->driver);
        return b;
    }
    if (probed && !ndevs)
        return "Network: no card found";
    if (probed && ndevs)
        return "Network: no address";
    return "Network: off";
}

int net_ping(uint32_t ip, int timeout_ms)
{
    uint8_t d[40];
    static uint16_t seq;
    if (!up || !dev)
        return -1;
    memset(d, 0, sizeof d);
    d[0] = 8;
    put16(d + 4, 0x4D59);
    put16(d + 6, ++seq);
    for (int i = 8; i < 40; i++)
        d[i] = (uint8_t)i;
    put16(d + 2, checksum(d, 40, 0));
    ping_got = 0;
    uint32_t t0 = timer_ticks();
    if (ip_send(ip, 1, d, 40, false) < 0)
        return -1;
    while ((int)(timer_ticks() - t0) * 10 < timeout_ms) {
        if (ping_got == seq)
            return (int)(timer_ticks() - t0) * 10;
        timer_sleep_ms(5);
    }
    return -1;
}

static volatile uint32_t dns_answer;

static void dns_rx(uint32_t src, uint16_t sport, const void *data, size_t len)
{
    const uint8_t *m = data;
    (void)src; (void)sport;
    if (len < 12 || get16(m) != 0x4D59 || !(m[2] & 0x80) || (m[3] & 15))
        return;
    int qd = get16(m + 4), an = get16(m + 6);
    size_t o = 12;
    for (int i = 0; i < qd && o < len; i++) {            /* skip the question(s) */
        while (o < len && m[o]) o += (m[o] & 0xC0) == 0xC0 ? 1 : (size_t)m[o] + 1;
        o += (o < len && (m[o] & 0xC0) == 0xC0) ? 1 : 1;
        o += 4;
    }
    for (int i = 0; i < an && o + 12 <= len; i++) {
        if ((m[o] & 0xC0) == 0xC0) o += 2;
        else { while (o < len && m[o]) o += (size_t)m[o] + 1; o++; }
        if (o + 10 > len) return;
        uint16_t type = get16(m + o), rdlen = get16(m + o + 8);
        o += 10;
        if (type == 1 && rdlen == 4 && o + 4 <= len) {
            dns_answer = get32(m + o);
            return;
        }
        o += rdlen;
    }
}

uint32_t net_resolve(const char *name)
{
    uint32_t ip;
    uint8_t q[300];
    if (net_parse_ip(name, &ip))
        return ip;
    if (!up || !dev->dns)
        return 0;
    size_t o = 12;
    memset(q, 0, sizeof q);
    put16(q, 0x4D59);
    put16(q + 2, 0x0100);                                /* recursion desired */
    put16(q + 4, 1);
    for (const char *p = name; *p;) {                    /* www.example.com -> 3www7example3com0 */
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63 || o + l + 6 > sizeof q)
            return 0;
        q[o++] = (uint8_t)l;
        memcpy(q + o, p, l);
        o += l;
        p += l + (dot ? 1 : 0);
    }
    q[o++] = 0;
    put16(q + o, 1); put16(q + o + 2, 1);                /* type A, class IN */
    o += 4;
    dns_answer = 0;
    net_udp_listen(53053, dns_rx);
    for (int tries = 0; tries < 3 && !dns_answer; tries++) {
        net_udp_send(dev->dns, 53053, 53, q, o);
        for (int i = 0; i < 100 && !dns_answer; i++)
            timer_sleep_ms(10);
    }
    return dns_answer;
}
