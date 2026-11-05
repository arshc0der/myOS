/* pit.c - 8253/8254 programmable interval timer. */
#include "timer.h"
#include "isr.h"
#include "io.h"
#include "sched.h"
#include "sys.h"

static volatile uint32_t ticks;
static void (*tick_hook)(void);

static void pit_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (tick_hook)
        tick_hook();
    sched_preempt();            /* time slice over? switch task */
}

void timer_init(void)
{
    uint32_t divisor = 1193182 / TIMER_HZ;

    outb(0x43, 0x36);                       /* channel 0, square wave */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)(divisor >> 8));
    irq_register(0, pit_irq);
}

uint32_t timer_ticks(void) { return ticks; }
uint32_t timer_ms(void) { return ticks * (1000 / TIMER_HZ); }
uint32_t timer_uptime_sec(void) { return ticks / TIMER_HZ; }
void timer_set_hook(void (*fn)(void)) { tick_hook = fn; }

void timer_sleep_ms(uint32_t ms)
{
    uint32_t until = ticks + (ms + (1000 / TIMER_HZ) - 1) / (1000 / TIMER_HZ);

    if (sched_running() && task_current()->pid != 0) {
        sched_sleep_until(until);
        return;
    }
    while ((int32_t)(until - ticks) > 0)
        cpu_hlt_wait();
}

/* PC speaker: PIT channel 2 square wave. Real hardware beeps; QEMU needs "-audiodev ... -machine pcspk-audiodev=...". */
void speaker_beep(uint32_t hz, uint32_t ms)
{
    if (sys_prefs.muted || sys_prefs.volume == 0 || hz < 40 || hz > 20000)
        return;
    uint32_t div = 1193182u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)(div >> 8));
    outb(0x61, inb(0x61) | 3);
    timer_sleep_ms(ms);
    outb(0x61, inb(0x61) & ~3);
}
