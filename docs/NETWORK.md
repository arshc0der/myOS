# Networking

Drivers: e1000 (QEMU default) and virtio-net (UTM), polling only. Stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP,
DNS. No TCP. `net_up()` finds a card and runs DHCP; the desktop starts it on demand.
API: include/net.h (net_ping, net_resolve, net_udp_send, net_udp_listen).
Wi-Fi / Bluetooth: registries only (`wifi_register`, `bt_register`); implement scan/connect callbacks in a
driver and the Settings and Setup Assistant pages use them automatically. Hardware is only detected.
Shell: ifconfig-style commands in src/shell/cmd_net.c.

TCP (2.7.0): `net_tcp_connect/send/recv/close` and `net_http_get`; shell `wget http://host/path`.
Bluetooth: USB dongles are discovered by src/drivers/usb/btusb.c (scan only).
