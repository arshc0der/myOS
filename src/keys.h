/*
 * keys.h - special key codes returned by keyboard_getchar().
 *
 * Normal keys return their ASCII value (0-127).
 * Special keys (arrows, Page Up, ...) return values above 255 so they can
 * never be confused with a typed character.
 */
#ifndef KEYS_H
#define KEYS_H

#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103
#define KEY_PGUP    0x104
#define KEY_PGDN    0x105
#define KEY_HOME    0x106
#define KEY_END     0x107
#define KEY_DELETE  0x108

#endif
