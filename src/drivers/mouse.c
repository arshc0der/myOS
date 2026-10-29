/*
 * mouse.c - PS/2 mouse. The mouse sends 3-byte packets on IRQ 12:
 *   byte 0: buttons + sign bits,  byte 1: x movement,  byte 2: y movement.
 *
 * Note: some virtual machines (for example UTM's default USB tablet) do not
 * provide a PS/2 mouse. The GUI can then be used with the keyboard alone.
 */
#include "input.h"
#include "isr.h"
#include "io.h"
#include "log.h"

#define KBC_DATA   0x60
#define KBC_STATUS 0x64
#define KBC_CMD    0x64

static bool present;
static uint8_t packet[3];
static int idx;
static uint8_t last_buttons;

static bool wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(KBC_STATUS) & 2))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(KBC_STATUS) & 1)
            return true;
    return false;
}

static bool mouse_cmd(uint8_t c)
{
    if (!wait_write())
        return false;
    outb(KBC_CMD, 0xD4);                        /* next byte goes to the mouse */
    if (!wait_write())
        return false;
    outb(KBC_DATA, c);
    if (!wait_read())
        return false;
    return inb(KBC_DATA) == 0xFA;               /* ACK */
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    uint8_t b = inb(KBC_DATA);

    if (idx == 0 && !(b & 0x08))                /* out of sync: bit 3 is always 1 */
        return;
    packet[idx++] = b;
    if (idx < 3)
        return;
    idx = 0;
    if (packet[0] & 0xC0)                       /* overflow */
        return;

    int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    struct input_event ev = {
        .type = INPUT_MOUSE,
        .buttons = packet[0] & 7,
        .dx = dx,
        .dy = -dy,                              /* screen y grows downwards */
    };
    last_buttons = ev.buttons;
    input_push(&ev);
}

bool mouse_init(void)
{
    uint8_t cfg;

    for (int i = 0; i < 64 && (inb(KBC_STATUS) & 1); i++)
        (void)inb(KBC_DATA);                    /* drop stale bytes */
    if (!wait_write()) return false;
    outb(KBC_CMD, 0xA8);                        /* enable the aux (mouse) port */
    if (!wait_write()) return false;
    outb(KBC_CMD, 0x20);                        /* read controller config */
    if (!wait_read()) return false;
    cfg = inb(KBC_DATA);
    cfg |= 0x02;                                /* enable IRQ 12 */
    cfg &= (uint8_t)~0x20;                      /* enable mouse clock */
    if (!wait_write()) return false;
    outb(KBC_CMD, 0x60);
    if (!wait_write()) return false;
    outb(KBC_DATA, cfg);

    (void)mouse_cmd(0xFF);                      /* reset (answer may be longer) */
    for (int i = 0; i < 64 && (inb(KBC_STATUS) & 1); i++)
        (void)inb(KBC_DATA);
    (void)mouse_cmd(0xF6);                      /* defaults */
    if (!mouse_cmd(0xF4) && !mouse_cmd(0xF4))   /* start streaming */
        return false;
    idx = 0;
    irq_register(12, mouse_irq);
    present = true;
    return true;
}

bool mouse_present(void) { return present; }
