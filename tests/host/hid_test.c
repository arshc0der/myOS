/* Host test for src/drivers/usb/hid.c: feeds fake USB reports and prints the events. */
#include "usb.h"
#include "input.h"

int printf(const char *, ...);          /* declared by hand: libc headers clash with the kernel types */

static uint32_t now;
uint32_t timer_ticks(void) { return now; }
static struct input_event got[64];
static int ngot;
void input_push(const struct input_event *ev) { got[ngot++] = *ev; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const uint8_t tablet_desc[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,
    0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x00,
    0x26,0xff,0x7f,0x35,0x00,0x46,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,0x05,0x01,0x09,0x38,0x15,0x81,
    0x25,0x7f,0x35,0x00,0x45,0x00,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0 };

int main(void)
{
    struct hid_dev k, m, t;

    hid_setup(&k, HID_KEYBOARD, NULL, 0);
    uint8_t r1[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };               /* 'a' */
    hid_report(&k, r1, 8);
    CHECK(ngot == 1 && got[0].key == 'a' && got[0].mods == 0);
    uint8_t r2[8] = { 0x02, 0, 0x04, 0x1E, 0, 0, 0, 0 };         /* shift + a held, '1' pressed */
    hid_report(&k, r2, 8);
    CHECK(ngot == 2 && got[1].key == '!' && (got[1].mods & MOD_SHIFT));
    uint8_t r3[8] = { 0x01, 0, 0x06, 0, 0, 0, 0, 0 };            /* ctrl+c */
    ngot = 0;
    hid_report(&k, r3, 8);
    CHECK(ngot == 1 && got[0].key == 3 && (got[0].mods & MOD_CTRL));
    uint8_t r4[8] = { 0x04, 0, 0x14, 0, 0, 0, 0, 0 };            /* alt+q */
    ngot = 0;
    hid_report(&k, r4, 8);
    CHECK(ngot == 1 && got[0].key == 'q' && (got[0].mods & MOD_ALT));
    uint8_t r5[8] = { 0, 0, 0x28, 0, 0, 0, 0, 0 };               /* enter */
    ngot = 0;
    hid_report(&k, r5, 8);
    CHECK(got[0].key == '\n');
    now = 100;                                                   /* repeat after 400 ms */
    ngot = 0;
    hid_poll(&k, now + 41);
    CHECK(ngot == 0 || got[0].key == '\n');
    uint8_t r6[8] = { 0, 0, 0x50, 0, 0, 0, 0, 0 };               /* left arrow */
    ngot = 0;
    hid_report(&k, r6, 8);
    CHECK(got[0].key == KEY_LEFT);
    uint8_t r7[8] = { 0 };
    hid_report(&k, r7, 8);
    ngot = 0;
    hid_poll(&k, 100000);
    CHECK(ngot == 0);                                            /* released: no repeat */

    hid_setup(&m, HID_MOUSE, NULL, 0);
    uint8_t mr[4] = { 0x01, 5, (uint8_t)-3, 0 };
    ngot = 0;
    hid_report(&m, mr, 4);
    CHECK(ngot == 1 && got[0].dx == 5 && got[0].dy == -3 && got[0].buttons == 1 && !(got[0].pad & 1));

    hid_setup(&t, HID_NONE, tablet_desc, sizeof tablet_desc);
    CHECK(t.kind == HID_POINTER_ABS);
    uint8_t tr[6] = { 0x02, 0xFF, 0x3F, 0x00, 0x40, 0 };         /* x=16383 y=16384, right button */
    ngot = 0;
    hid_report(&t, tr, 6);
    CHECK(ngot == 1 && (got[0].pad & 1) && got[0].buttons == 2);
    CHECK(got[0].dx > 16000 && got[0].dx < 16500 && got[0].dy > 16000 && got[0].dy < 16600);
    printf("%s (%d failures)\n", fails ? "FAILED" : "all HID tests passed", fails);
    return fails != 0;
}
