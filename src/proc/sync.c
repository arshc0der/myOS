/* sync.c - wait queues, mutexes, semaphores, spinlocks. */
#include "sync.h"
#include "sched.h"
#include "io.h"
#include "panic.h"

void wq_init(struct wait_queue *q) { list_init(&q->waiters); }
bool wq_empty(struct wait_queue *q) { return list_empty(&q->waiters); }

void wq_wait_locked(struct wait_queue *q)
{
    struct task *t = task_current();

    if (!sched_running())
        PANIC("blocking wait before the scheduler started");
    list_add_tail(&t->queue_node, &q->waiters);
    sched_block_current();
}

void wq_wake_one(struct wait_queue *q)
{
    uint32_t f = irq_save();

    if (!list_empty(&q->waiters)) {
        struct task *t = list_entry(q->waiters.next, struct task, queue_node);
        list_del(&t->queue_node);
        sched_make_ready(t);
    }
    irq_restore(f);
}

void wq_wake_all(struct wait_queue *q)
{
    uint32_t f = irq_save();

    while (!list_empty(&q->waiters)) {
        struct task *t = list_entry(q->waiters.next, struct task, queue_node);
        list_del(&t->queue_node);
        sched_make_ready(t);
    }
    irq_restore(f);
}

void mutex_init(struct mutex *m)
{
    m->locked = false;
    m->owner = NULL;
    wq_init(&m->wq);
}

void mutex_lock(struct mutex *m)
{
    uint32_t f = irq_save();

    while (m->locked)
        wq_wait_locked(&m->wq);
    m->locked = true;
    m->owner = task_current();
    irq_restore(f);
}

bool mutex_trylock(struct mutex *m)
{
    uint32_t f = irq_save();
    bool ok = !m->locked;

    if (ok) {
        m->locked = true;
        m->owner = task_current();
    }
    irq_restore(f);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    uint32_t f = irq_save();

    m->locked = false;
    m->owner = NULL;
    wq_wake_one(&m->wq);
    irq_restore(f);
}

void sem_init(struct semaphore *s, int initial)
{
    s->count = initial;
    wq_init(&s->wq);
}

void sem_down(struct semaphore *s)
{
    uint32_t f = irq_save();

    while (s->count <= 0)
        wq_wait_locked(&s->wq);
    s->count--;
    irq_restore(f);
}

bool sem_trydown(struct semaphore *s)
{
    uint32_t f = irq_save();
    bool ok = s->count > 0;

    if (ok)
        s->count--;
    irq_restore(f);
    return ok;
}

void sem_up(struct semaphore *s)
{
    uint32_t f = irq_save();

    s->count++;
    wq_wake_one(&s->wq);
    irq_restore(f);
}

void spin_lock(struct spinlock *l)
{
    uint32_t f = irq_save();

    if (l->held)
        PANIC("spinlock already held (deadlock on a single CPU)");
    l->flags = f;
    l->held = true;
}

void spin_unlock(struct spinlock *l)
{
    l->held = false;
    irq_restore(l->flags);
}
