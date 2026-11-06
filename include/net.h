/*
 * net.h - networking: network cards, a small IPv4 stack, Wi-Fi and Bluetooth adapters.
 *
 *   NIC drivers (e1000, virtio-net) register a struct netdev.
 *   net_up() finds a card, starts the stack and asks the router for an address (DHCP).
 *   net_ping() / net_resolve() / net_udp_send() are the services on top.
 *
 * Supported: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, DNS (A records). No TCP yet.
 * Wi-Fi and Bluetooth: see struct wifi_adapter / bt_adapter - the registries exist so a driver
 * can be added; MyOS has no Wi-Fi or Bluetooth hardware driver (needs vendor firmware / HCI).
 * See docs/NETWORK.md.
 */
#ifndef NET_H
#define NET_H
#include "types.h"

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

struct netdev {
    char name[8];                       /* "eth0" */
    const char *driver;                 /* "e1000", "virtio-net" */
    uint8_t mac[6];
    bool link;                          /* cable / virtual link is up */
    int (*send)(struct netdev *d, const void *frame, size_t len);   /* 0 on success */
    int (*poll)(struct netdev *d, void *buf, size_t max);           /* length of one received frame, 0 = none */
    void *priv;
    /* filled in by the stack */
    uint32_t ip, mask, gw, dns;         /* host byte order */
    uint32_t rx_frames, tx_frames;
};

int  net_register(struct netdev *d);
struct netdev *net_default(void);
int  net_probe(void);                   /* look for supported cards (PCI); returns how many */

/* Bring the network up: probe, start the stack, DHCP. Returns 0 when an address was obtained. */
int  net_up(void);
void net_start_async(void);             /* same, in a background task (used by the desktop) */
bool net_is_up(void);                   /* has an IP address */
bool net_busy(void);                    /* net_up is running */
const char *net_status_text(void);      /* one line for menus */
void net_poll(void);                    /* process received frames (called by the net task) */

/* Services. Addresses are host byte order. */
int  net_ping(uint32_t ip, int timeout_ms);                 /* round trip in ms, or -1 */
uint32_t net_resolve(const char *name);                     /* DNS; 0 = not found */
int  net_udp_send(uint32_t ip, uint16_t sport, uint16_t dport, const void *data, size_t len);
/* Receive hook: called for every UDP datagram to `port` (from the net task). */
int  net_udp_listen(uint16_t port, void (*cb)(uint32_t src_ip, uint16_t src_port, const void *data, size_t len));
void net_format_ip(uint32_t ip, char *out, size_t n);
bool net_parse_ip(const char *s, uint32_t *ip);

void net_set_enabled(bool on);                       /* Ethernet switch (Control Center) */
bool net_enabled(void);
void wifi_set_enabled(bool on);
bool wifi_enabled(void);
void bt_set_enabled(bool on);
bool bt_enabled(void);

/* TCP client (one connection at a time) and plain HTTP */
int  net_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);    /* 0 or -1 */
int  net_tcp_send(const void *data, size_t len);
int  net_tcp_recv(void *buf, size_t max, int timeout_ms);            /* >0 bytes, 0 closed by peer, -1 timeout/reset */
void net_tcp_close(void);
int  net_http_get(const char *host, uint16_t port, const char *path, char *out, size_t max, int *status);

/* NIC drivers (called by net_probe) */
int e1000_probe(void);
int virtio_net_probe(void);

/* ---- Wi-Fi ---- */
struct wifi_ap { char ssid[33]; int signal; bool secured; };    /* signal 0..100 */
struct wifi_adapter {
    const char *name;
    int  (*scan)(struct wifi_adapter *a, struct wifi_ap *out, int max);          /* number found */
    int  (*connect)(struct wifi_adapter *a, const char *ssid, const char *password);
    void (*disconnect)(struct wifi_adapter *a);
    const char *(*status)(struct wifi_adapter *a);                               /* "connected to X" ... */
    void *priv;
};
int  wifi_register(struct wifi_adapter *a);
int  wifi_count(void);
struct wifi_adapter *wifi_get(int i);
/* Wi-Fi radios found on the PCI bus (no driver): fills `names`, returns count */
int  wifi_detect_hardware(char names[][40], int max);

/* ---- Bluetooth ---- */
struct bt_device { char name[32]; uint8_t addr[6]; bool paired, connected; };
struct bt_adapter {
    const char *name;
    int  (*scan)(struct bt_adapter *a, struct bt_device *out, int max);
    int  (*pair)(struct bt_adapter *a, const uint8_t addr[6]);
    int  (*connect)(struct bt_adapter *a, const uint8_t addr[6]);
    void *priv;
};
int  bt_register(struct bt_adapter *a);
int  bt_count(void);
struct bt_adapter *bt_get(int i);
int  bt_detect_hardware(char names[][40], int max);     /* USB Bluetooth dongles (class 0xE0), no driver */
#endif
