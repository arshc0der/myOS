/*
 * keyboard.c - PS/2 keyboard driver (polling, scancode set 1).
 *
 * keyboard_getchar() waits for a key and returns it:
 *   - normal keys return their ASCII character
 *   - arrows, Page Up/Down, Home, End, Delete return KEY_* codes (keys.h)
 *
 * Extra keys such as the arrows send TWO bytes: 0xE0 first, then the code.
 * A key release sends the same code with bit 7 (0x80) set.
 */

#include <stdint.h>
#include "keys.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static const char normal[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3',
    [0x05]='4', [0x06]='5', [0x07]='6',
    [0x08]='7', [0x09]='8', [0x0A]='9',
    [0x0B]='0', [0x0C]='-', [0x0D]='=',
    [0x0E]='\b', [0x0F]='\t',

    [0x10]='q', [0x11]='w', [0x12]='e',
    [0x13]='r', [0x14]='t', [0x15]='y',
    [0x16]='u', [0x17]='i', [0x18]='o',
    [0x19]='p', [0x1A]='[', [0x1B]=']',
    [0x1C]='\n',

    [0x1E]='a', [0x1F]='s', [0x20]='d',
    [0x21]='f', [0x22]='g', [0x23]='h',
    [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`',
    [0x2B]='\\',

    [0x2C]='z', [0x2D]='x', [0x2E]='c',
    [0x2F]='v', [0x30]='b', [0x31]='n',
    [0x32]='m', [0x33]=',', [0x34]='.',
    [0x35]='/', [0x39]=' '
};

static const char shifted[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#',
    [0x05]='$', [0x06]='%', [0x07]='^',
    [0x08]='&', [0x09]='*', [0x0A]='(',
    [0x0B]=')', [0x0C]='_', [0x0D]='+',
    [0x0E]='\b', [0x0F]='\t',

    [0x10]='Q', [0x11]='W', [0x12]='E',
    [0x13]='R', [0x14]='T', [0x15]='Y',
    [0x16]='U', [0x17]='I', [0x18]='O',
    [0x19]='P', [0x1A]='{', [0x1B]='}',
    [0x1C]='\n',

    [0x1E]='A', [0x1F]='S', [0x20]='D',
    [0x21]='F', [0x22]='G', [0x23]='H',
    [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|',

    [0x2C]='Z', [0x2D]='X', [0x2E]='C',
    [0x2F]='V', [0x30]='B', [0x31]='N',
    [0x32]='M', [0x33]='<', [0x34]='>',
    [0x35]='?', [0x39]=' '
};

int keyboard_getchar(void)
{
    static int left_shift = 0;
    static int right_shift = 0;
    static int caps_lock = 0;
    static int extended = 0;        /* last byte was the 0xE0 prefix */

    for (;;) {
        /* Wait until the keyboard controller has data. */
        while (!(inb(0x64) & 1))
            __asm__ volatile ("pause");

        uint8_t sc = inb(0x60);

        if (sc == 0xE0) {           /* prefix for extended keys */
            extended = 1;
            continue;
        }

        int release = sc & 0x80;
        uint8_t code = sc & 0x7F;

        if (extended) {
            extended = 0;

            if (release)
                continue;

            switch (code) {
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x49: return KEY_PGUP;
            case 0x51: return KEY_PGDN;
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            case 0x53: return KEY_DELETE;
            default:   continue;    /* other extended keys: ignore */
            }
        }

        if (code == 0x2A) {         /* left shift  */
            left_shift = !release;
            continue;
        }

        if (code == 0x36) {         /* right shift */
            right_shift = !release;
            continue;
        }

        if (release)
            continue;

        if (code == 0x3A) {         /* caps lock toggles on key press */
            caps_lock = !caps_lock;
            continue;
        }

        char c = (left_shift || right_shift) ? shifted[code]
                                             : normal[code];

        /* Caps Lock flips the case of letters (Shift flips it back). */
        if (caps_lock) {
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            else if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
        }

        /* Ignore unmapped scan codes. */
        if (c != '\0')
            return (unsigned char)c;
    }
}
