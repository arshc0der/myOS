/*
 * sync.h - waiting and locking.
 *
 * wait_queue : a list of tasks sleeping until something happens.
 * mutex      : only one task at a time may hold it (others sleep).
 * semaphore  : a counter; down() waits if it is zero, up() adds one.
 * spinlock   : on this single-CPU kernel it simply turns interrupts off.
 */
#ifndef SYNC_H
#define SYNC_H
#include "types.h"
#include "list.h"

struct task;

struct wait_queue { struct list_head waiters; };
#define WAIT_QUEUE_INIT(n) { LIST_HEAD_INIT((n).waiters) }
void wq_init(struct wait_queue *q);
/* Put the current task to sleep on q. Interrupts must be OFF when called
 * (use irq_save first) so a wake-up cannot slip in before we sleep. */
void wq_wait_locked(struct wait_queue *q);
void wq_wake_one(struct wait_queue *q);
void wq_wake_all(struct wait_queue *q);
bool wq_empty(struct wait_queue *q);

/* Sleep until `cond` becomes true. Another task/IRQ must call wq_wake_*. */
#define WAIT_EVENT(q, cond) \
    do { \
        uint32_t _f = irq_save(); \
        while (!(cond)) \
            wq_wait_locked(&(q)); \
        irq_restore(_f); \
    } while (0)

struct mutex {
    bool locked;
    struct task *owner;
    struct wait_queue wq;
};
#define MUTEX_INIT(n) { false, NULL, WAIT_QUEUE_INIT((n).wq) }
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

struct semaphore {
    int count;
    struct wait_queue wq;
};
void sem_init(struct semaphore *s, int initial);
void sem_down(struct semaphore *s);            /* wait / P */
bool sem_trydown(struct semaphore *s);
void sem_up(struct semaphore *s);              /* signal / V */

struct spinlock { uint32_t flags; bool held; };
#define SPINLOCK_INIT { 0, false }
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

#endif
