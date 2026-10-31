/*
 * input.h - keyboard and mouse events.
 *
 * Drivers push events into one queue; the shell or the GUI reads them.
 *
 *     struct input_event ev;
 *     if (input_wait(&ev, 100) == 0 && ev.type == INPUT_KEY)
 *         kprintf("key %c\n", ev.key);
 */
#ifndef INPUT_H
#define INPUT_H
#include "types.h"

/* Special keys (ordinary keys are plain ASCII: 'a', '\n', 27 for Esc ...) */
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_HOME    0x104
#define KEY_END     0x105
#define KEY_PGUP    0x106
#define KEY_PGDN    0x107
#define KEY_DELETE  0x108
#define KEY_INSERT  0x109
#define KEY_F1      0x110       /* F1..F12 = 0x110..0x11B */
#define KEY_F(n)    (KEY_F1 + (n) - 1)

#define MOD_SHIFT 0x1
#define MOD_CTRL  0x2
#define MOD_ALT   0x4

#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

enum input_type { INPUT_NONE = 0, INPUT_KEY, INPUT_MOUSE };

struct input_event {
    uint8_t  type;
    uint8_t  mods;          /* MOD_* (keys)                        */
    uint8_t  buttons;       /* MOUSE_* currently held (mouse)      */
    uint8_t  pad;           /* mouse: bit 0 = absolute position (dx,dy are 0..32767) */
    int32_t  key;           /* ASCII or KEY_* (keys)               */
    int32_t  dx, dy;        /* movement since the last event (mouse) */
};

void input_init(void);
void input_push(const struct input_event *ev);          /* from drivers (IRQ) */
bool input_poll(struct input_event *ev);                /* non-blocking       */
/* Wait up to timeout_ms (0 = forever). Returns 0, or -ETIMEDOUT. */
int  input_wait(struct input_event *ev, uint32_t timeout_ms);
void input_flush(void);
uint32_t input_dropped(void);

/* keyboard (src/drivers/keyboard.c) */
void keyboard_init(void);
/* Blocking: next key press as a character code. Mouse events are skipped. */
int  kbd_getchar(void);
/* mouse (src/drivers/mouse.c) */
bool mouse_init(void);
bool mouse_present(void);

/* Where text input comes from. The shell reads keys with console_getkey();
 * the GUI replaces it so a Terminal window receives its own keys. */
int  console_getkey(void);
void console_set_key_source(int (*fn)(void));

#endif
