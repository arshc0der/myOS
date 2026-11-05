/* cmd_net.c - shell commands: net, ping, nslookup, wifi, bluetooth. */
#include "shell.h"
#include "net.h"
#include "kprintf.h"
#include "kstring.h"
#include "kerror.h"

static int cmd_net(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "up")) {
        kprintf("starting the network (DHCP can take a few seconds)...\n");
        int rc = net_up();
        if (rc < 0) {
            kprintf("net: %s\n", rc == -ENODEV ? "no supported network card (e1000 or virtio-net)" : "no DHCP answer");
            return 1;
        }
    }
    struct netdev *d = net_default();
    kprintf("  %s\n", net_status_text());
    if (d) {
        char a[20], m[20], g[20], n[20];
        net_format_ip(d->ip, a, sizeof a);
        net_format_ip(d->mask, m, sizeof m);
        net_format_ip(d->gw, g, sizeof g);
        net_format_ip(d->dns, n, sizeof n);
        kprintf("  %s  %s  mac %02x:%02x:%02x:%02x:%02x:%02x  link %s\n", d->name, d->driver, d->mac[0], d->mac[1], d->mac[2],
                d->mac[3], d->mac[4], d->mac[5], d->link ? "up" : "down");
        kprintf("  address %s  mask %s  gateway %s  dns %s\n  frames: %u received, %u sent\n", a, m, g, n, d->rx_frames, d->tx_frames);
    } else
        kprintf("  usage: net up   (probes the card and asks the router for an address)\n");
    return 0;
}
SHELL_COMMAND("net", "net [up]", "Network status; \"net up\" connects with DHCP", cmd_net);

static int cmd_ping(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: ping <address or name>\n"); return 1; }
    if (!net_is_up() && net_up() < 0) { kprintf("ping: network is not up (try \"net up\")\n"); return 1; }
    uint32_t ip = net_resolve(argv[1]);
    char a[20];
    if (!ip) { kprintf("ping: cannot resolve %s\n", argv[1]); return 1; }
    net_format_ip(ip, a, sizeof a);
    for (int i = 0; i < 4; i++) {
        int ms = net_ping(ip, 2000);
        if (ms >= 0)
            kprintf("reply from %s: time=%d ms\n", a, ms);
        else
            kprintf("request to %s timed out\n", a);
    }
    return 0;
}
SHELL_COMMAND("ping", "ping <host>", "Send ICMP echo requests", cmd_ping);

static int cmd_nslookup(int argc, char **argv)
{
    char a[20];
    if (argc < 2) { kprintf("usage: nslookup <name>\n"); return 1; }
    if (!net_is_up() && net_up() < 0) { kprintf("nslookup: network is not up\n"); return 1; }
    uint32_t ip = net_resolve(argv[1]);
    if (!ip) { kprintf("not found\n"); return 1; }
    net_format_ip(ip, a, sizeof a);
    kprintf("%s has address %s\n", argv[1], a);
    return 0;
}
SHELL_COMMAND("nslookup", "nslookup <name>", "Look up an IPv4 address with DNS", cmd_nslookup);

static int cmd_wifi(int argc, char **argv)
{
    char hw[4][40];
    (void)argc; (void)argv;
    if (!wifi_count()) {
        int n = wifi_detect_hardware(hw, 4);
        kprintf("  no Wi-Fi driver (see docs/NETWORK.md). Radios found: %d\n", n);
        for (int i = 0; i < n; i++)
            kprintf("    %s\n", hw[i]);
        return 0;
    }
    struct wifi_adapter *a = wifi_get(0);
    struct wifi_ap ap[8];
    int n = a->scan ? a->scan(a, ap, 8) : 0;
    kprintf("  %s: %s\n", a->name, a->status ? a->status(a) : "ready");
    for (int i = 0; i < n; i++)
        kprintf("    %-32s %3d%% %s\n", ap[i].ssid, ap[i].signal, ap[i].secured ? "secured" : "open");
    if (argc >= 3 && a->connect)
        return a->connect(a, argv[1], argv[2]) < 0;
    kprintf("  connect with: wifi <ssid> <password>\n");
    return 0;
}
SHELL_COMMAND("wifi", "wifi [ssid password]", "List Wi-Fi networks / connect (needs a driver)", cmd_wifi);

static int cmd_bluetooth(int argc, char **argv)
{
    char hw[4][40];
    (void)argc; (void)argv;
    if (!bt_count()) {
        int n = bt_detect_hardware(hw, 4);
        kprintf("  no Bluetooth driver (see docs/NETWORK.md). Adapters found: %d\n", n);
        for (int i = 0; i < n; i++)
            kprintf("    %s\n", hw[i]);
        return 0;
    }
    struct bt_adapter *a = bt_get(0);
    struct bt_device d[8];
    int n = a->scan ? a->scan(a, d, 8) : 0;
    for (int i = 0; i < n; i++)
        kprintf("    %-24s %02x:%02x:%02x:%02x:%02x:%02x %s\n", d[i].name, d[i].addr[0], d[i].addr[1], d[i].addr[2],
                d[i].addr[3], d[i].addr[4], d[i].addr[5], d[i].connected ? "connected" : d[i].paired ? "paired" : "");
    return 0;
}
SHELL_COMMAND("bluetooth", "bluetooth", "List Bluetooth devices (needs a driver)", cmd_bluetooth);

static int cmd_wget(int argc, char **argv)
{
    static char page[8192];
    char host[64], path[128] = "/";
    const char *u;
    int port = 80, status = 0, n, i = 0;

    if (argc < 2) { kprintf("usage: wget http://host[:port]/path\n"); return 1; }
    u = argv[1];
    if (!strncmp(u, "http://", 7)) u += 7;
    while (*u && *u != '/' && *u != ':' && i < 63) host[i++] = *u++;
    host[i] = 0;
    if (*u == ':') { port = katoi(u + 1); while (*u && *u != '/') u++; }
    if (*u) strlcpy(path, u, sizeof path);
    if (!net_is_up() && net_up() < 0) { kprintf("wget: network is not up\n"); return 1; }
    n = net_http_get(host, (uint16_t)port, path, page, sizeof page, &status);
    if (n < 0) { kprintf("wget: could not fetch %s\n", argv[1]); return 1; }
    kprintf("HTTP %d, %d bytes\n%s\n", status, n, page);
    return 0;
}
SHELL_COMMAND("wget", "wget http://host[:port]/path", "Fetch a web page over HTTP (no HTTPS) and print it", cmd_wget);
