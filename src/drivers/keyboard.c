/*
 * keyboard.c - PS/2 keyboard (scancode set 1) -> input events.
 *
 * Each key press and release sends a "scancode" byte on IRQ 1. Keys in the
 * grey navigation block send 0xE0 first. A release has bit 7 set.
 */
#include "input.h"
#include "isr.h"
#include "io.h"
#include "kerror.h"

static const char normal[0x59] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0, '*',
    0, ' ',
};
static const char shifted[0x59] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0, '*',
    0, ' ',
};

static bool shift, ctrl, alt, caps, extended;

static int nav_key(uint8_t sc)
{
    switch (sc) {
    case 0x48: return KEY_UP;     case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;   case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;   case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;   case 0x51: return KEY_PGDN;
    case 0x53: return KEY_DELETE; case 0x52: return KEY_INSERT;
    }
    return 0;
}

static void emit(int key)
{
    struct input_event ev = { .type = INPUT_KEY, .key = key };

    ev.mods = (uint8_t)((shift ? MOD_SHIFT : 0) | (ctrl ? MOD_CTRL : 0) |
                        (alt ? MOD_ALT : 0));
    input_push(&ev);
}

static void kbd_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        extended = true;
        return;
    }
    bool release = sc & 0x80;
    sc &= 0x7F;
    bool ext = extended;
    extended = false;

    switch (sc) {
    case 0x2A: case 0x36: if (!ext) { shift = !release; return; } break;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release)
        return;

    int key = 0;
    if (ext || (sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4E)) {
        key = nav_key(sc);
        if (!key && ext && sc == 0x1C)
            key = '\n';                         /* keypad enter */
        if (!key && ext && sc == 0x35)
            key = '/';
    } else if (sc >= 0x3B && sc <= 0x44) {
        key = KEY_F(sc - 0x3B + 1);
    } else if (sc == 0x57 || sc == 0x58) {
        key = KEY_F(sc - 0x57 + 11);
    } else if (sc < sizeof normal) {
        char c = shift ? shifted[sc] : normal[sc];
        if (caps && c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        else if (caps && c >= 'A' && c <= 'Z' && shift)
            c = (char)(c + 32);
        if (ctrl && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);            /* Ctrl+A = 1 ... */
        else if (ctrl && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        key = (unsigned char)c;
    }
    if (key)
        emit(key);
}

void keyboard_init(void)
{
    while (inb(0x64) & 1)                       /* drain old bytes */
        inb(0x60);
    irq_register(1, kbd_irq);
}

int kbd_getchar(void)
{
    struct input_event ev;

    for (;;) {
        input_wait(&ev, 0);
        if (ev.type == INPUT_KEY)
            return ev.key;
    }
}
