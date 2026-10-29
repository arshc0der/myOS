/*
 * rtc.c - reads the date and time from the CMOS real-time clock (RTC).
 *
 * The RTC is a small battery-backed clock chip. To read one of its
 * registers, write the register number to port 0x70 and read the value
 * from port 0x71.
 */

#include "hw.h"
#include "io.h"

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t from_bcd(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

static void read_raw(uint8_t r[6])
{
    /* Wait while the clock is in the middle of updating itself. */
    while (cmos_read(0x0A) & 0x80)
        ;

    r[0] = cmos_read(0x09);     /* year (2 digits) */
    r[1] = cmos_read(0x08);     /* month           */
    r[2] = cmos_read(0x07);     /* day             */
    r[3] = cmos_read(0x04);     /* hour            */
    r[4] = cmos_read(0x02);     /* minute          */
    r[5] = cmos_read(0x00);     /* second          */
}

/*
 * Fills t[] with: year, month, day, hour, minute, second.
 * The RTC normally holds UTC time (in virtual machines it follows the host).
 */
void rtc_read(uint32_t t[6])
{
    uint8_t a[6], b[6];

    /* Read twice and compare, so we never use a half-updated value. */
    do {
        read_raw(a);
        read_raw(b);
    } while (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] ||
             a[3] != b[3] || a[4] != b[4] || a[5] != b[5]);

    uint8_t status = cmos_read(0x0B);
    int binary    = status & 0x04;      /* otherwise values are BCD  */
    int hour24    = status & 0x02;      /* otherwise 12-hour clock   */
    int pm        = a[3] & 0x80;

    if (!binary) {
        for (int i = 0; i < 6; i++) {
            if (i == 3)
                a[i] &= 0x7F;           /* strip the PM bit first    */
            a[i] = from_bcd(a[i]);
        }
    } else {
        a[3] &= 0x7F;
    }

    if (!hour24) {
        if (pm)
            a[3] = (uint8_t)((a[3] % 12) + 12);
        else if (a[3] == 12)
            a[3] = 0;
    }

    t[0] = 2000u + a[0];                /* assume the 21st century   */
    t[1] = a[1];
    t[2] = a[2];
    t[3] = a[3];
    t[4] = a[4];
    t[5] = a[5];
}
