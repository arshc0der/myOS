/*
 * timer.h - the system clock (PIT, 100 ticks per second).
 */
#ifndef TIMER_H
#define TIMER_H
#include "types.h"

#define TIMER_HZ 100

void     timer_init(void);
uint32_t timer_ticks(void);                 /* ticks since boot            */
uint32_t timer_ms(void);                    /* milliseconds since boot     */
uint32_t timer_uptime_sec(void);
/* Sleep (interrupts must be on). Inside a thread this lets others run. */
void     timer_sleep_ms(uint32_t ms);
/* Called on every tick from the interrupt handler (used by the scheduler). */
void     timer_set_hook(void (*fn)(void));

#endif
