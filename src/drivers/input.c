/* input.c - shared event queue between input drivers and consumers. */
#include "input.h"
#include "sync.h"
#include "sched.h"
#include "timer.h"
#include "io.h"
#include "kerror.h"

#define QSIZE 256

static struct input_event queue[QSIZE];
static volatile uint32_t head, tail;        /* head: next write, tail: next read */
static struct wait_queue waitq = WAIT_QUEUE_INIT(waitq);
static uint32_t dropped;

void input_init(void)
{
    head = tail = 0;
}

void input_push(const struct input_event *ev)
{
    uint32_t f = irq_save();
    uint32_t next = (head + 1) % QSIZE;

    if (next == tail) {                     /* full: drop the newest */
        dropped++;
    } else {
        queue[head] = *ev;
        head = next;
        wq_wake_all(&waitq);
    }
    irq_restore(f);
}

bool input_poll(struct input_event *ev)
{
    uint32_t f = irq_save();
    bool got = tail != head;

    if (got) {
        *ev = queue[tail];
        tail = (tail + 1) % QSIZE;
    }
    irq_restore(f);
    return got;
}

int input_wait(struct input_event *ev, uint32_t timeout_ms)
{
    uint32_t deadline = timer_ticks() + (timeout_ms + 9) / 10;

    for (;;) {
        if (input_poll(ev))
            return 0;
        if (timeout_ms && (int32_t)(timer_ticks() - deadline) >= 0)
            return -ETIMEDOUT;
        if (!sched_running() || task_current()->pid == 0) {
            cpu_hlt_wait();                 /* boot context: just idle */
            continue;
        }
        uint32_t f = irq_save();
        if (tail == head) {
            if (timeout_ms) {
                /* sleep until the deadline OR until an event wakes us */
                struct task *t = task_current();
                list_add_tail(&t->queue_node, &waitq.waiters);
                t->wake_tick = deadline;
                t->state = TASK_SLEEPING;
                schedule();
                /* we may have been woken by the timer: leave the wait list */
                if (!list_empty(&t->queue_node))
                    list_del(&t->queue_node);
            } else {
                wq_wait_locked(&waitq);
            }
        }
        irq_restore(f);
    }
}

void input_flush(void)
{
    uint32_t f = irq_save();
    tail = head;
    irq_restore(f);
}

uint32_t input_dropped(void) { return dropped; }

/* ---- console key source ---- */

static int (*key_source)(void);

void console_set_key_source(int (*fn)(void)) { key_source = fn; }

int console_getkey(void)
{
    return key_source ? key_source() : kbd_getchar();
}
