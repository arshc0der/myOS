# Host-side unit tests (run on your normal computer, not inside MyOS):  make -C tests/host
all:
	cc -w -std=gnu11 -I../../include hid_test.c ../../src/drivers/usb/hid.c -o /tmp/myos_hid_test && /tmp/myos_hid_test
